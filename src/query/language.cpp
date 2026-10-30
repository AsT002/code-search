#include "codesearch/query/language.hpp"

#include <vector>

#include "codesearch/util/ascii.hpp"

namespace codesearch {

namespace {

struct Language {
    std::string_view name;                     // the main name
    std::vector<std::string_view> aliases;     // other names lang: accepts
    std::vector<std::string_view> extensions;  // with the dot, lowercase
    std::vector<std::string_view> fileNames;   // whole file names, exact case
};

// Sorted by name. A file gets the first language that claims it.
const std::vector<Language>& languages() {
    // static: built once, the first time this runs.
    static const std::vector<Language> table = {
        {"c", {}, {".c", ".h"}, {}},
        {"cmake", {}, {".cmake"}, {"CMakeLists.txt"}},
        {"cpp", {"c++", "cxx"}, {".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx"}, {}},
        {"csharp", {"c#", "cs"}, {".cs"}, {}},
        {"css", {}, {".css"}, {}},
        {"go", {"golang"}, {".go"}, {}},
        {"html", {}, {".htm", ".html"}, {}},
        {"java", {}, {".java"}, {}},
        {"javascript", {"js"}, {".cjs", ".js", ".jsx", ".mjs"}, {}},
        {"json", {}, {".json"}, {}},
        {"kotlin", {"kt"}, {".kt", ".kts"}, {}},
        {"make", {"makefile"}, {".mk"}, {"GNUmakefile", "Makefile", "makefile"}},
        {"markdown", {"md"}, {".markdown", ".md"}, {}},
        {"php", {}, {".php"}, {}},
        {"python", {"py"}, {".py", ".pyi"}, {}},
        {"ruby", {"rb"}, {".rb"}, {"Gemfile", "Rakefile"}},
        {"rust", {"rs"}, {".rs"}, {}},
        {"shell", {"bash", "sh", "zsh"}, {".bash", ".sh", ".zsh"}, {}},
        {"sql", {}, {".sql"}, {}},
        {"swift", {}, {".swift"}, {}},
        {"toml", {}, {".toml"}, {}},
        {"typescript", {"ts"}, {".ts", ".tsx"}, {}},
        {"yaml", {"yml"}, {".yaml", ".yml"}, {}},
    };
    return table;
}

std::string lowercase(std::string_view text) {
    std::string out;
    for (const char c : text) {
        out.push_back(foldAscii(c));
    }
    return out;
}

}  // namespace

std::optional<std::string> languageName(std::string_view typed) {
    const std::string wanted = lowercase(typed);
    for (const Language& language : languages()) {
        if (language.name == wanted) {
            return std::string(language.name);
        }
        for (const std::string_view alias : language.aliases) {
            if (alias == wanted) {
                return std::string(language.name);
            }
        }
    }
    return std::nullopt;
}

std::string_view languageOf(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::string_view fileName =
        slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = fileName.rfind('.');
    // ".bashrc" has no extension: a dot at the start just hides the file.
    const std::string extension =
        dot == std::string_view::npos || dot == 0 ? "" : lowercase(fileName.substr(dot));

    for (const Language& language : languages()) {
        for (const std::string_view name : language.fileNames) {
            if (name == fileName) {
                return language.name;
            }
        }
        for (const std::string_view known : language.extensions) {
            if (known == extension) {
                return language.name;
            }
        }
    }
    return "";
}

std::string knownLanguages() {
    std::string out;
    for (const Language& language : languages()) {
        out += out.empty() ? "" : ", ";
        out += language.name;
    }
    return out;
}

}  // namespace codesearch
