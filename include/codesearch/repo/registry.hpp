#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/util/object_id.hpp"

namespace codesearch {

// Thrown when registry.json can't be read or doesn't make sense.
class RegistryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class RepoSource {
    Local,   // a repo on this machine; we never change or delete it
    GitHub,  // a clone we own, in the workspace's repos/ folder
};

// The current index file of a repo, and what it was built from.
struct IndexInfo {
    std::string file;  // a name inside indexes/, like "r_1c2d3e4f-g0001.idx"
    ObjectId commit;
    std::uint32_t formatVersion = 0;
    std::uint32_t policyVersion = 0;
    std::uint64_t configFingerprint = 0;
};

struct RepoEntry {
    std::string id;    // internal, like "r_1c2d3e4f"; used for file names
    std::string name;  // the user's label; never used for file names
    RepoSource source = RepoSource::Local;
    std::string location;  // the folder for Local, the URL for GitHub
    IndexInfo index;
};

struct Registry {
    std::vector<RepoEntry> repos;  // sorted by name

    // The repo with this name, or nullptr.
    const RepoEntry* find(std::string_view name) const;
};

// Repo names: 1 to 100 letters, digits, '.', '_' and '-', not starting with
// '-' or '.'. So a name can't look like an option, and is safe to print.
bool isValidRepoName(std::string_view name);

// The index file name for repo `id`'s build number `generation`.
std::string indexFileName(const std::string& id, std::uint32_t generation);

// A new random repo ID that `registry` doesn't use yet.
std::string newRepoId(const Registry& registry);

// The JSON form of the registry, and back. registryFromJson checks
// everything (types, IDs, names, file names, no repeats) and throws
// RegistryError if anything is wrong.
std::string registryToJson(const Registry& registry);
Registry registryFromJson(std::string_view text);

// Where the registry is kept. An interface, so we could move it to SQLite
// later without changing the code that uses it.
class RegistryStore {
public:
    virtual ~RegistryStore() = default;
    virtual Registry load() const = 0;
    virtual void save(const Registry& registry) = 0;
};

// The registry as a JSON file. A missing file is an empty registry. save()
// replaces the file all at once (AtomicFile).
class JsonRegistryStore : public RegistryStore {
public:
    explicit JsonRegistryStore(std::filesystem::path path) : path_(std::move(path)) {}

    Registry load() const override;
    void save(const Registry& registry) override;

private:
    std::filesystem::path path_;
};

}  // namespace codesearch
