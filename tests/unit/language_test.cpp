#include "codesearch/query/language.hpp"

#include <gtest/gtest.h>

namespace codesearch {
namespace {

TEST(Language, NamesAndAliases) {
    EXPECT_EQ(languageName("go"), "go");
    EXPECT_EQ(languageName("GoLang"), "go");
    EXPECT_EQ(languageName("c++"), "cpp");
    EXPECT_EQ(languageName("TS"), "typescript");
    EXPECT_FALSE(languageName("cobol").has_value());
    EXPECT_FALSE(languageName("").has_value());
    EXPECT_TRUE(knownLanguages().starts_with("c, cmake, cpp, "));
}

TEST(Language, FromTheFileName) {
    EXPECT_EQ(languageOf("src/main.go"), "go");
    EXPECT_EQ(languageOf("lib/a.test.JS"), "javascript");  // extensions ignore case
    EXPECT_EQ(languageOf("include/x.h"), "c");
    EXPECT_EQ(languageOf("include/x.hpp"), "cpp");
    EXPECT_EQ(languageOf("tools/Makefile"), "make");
    EXPECT_EQ(languageOf("CMakeLists.txt"), "cmake");
    EXPECT_EQ(languageOf("notes.txt"), "");
    EXPECT_EQ(languageOf("README"), "");
    EXPECT_EQ(languageOf(".bashrc"), "");  // a leading dot isn't an extension
    EXPECT_EQ(languageOf("dir.go/file"), "");
    EXPECT_EQ(languageOf("makefile.txt"), "");
}

}  // namespace
}  // namespace codesearch
