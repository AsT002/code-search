#include "codesearch/search/json_output.hpp"

#include <nlohmann/json.hpp>

#include "codesearch/util/utf8.hpp"

namespace codesearch {

namespace {

// ordered_json keeps keys in the order we add them, so the output is stable.
using Json = nlohmann::ordered_json;

// Standard base64 (RFC 4648), with '=' padding.
std::string base64(std::string_view bytes) {
    constexpr std::string_view kDigits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    // Each 3 bytes (24 bits) become 4 digits of 6 bits each.
    while (i < bytes.size()) {
        const std::size_t count = std::min<std::size_t>(3, bytes.size() - i);
        std::uint32_t group = 0;
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t byte = k < count ? static_cast<unsigned char>(bytes[i + k]) : 0U;
            group = (group << 8U) | byte;
        }
        for (std::size_t k = 0; k < 4; ++k) {
            if (k <= count) {
                out.push_back(kDigits[(group >> (18U - (6U * k))) & 0x3FU]);
            } else {
                out.push_back('=');
            }
        }
        i += count;
    }
    return out;
}

// Adds `key` with `text`, plus `key`_base64 if `text` isn't valid UTF-8.
void addText(Json& object, const std::string& key, std::string_view text) {
    if (isValidUtf8(text)) {
        object[key] = std::string(text);
        return;
    }
    object[key] = replaceInvalidUtf8(text);
    object[key + "_base64"] = base64(text);
}

}  // namespace

std::string_view statusName(SearchStatus status) {
    switch (status) {
        case SearchStatus::Complete:
            return "complete";
        case SearchStatus::Truncated:
            return "truncated";
        case SearchStatus::Cancelled:
            return "cancelled";
    }
    return "complete";  // every status returned above
}

std::string resultFileJson(const ResultFile& file) {
    Json object = Json::object();
    addText(object, "repo", file.repo);
    addText(object, "path", file.path);
    Json lines = Json::array();
    for (const ResultLine& line : file.lines) {
        Json entry = Json::object();
        entry["line"] = line.number;
        addText(entry, "text", line.text);
        Json matches = Json::array();
        for (const Span& span : line.matches) {
            matches.push_back(Json{{"start", span.begin}, {"end", span.end}});
        }
        entry["matches"] = std::move(matches);
        lines.push_back(std::move(entry));
    }
    object["lines"] = std::move(lines);
    return object.dump();
}

JsonResultWriter::JsonResultWriter(std::ostream& out) : out_(&out) {
    *out_ << R"({"files":[)";
}

void JsonResultWriter::file(const ResultFile& file) {
    if (!first_) {
        *out_ << ',';
    }
    first_ = false;
    *out_ << '\n' << resultFileJson(file);
}

void JsonResultWriter::finish(SearchStatus status) {
    *out_ << (first_ ? "" : "\n") << R"(],"status":")" << statusName(status) << "\"}\n";
}

}  // namespace codesearch
