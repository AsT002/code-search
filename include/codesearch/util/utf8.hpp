#pragma once

#include <string>
#include <string_view>

namespace codesearch {

// True if `text` is valid UTF-8: every character is 1 to 4 bytes in the
// shortest form, with no UTF-16 surrogates (U+D800 to U+DFFF) and nothing
// above U+10FFFF. Empty text is valid.
bool isValidUtf8(std::string_view text);

// `text` with each byte that isn't part of a valid character replaced by
// U+FFFD, the replacement character. Valid text comes back unchanged.
std::string replaceInvalidUtf8(std::string_view text);

// `text` made safe to print on a terminal: each byte that could control it
// is shown as \xHH instead. That's the ASCII control bytes except tab
// (ESC starts escape sequences), DEL, the C1 controls U+0080 to U+009F, and
// bytes that aren't part of a valid UTF-8 character (a lone 0x9B starts an
// escape sequence on some terminals). Everything else is unchanged.
std::string escapeForTerminal(std::string_view text);

}  // namespace codesearch
