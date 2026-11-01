#include "codesearch/search/json_output.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

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

}  // namespace
}  // namespace codesearch
