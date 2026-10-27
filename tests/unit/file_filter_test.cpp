#include "codesearch/index/file_filter.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace codesearch {
namespace {

TEST(FileFilter, SkipsExcludedPatternsAnywhere) {
    const FileFilter filter;
    EXPECT_EQ(filter.checkEntry("app.min.js", 10), SkipReason::Excluded);
    EXPECT_EQ(filter.checkEntry("web/static/app.min.js", 10), SkipReason::Excluded);
    EXPECT_EQ(filter.checkEntry("app.js", 10), std::nullopt);
    EXPECT_EQ(filter.checkEntry("package-lock.json", 10), std::nullopt);  // not by name
}

TEST(FileFilter, SkipsFilesOverTheLimit) {
    FilterConfig config;
    config.maxFileBytes = 100;
    const FileFilter filter(config);
    EXPECT_EQ(filter.checkEntry("a.txt", 100), std::nullopt);
    EXPECT_EQ(filter.checkEntry("a.txt", 101), SkipReason::TooLarge);
}

TEST(FileFilter, ChecksContent) {
    EXPECT_EQ(FileFilter::checkContent("plain text\n"), std::nullopt);
    EXPECT_EQ(FileFilter::checkContent(std::string("a\0b", 3)), SkipReason::Binary);
}

// A valid pointer, copied from the format in the LFS spec.
constexpr std::string_view kPointer =
    "version https://git-lfs.github.com/spec/v1\n"
    "oid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\n"
    "size 12345\n";

TEST(IsLfsPointer, AcceptsValidPointers) {
    EXPECT_EQ(FileFilter::checkContent(kPointer), SkipReason::LfsPointer);
    // Extra keys are allowed if all keys after "version" stay sorted.
    const std::string withExtension =
        "version https://git-lfs.github.com/spec/v1\n"
        "ext-0-foo sha256:0000000000000000000000000000000000000000000000000000000000000000\n"
        "oid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\n"
        "size 0\n";
    EXPECT_TRUE(isLfsPointer(withExtension));
    // The older version name the spec still accepts.
    std::string legacy(kPointer);
    legacy.replace(0, legacy.find('\n'), "version https://hawser.github.com/spec/v1");
    EXPECT_TRUE(isLfsPointer(legacy));
}

TEST(IsLfsPointer, KeepsTextThatOnlyLooksLikeOne) {
    // A document about LFS that starts with the same line is a real file.
    EXPECT_FALSE(
        isLfsPointer("version https://git-lfs.github.com/spec/v1\n"
                     "This line explains what the version line means.\n"));
    EXPECT_FALSE(isLfsPointer("version https://git-lfs.github.com/spec/v1\n"));
}

// Each of these breaks one rule, so it's a normal file that must be indexed.
TEST(IsLfsPointer, RejectsBadExtensionValuesAndRepeatedVersion) {
    const std::string version = "version https://git-lfs.github.com/spec/v1\n";
    const std::string oid =
        "oid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\n";
    const std::string size = "size 12345\n";

    // Values must not contain a carriage return, in any key.
    EXPECT_FALSE(isLfsPointer(version + "ext-0-foo sha256:00\r\n" + oid + size));
    EXPECT_FALSE(isLfsPointer(version + oid + size + "x-note a\rb\n"));
    EXPECT_FALSE(isLfsPointer(version + oid + "size 12345\r\n"));

    // "version" comes first and only once.
    EXPECT_FALSE(isLfsPointer(version + version + oid + size));
    EXPECT_FALSE(isLfsPointer(version + oid + size + version));

    // Size 0 is fine.
    EXPECT_TRUE(isLfsPointer(version + oid + "size 0\n"));
}

TEST(IsLfsPointer, RejectsBrokenPointers) {
    const std::string version = "version https://git-lfs.github.com/spec/v1\n";
    const std::string oid =
        "oid sha256:4d7a214614ab2935c943f9e0ff69d22eadbb8f32b1258daaa5e2ca24d17e2393\n";
    const std::string size = "size 12345\n";

    EXPECT_FALSE(isLfsPointer(version + oid));                        // no size
    EXPECT_FALSE(isLfsPointer(version + size));                       // no oid
    EXPECT_FALSE(isLfsPointer(version + size + oid));                 // not sorted
    EXPECT_FALSE(isLfsPointer(oid + version + size));                 // version not first
    EXPECT_FALSE(isLfsPointer(version + oid + oid + size));           // key twice
    EXPECT_FALSE(isLfsPointer(version + oid + "size 12a\n"));         // size not a number
    EXPECT_FALSE(isLfsPointer(version + oid + "size \n"));            // empty size
    EXPECT_FALSE(isLfsPointer(version + "oid sha256:abc\n" + size));  // short hash
    EXPECT_FALSE(isLfsPointer(version + "oid md5:abc\n" + size));     // other hash
    EXPECT_FALSE(isLfsPointer("version https://git-lfs.github.com/spec/v2\n" + oid + size));
    EXPECT_FALSE(isLfsPointer(version + oid + "size 12345"));        // no final newline
    EXPECT_FALSE(isLfsPointer(version + oid + "Size 12345\n"));      // uppercase key
    EXPECT_FALSE(isLfsPointer(version + oid + size + "nospace\n"));  // not "key value"
    std::string upperHash = oid;
    upperHash[12] = 'D';
    EXPECT_FALSE(isLfsPointer(version + upperHash + size));
    EXPECT_FALSE(isLfsPointer(version + oid + "size 012345\n"));  // leading zero
    EXPECT_FALSE(isLfsPointer(version + oid + "size  12345\n"));  // two spaces
    EXPECT_FALSE(
        isLfsPointer(std::string(kPointer) + "x-pad " + std::string(1100, 'x') + "\n"));  // too big
}

TEST(LooksBinary, OnlyLooksAtTheFirst8KB) {
    std::string late(9000, 'a');
    late[8191] = '\0';
    EXPECT_TRUE(looksBinary(late));
    late[8191] = 'a';
    late[8192] = '\0';
    EXPECT_FALSE(looksBinary(late));
    EXPECT_FALSE(looksBinary(""));
}

TEST(ConfigFingerprint, ChangesWithEverySetting) {
    const FilterConfig base;
    EXPECT_EQ(configFingerprint(base), configFingerprint(FilterConfig{}));

    FilterConfig bigger = base;
    bigger.maxFileBytes += 1;
    EXPECT_NE(configFingerprint(bigger), configFingerprint(base));

    FilterConfig morePatterns = base;
    morePatterns.excludePatterns.push_back("*.lock");
    EXPECT_NE(configFingerprint(morePatterns), configFingerprint(base));

    // Lengths are part of the hash, so splitting a pattern differently counts.
    FilterConfig ab;
    ab.excludePatterns = {"ab"};
    FilterConfig aAndB;
    aAndB.excludePatterns = {"a", "b"};
    EXPECT_NE(configFingerprint(ab), configFingerprint(aAndB));
}

}  // namespace
}  // namespace codesearch
