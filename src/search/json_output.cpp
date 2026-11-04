#include "codesearch/search/json_output.hpp"

#include <array>
#include <charconv>
#include <string>

#include "codesearch/util/utf8.hpp"

namespace codesearch {

namespace {

void appendHexEscape(std::string& out, unsigned code) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    out += "\\u00";
    out += kDigits[code >> 4U];
    out += kDigits[code & 0xFU];
}

// Whether byte `i` of `text` starts something appendString escapes.
bool needsEscape(std::string_view text, std::size_t i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x20 || c == '"' || c == '\\' || c == 0x7F) {
        return true;
    }
    // C2 80 to C2 9F: the C1 controls.
    return c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) <= 0x9F;
}

// Appends `text`, which must be valid UTF-8, as a JSON string, quotes and
// all. It escapes what JSON needs: '"', '\\' and characters below U+0020.
// It also escapes DEL and the C1 controls (U+0080 to U+009F): JSON allows
// them raw, but raw they can control a terminal the output is shown on.
//
// Most text needs no escaping, so the bytes between escapes are copied in
// one append each, not one at a time: this runs for every result line.
//
// Written by hand instead of with nlohmann/json: its values can allocate
// while being destroyed, where running out of memory can't be reported, and
// a result must be safe to drop halfway (see JsonResultWriter::file).
void appendString(std::string& out, std::string_view text) {
    out += '"';
    std::size_t plain = 0;  // where the bytes not added yet start
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!needsEscape(text, i)) {
            continue;
        }
        out.append(text.substr(plain, i - plain));
        const auto c = static_cast<unsigned char>(text[i]);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case 0xC2:  // a C1 control: the code is its second byte
                appendHexEscape(out, static_cast<unsigned char>(text[i + 1]));
                ++i;
                break;
            default:  // the other controls, and DEL
                appendHexEscape(out, c);
        }
        plain = i + 1;
    }
    out.append(text.substr(plain));
    out += '"';
}

// Appends a number in decimal. std::to_chars writes it into a buffer on
// the stack, so unlike std::to_string it allocates nothing.
void appendNumber(std::string& out, std::size_t value) {
    std::array<char, 20> digits{};  // enough for any 64-bit number
    const std::to_chars_result result =
        std::to_chars(digits.data(), digits.data() + digits.size(), value);
    out.append(digits.data(), result.ptr);
}

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

// Appends "key":"text", plus "key_base64":"..." if `text` isn't valid
// UTF-8. Then the text has U+FFFD for each bad byte.
void appendText(std::string& out, std::string_view key, std::string_view text) {
    out += '"';
    out += key;
    out += "\":";
    if (isValidUtf8(text)) {
        appendString(out, text);
        return;
    }
    appendString(out, replaceInvalidUtf8(text));
    out += ",\"";
    out += key;
    out += "_base64\":\"";
    out += base64(text);
    out += '"';
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
    // Room for the text and about 64 bytes of keys and numbers per line, so
    // the string rarely has to grow (and copy itself) on the way.
    std::size_t size = 64 + file.repo.size() + file.path.size();
    for (const ResultLine& line : file.lines) {
        size += 64 + line.text.size() + (32 * line.matches.size());
    }
    std::string out;
    out.reserve(size);
    out += '{';
    appendText(out, "repo", file.repo);
    out += ',';
    appendText(out, "path", file.path);
    out += ",\"lines\":[";
    for (std::size_t l = 0; l < file.lines.size(); ++l) {
        const ResultLine& line = file.lines[l];
        out += l == 0 ? "{\"line\":" : ",{\"line\":";
        appendNumber(out, line.number);
        out += ',';
        appendText(out, "text", line.text);
        out += ",\"matches\":[";
        for (std::size_t m = 0; m < line.matches.size(); ++m) {
            out += m == 0 ? "{\"start\":" : ",{\"start\":";
            appendNumber(out, line.matches[m].begin);
            out += ",\"end\":";
            appendNumber(out, line.matches[m].end);
            out += '}';
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

JsonResultWriter::JsonResultWriter(std::ostream& out) : out_(&out) {
    *out_ << R"({"files":[)";
}

void JsonResultWriter::file(const ResultFile& file) {
    // Made before anything is written: if this throws (out of memory),
    // there's no comma waiting for a file that never comes, and fail() can
    // still end the document as valid JSON.
    const std::string json = resultFileJson(file);
    *out_ << (first_ ? "\n" : ",\n") << json;
    first_ = false;
}

void JsonResultWriter::skipped(std::string_view repo, std::string_view message) {
    std::string entry = skipped_.empty() ? "{" : ",{";
    appendText(entry, "repo", repo);
    entry += ",\"error\":";
    appendString(entry, replaceInvalidUtf8(message));
    entry += '}';
    skipped_ += entry;
}

void JsonResultWriter::warning(std::string_view message) {
    if (!warnings_.empty()) {
        warnings_ += ',';
    }
    appendString(warnings_, replaceInvalidUtf8(message));
}

void JsonResultWriter::endFiles() {
    *out_ << (first_ ? "" : "\n") << "]";
    if (!skipped_.empty()) {
        *out_ << ",\"skipped\":[" << skipped_ << "]";
    }
    if (!warnings_.empty()) {
        *out_ << ",\"warnings\":[" << warnings_ << "]";
    }
}

void JsonResultWriter::finish(SearchStatus status) {
    endFiles();
    *out_ << R"(,"status":")" << statusName(status) << "\"}\n";
}

void JsonResultWriter::fail(std::string_view message) {
    std::string text;
    appendString(text, replaceInvalidUtf8(message));
    endFiles();
    *out_ << R"(,"status":"error","error":)" << text << "}\n";
}

}  // namespace codesearch
