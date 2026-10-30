#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace codesearch {

// The main name of the language the user wrote after lang:, like "cpp" for
// "c++" or "CPP". Nothing if we don't know that language.
std::optional<std::string> languageName(std::string_view typed);

// The language of a file, worked out from its name: its extension (".go"),
// or the whole name for files like "Makefile". "" if we don't know it.
std::string_view languageOf(std::string_view path);

// Every main language name, for error messages: "c, cpp, css, ...".
std::string knownLanguages();

}  // namespace codesearch
