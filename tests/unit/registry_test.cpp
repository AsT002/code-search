#include "codesearch/repo/registry.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../support/temp_dir.hpp"
#include "../support/test_ids.hpp"

namespace codesearch {
namespace {

using testing::TempDir;
using testing::writeFile;

RepoEntry sampleRepo(const std::string& id, const std::string& name, RepoSource source) {
    return RepoEntry{
        .id = id,
        .name = name,
        .source = source,
        .location =
            source == RepoSource::Local ? "/home/me/" + name : "https://github.com/example/" + name,
        .index =
            IndexInfo{
                .file = indexFileName(id, 3),
                .commit = testing::fakeObjectId('a'),
                .formatVersion = 1,
                .policyVersion = 2,
                .configFingerprint = 0x0123456789abcdefULL,
            },
    };
}

Registry sampleRegistry() {
    Registry registry;
    registry.repos.push_back(sampleRepo("r_0000beef", "backend", RepoSource::Local));
    registry.repos.push_back(sampleRepo("r_1234abcd", "frontend", RepoSource::GitHub));
    return registry;
}

void expectSame(const Registry& a, const Registry& b) {
    ASSERT_EQ(a.repos.size(), b.repos.size());
    for (std::size_t i = 0; i < a.repos.size(); ++i) {
        const RepoEntry& x = a.repos[i];
        const RepoEntry& y = b.repos[i];
        EXPECT_EQ(x.id, y.id);
        EXPECT_EQ(x.name, y.name);
        EXPECT_EQ(x.source, y.source);
        EXPECT_EQ(x.location, y.location);
        EXPECT_EQ(x.index.file, y.index.file);
        EXPECT_EQ(x.index.commit, y.index.commit);
        EXPECT_EQ(x.index.formatVersion, y.index.formatVersion);
        EXPECT_EQ(x.index.policyVersion, y.index.policyVersion);
        EXPECT_EQ(x.index.configFingerprint, y.index.configFingerprint);
    }
}

TEST(Registry, JsonRoundTrip) {
    const Registry registry = sampleRegistry();
    const std::string json = registryToJson(registry);
    expectSame(registryFromJson(json), registry);
    EXPECT_NE(json.find("\"config_fingerprint\": \"0123456789abcdef\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"file\": \"r_0000beef-g0003.idx\""), std::string::npos) << json;
}

TEST(Registry, LoadSortsByName) {
    Registry registry;
    registry.repos.push_back(sampleRepo("r_00000002", "zeta", RepoSource::Local));
    registry.repos.push_back(sampleRepo("r_00000001", "alpha", RepoSource::Local));
    const Registry loaded = registryFromJson(registryToJson(registry));
    EXPECT_EQ(loaded.repos[0].name, "alpha");
    EXPECT_EQ(loaded.repos[1].name, "zeta");
    EXPECT_NE(loaded.find("zeta"), nullptr);
    EXPECT_EQ(loaded.find("beta"), nullptr);
}

TEST(Registry, StoreSavesAndLoads) {
    const TempDir dir;
    JsonRegistryStore store(dir.file("registry.json"));
    EXPECT_TRUE(store.load().repos.empty());  // no file yet
    store.save(sampleRegistry());
    expectSame(store.load(), sampleRegistry());
}

TEST(Registry, DamagedFileNamesTheFile) {
    const TempDir dir;
    writeFile(dir.file("registry.json"), "{ not json");
    try {
        JsonRegistryStore(dir.file("registry.json")).load();
        FAIL() << "expected an error";
    } catch (const RegistryError& error) {
        EXPECT_NE(std::string(error.what()).find("registry.json is damaged"), std::string::npos)
            << error.what();
    }
}

// Replaces the first `from` in the sample registry's JSON with `to`.
std::string sampleWith(const std::string& from, const std::string& to) {
    std::string json = registryToJson(sampleRegistry());
    const std::size_t at = json.find(from);
    EXPECT_NE(at, std::string::npos) << from;
    return json.replace(at, from.size(), to);
}

std::string sameIdTwice() {
    Registry registry;
    registry.repos.push_back(sampleRepo("r_00000001", "one", RepoSource::Local));
    registry.repos.push_back(sampleRepo("r_00000001", "two", RepoSource::Local));
    return registryToJson(registry);
}

TEST(Registry, RejectsBadContents) {
    struct Case {
        std::string json;
        std::string message;
    };
    const std::vector<Case> cases = {
        {"", "not valid JSON"},
        {"[]", "missing \"version\""},
        {R"({"version": 2, "repos": []})", "version 2"},
        {R"({"version": -1, "repos": []})", "\"version\" must be a whole number"},
        {R"({"version": 1.5, "repos": []})", "\"version\" must be a whole number"},
        {R"({"version": 1, "repos": {}})", "must be a list"},
        {R"({"version": 1})", "missing \"repos\""},
        {sampleWith("\"r_0000beef\"", "\"r_0000BEEF\""), "bad repo ID"},
        {sampleWith("\"r_0000beef\"", "\"../../etc\""), "bad repo ID"},
        {sampleWith("\"backend\"", "\"-backend\""), "bad repo name"},
        {sampleWith("\"backend\"", "\"frontend\""), "used twice"},
        {sameIdTwice(), "repo ID r_00000001 is used twice"},
        {sampleWith("\"local\"", "\"ftp\""), "unknown source"},
        {sampleWith("\"/home/me/backend\"", "\"\""), "no location"},
        {sampleWith(R"("r_0000beef-g0003.idx")", R"("../x-g0003.idx")"), "bad index file name"},
        {sampleWith("\"r_0000beef-g0003.idx\"", "\"r_1234abcd-g0003.idx\""), "bad index file name"},
        {sampleWith("\"r_0000beef-g0003.idx\"", "\"r_0000beef-g3.idx\""), "bad index file name"},
        {sampleWith("\"0123456789abcdef\"", "\"0123\""), "config_fingerprint"},
        {sampleWith("\"0123456789abcdef\"", "\"0123456789ABCDEF\""), "config_fingerprint"},
        {sampleWith(std::string(40, 'a'), "zz"), "\"commit\" must be"},
        {sampleWith(R"("policy_version": 2)", R"("policy_version": "2")"), "policy_version"},
        {sampleWith(R"("policy_version": 2)", R"("policy_version": 4294967296)"), "policy_version"},
        {sampleWith(R"("name": "backend",)", ""), R"(missing "name")"},
    };
    for (const Case& c : cases) {
        try {
            registryFromJson(c.json);
            ADD_FAILURE() << "expected an error for: " << c.json;
        } catch (const RegistryError& error) {
            EXPECT_NE(std::string(error.what()).find(c.message), std::string::npos)
                << "got: " << error.what() << "\nwanted: " << c.message;
        }
    }
}

TEST(Registry, NamesAndFileNames) {
    EXPECT_TRUE(isValidRepoName("my-backend_2.0"));
    EXPECT_FALSE(isValidRepoName(""));
    EXPECT_FALSE(isValidRepoName("-x"));
    EXPECT_FALSE(isValidRepoName(".x"));
    EXPECT_FALSE(isValidRepoName("a b"));
    EXPECT_FALSE(isValidRepoName("a/b"));
    EXPECT_FALSE(isValidRepoName("a:b"));
    EXPECT_FALSE(isValidRepoName(std::string(101, 'a')));
    EXPECT_EQ(indexFileName("r_00000001", 7), "r_00000001-g0007.idx");
    EXPECT_EQ(indexFileName("r_00000001", 12345), "r_00000001-g12345.idx");
}

TEST(Registry, NextIndexFileName) {
    EXPECT_EQ(nextIndexFileName("r_00000001", ""), "r_00000001-g0001.idx");
    EXPECT_EQ(nextIndexFileName("r_00000001", "r_00000001-g0001.idx"), "r_00000001-g0002.idx");
    EXPECT_EQ(nextIndexFileName("r_00000001", "r_00000001-g9999.idx"), "r_00000001-g10000.idx");
    EXPECT_EQ(nextIndexFileName("r_00000001", "r_00000001-g4294967294.idx"),
              "r_00000001-g4294967295.idx");
    EXPECT_THROW(nextIndexFileName("r_00000001", "r_00000001-g4294967295.idx"), RegistryError);
    EXPECT_THROW(nextIndexFileName("r_00000001", "r_00000001-g99999999999.idx"), RegistryError);
    EXPECT_THROW(nextIndexFileName("r_00000001", "r_00000002-g0001.idx"), RegistryError);
}

TEST(Registry, NewIdsAreUnusedAndWellFormed) {
    Registry registry;
    for (int i = 0; i < 50; ++i) {
        const RepoEntry repo =
            sampleRepo(newRepoId(registry), "n" + std::to_string(i), RepoSource::Local);
        EXPECT_EQ(registry.find(repo.name), nullptr);
        for (const RepoEntry& other : registry.repos) {
            EXPECT_NE(other.id, repo.id);
        }
        registry.repos.push_back(repo);
    }
    // Every ID passes the checks on load.
    EXPECT_NO_THROW(registryFromJson(registryToJson(registry)));
}

}  // namespace
}  // namespace codesearch
