#include "codesearch/search/json_output.hpp"

#include "../support/allocations.hpp"
#include "codesearch/util/utf8.hpp"

#include <gtest/gtest.h>

#include <array>
#include <new>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>

namespace codesearch {
namespace {

TEST(JsonOutput, OneFile) {
    const ResultFile file{
        .repo = "demo",
        .path = "src/a.txt",
        .lines =
            {
                {.number = 1, .text = "before", .matches = {}},
                {.number = 2, .text = "say \"hi\"\tnow", .matches = {{4, 8}, {9, 12}}},
            },
    };
    EXPECT_EQ(resultFileJson(file),
              R"({"repo":"demo","path":"src/a.txt","lines":[)"
              R"({"line":1,"text":"before","matches":[]},)"
              R"({"line":2,"text":"say \"hi\"\tnow","matches":[{"start":4,"end":8},)"
              R"({"start":9,"end":12}]}]})");
}

TEST(JsonOutput, BadUtf8KeepsTheExactBytes) {
    const ResultFile file{
        .repo = "",
        .path = "caf\xE9.txt",  // Latin-1, not UTF-8
        .lines =
            {
                {
                    .number = 3,
                    .text = "\xFF\xFE"
                            "ab",
                    .matches = {{2, 4}},
                },
            },
    };
    EXPECT_EQ(resultFileJson(file),
              "{\"repo\":\"\",\"path\":\"caf\xEF\xBF\xBD.txt\",\"path_base64\":\"Y2Fm6S50eHQ=\","
              "\"lines\":[{\"line\":3,\"text\":\"\xEF\xBF\xBD\xEF\xBF\xBD"
              "ab\","
              "\"text_base64\":\"//5hYg==\",\"matches\":[{\"start\":2,\"end\":4}]}]}");
}

TEST(JsonOutput, Base64Padding) {
    // 1, 2 and 3 bytes need 2, 1 and 0 '=' signs.
    const auto encoded = [](const std::string& path) {
        return resultFileJson(ResultFile{.repo = "", .path = path, .lines = {}});
    };
    EXPECT_NE(encoded("\xFF").find("\"path_base64\":\"/w==\""), std::string::npos);
    EXPECT_NE(encoded("\xFF\xFF").find("\"path_base64\":\"//8=\""), std::string::npos);
    EXPECT_NE(encoded("\xFF\xFF\xFF").find("\"path_base64\":\"////\""), std::string::npos);
}

TEST(JsonOutput, WholeDocument) {
    std::ostringstream out;
    {
        JsonResultWriter writer(out);
        writer.finish(SearchStatus::Complete);
    }
    EXPECT_EQ(out.str(), "{\"files\":[],\"status\":\"complete\"}\n");

    out.str("");
    JsonResultWriter writer(out);
    writer.file(ResultFile{.repo = "a", .path = "x", .lines = {}});
    writer.file(ResultFile{.repo = "b", .path = "y", .lines = {}});
    writer.finish(SearchStatus::Truncated);
    EXPECT_EQ(out.str(),
              "{\"files\":[\n"
              "{\"repo\":\"a\",\"path\":\"x\",\"lines\":[]},\n"
              "{\"repo\":\"b\",\"path\":\"y\",\"lines\":[]}\n"
              "],\"status\":\"truncated\"}\n");
    EXPECT_EQ(statusName(SearchStatus::Cancelled), "cancelled");
}

TEST(JsonResultWriter, AnErrorStillEndsTheDocument) {
    std::ostringstream out;
    JsonResultWriter writer(out);
    writer.file(ResultFile{.repo = "r", .path = "a.txt", .lines = {}});
    writer.fail("r: index file is damaged: \"x\" \xff");
    EXPECT_EQ(
        out.str(),
        "{\"files\":[\n{\"repo\":\"r\",\"path\":\"a.txt\",\"lines\":[]}\n"
        "],\"status\":\"error\",\"error\":\"r: index file is damaged: \\\"x\\\" \xef\xbf\xbd\"}\n");
}

// JSON may hold DEL and the C1 controls raw, but on a terminal they can
// control it, so they're written as \u escapes, which mean the same text.
TEST(Json, TerminalControlsAreEscaped) {
    const ResultFile file{
        .repo = "r",
        .path = "a.txt",
        .lines =
            {
                ResultLine{
                    .number = 1,
                    .text = "x\x7f \xc2\x9b"
                            "31m \xc2\xa0",
                    .matches = {},
                },
            },
    };
    const std::string json = resultFileJson(file);
    // U+00A0 (C2 A0) is past the C1 range, so it stays as it is.
    EXPECT_NE(json.find("\"text\":\"x\\u007f \\u009b31m \xc2\xa0\""), std::string::npos) << json;
}

// A stream into a fixed buffer, so writing to it never allocates.
class FixedBuffer : public std::streambuf {
public:
    FixedBuffer() { setp(buffer_.data(), buffer_.data() + buffer_.size()); }
    std::string text() const { return {pbase(), pptr()}; }

private:
    std::array<char, std::size_t{1} << 16U> buffer_{};
};

// If making a file's JSON fails (memory runs out), fail() must still end
// the document as valid JSON: no comma before a file that never came.
TEST(JsonResultWriter, AFileThatFailsLeavesValidJson) {
    const ResultFile first{.repo = "r", .path = "a.txt", .lines = {}};
    const ResultFile second{
        .repo = "r",
        .path = "b.txt",
        .lines = {ResultLine{.number = 1, .text = "some text", .matches = {{0, 4}}}},
    };
    gMainThread = std::this_thread::get_id();
    for (long failAt = 1; failAt <= 60; ++failAt) {
        SCOPED_TRACE(failAt);
        FixedBuffer buffer;
        std::ostream out(&buffer);
        JsonResultWriter writer(out);
        writer.file(first);
        // Only allocations on another thread fail, so do it on one.
        std::thread([&] {
            gFailIn = failAt;
            try {
                writer.file(second);
            } catch (const std::bad_alloc&) {  // NOLINT(bugprone-empty-catch) what's tested
            }
            gFailIn = 0;
        }).join();
        writer.fail("out of memory");
        EXPECT_TRUE(nlohmann::json::accept(buffer.text())) << buffer.text();
    }
}

// The JSON is written by hand, so check it against a real JSON parser: any
// bytes in a path or line must come back as the same text (with U+FFFD for
// bytes that aren't valid UTF-8, whose exact bytes are in *_base64).
TEST(Json, RandomTextReadsBack) {
    // A fixed seed, so a failure comes back the same way.
    std::mt19937 rng(11);  // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> length(0, 12);
    for (int round = 0; round < 3000; ++round) {
        std::string text;
        for (int n = length(rng); n > 0; --n) {
            text.push_back(static_cast<char>(byte(rng)));
        }
        const ResultFile file{
            .repo = "r",
            .path = text,
            .lines = {ResultLine{.number = 7, .text = text, .matches = {{0, 1}}}},
        };
        const nlohmann::json read = nlohmann::json::parse(resultFileJson(file));
        ASSERT_EQ(read["path"], replaceInvalidUtf8(text)) << ::testing::PrintToString(text);
        ASSERT_EQ(read["lines"][0]["text"], replaceInvalidUtf8(text));
        ASSERT_EQ(read["lines"][0]["line"], 7);
        ASSERT_EQ(read.contains("path_base64"), !isValidUtf8(text));
    }
}

}  // namespace
}  // namespace codesearch
