#include "codesearch/repo/registry.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <random>
#include <set>
#include <sstream>

#include "codesearch/util/ascii.hpp"
#include "codesearch/util/atomic_file.hpp"

namespace codesearch {

namespace {

// ordered_json keeps keys in the order we add them, so the file is easy to read.
using Json = nlohmann::ordered_json;

constexpr int kRegistryVersion = 1;
constexpr std::string_view kIdPrefix = "r_";
constexpr std::size_t kIdHexDigits = 8;
constexpr std::size_t kMaxNameLength = 100;
constexpr std::size_t kFingerprintHexDigits = 16;
constexpr std::string_view kHexDigits = "0123456789abcdef";

[[noreturn]] void bad(const std::string& problem) {
    throw RegistryError(problem);
}

bool isLowerHex(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool isDigits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return c >= '0' && c <= '9';
    });
}

std::string fingerprintToHex(std::uint64_t value) {
    std::string hex(kFingerprintHexDigits, '0');
    for (std::size_t i = kFingerprintHexDigits; i > 0; --i) {
        hex[i - 1] = kHexDigits[value % 16];
        value /= 16;
    }
    return hex;
}

std::uint64_t fingerprintFromHex(std::string_view hex) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
    if (hex.size() != kFingerprintHexDigits || !isLowerHex(hex) || error != std::errc() ||
        end != hex.data() + hex.size()) {
        bad("config_fingerprint must be 16 lowercase hex digits");
    }
    return value;
}

// ---- Reading fields, with a clear error for each kind of problem ----

const Json& field(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) {
        bad(std::string("missing \"") + key + "\"");
    }
    return object.at(key);
}

std::string stringField(const Json& object, const char* key) {
    const Json& value = field(object, key);
    if (!value.is_string()) {
        bad(std::string("\"") + key + "\" must be a string");
    }
    return value.get<std::string>();
}

std::uint32_t u32Field(const Json& object, const char* key) {
    const Json& value = field(object, key);
    // is_number_unsigned is false for negative numbers and fractions.
    if (!value.is_number_unsigned() ||
        value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
        bad(std::string("\"") + key + "\" must be a whole number from 0 to 4294967295");
    }
    return static_cast<std::uint32_t>(value.get<std::uint64_t>());
}

IndexInfo indexFromJson(const Json& json, const std::string& id) {
    const std::string commitHex = stringField(json, "commit");
    const std::optional<ObjectId> commit = ObjectId::fromHex(commitHex);
    if (!commit.has_value()) {
        bad("\"commit\" must be 40 lowercase hex digits");
    }
    IndexInfo index{
        .file = stringField(json, "file"),
        .commit = commit.value(),
        .formatVersion = u32Field(json, "format_version"),
        .policyVersion = u32Field(json, "policy_version"),
        .configFingerprint = fingerprintFromHex(stringField(json, "config_fingerprint")),
    };
    if (!isValidIndexFileName(index.file, id)) {
        bad("bad index file name \"" + index.file + "\"");
    }
    return index;
}

RepoEntry repoFromJson(const Json& json) {
    std::string id = stringField(json, "id");
    if (!isValidRepoId(id)) {
        bad("bad repo ID \"" + id + "\"");
    }
    std::string name = stringField(json, "name");
    if (!isValidRepoName(name)) {
        bad("bad repo name \"" + name + "\"");
    }
    const std::string sourceText = stringField(json, "source");
    RepoSource source = RepoSource::Local;
    std::string location;
    if (sourceText == "local") {
        location = stringField(json, "path");
    } else if (sourceText == "github") {
        source = RepoSource::GitHub;
        location = stringField(json, "url");
    } else {
        bad("unknown source \"" + sourceText + "\"");
    }
    if (location.empty()) {
        bad("repo " + name + " has no location");
    }
    IndexInfo index = indexFromJson(field(json, "index"), id);
    return RepoEntry{
        .id = std::move(id),
        .name = std::move(name),
        .source = source,
        .location = std::move(location),
        .index = std::move(index),
    };
}

}  // namespace

bool isValidRepoId(std::string_view id) {
    return id.size() == kIdPrefix.size() + kIdHexDigits && id.starts_with(kIdPrefix) &&
           isLowerHex(id.substr(kIdPrefix.size()));
}

bool isValidIndexFileName(std::string_view file, std::string_view id) {
    const std::string start = std::string(id) + "-g";
    if (!file.starts_with(start) || !file.ends_with(".idx")) {
        return false;
    }
    const std::string_view digits =
        file.substr(start.size(), file.size() - start.size() - std::string_view(".idx").size());
    return digits.size() >= 4 && isDigits(digits);
}

const RepoEntry* Registry::find(std::string_view name) const {
    for (const RepoEntry& repo : repos) {
        if (repo.name == name) {
            return &repo;
        }
    }
    return nullptr;
}

bool isValidRepoName(std::string_view name) {
    if (name.empty() || name.size() > kMaxNameLength || name.front() == '-' ||
        name.front() == '.') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return isAsciiAlnum(c) || c == '.' || c == '_' || c == '-';
    });
}

std::string indexFileName(const std::string& id, std::uint32_t generation) {
    std::string digits = std::to_string(generation);
    if (digits.size() < 4) {
        digits.insert(0, 4 - digits.size(), '0');  // 7 -> "0007"
    }
    return id + "-g" + digits + ".idx";
}

std::string nextIndexFileName(const std::string& id, const std::string& currentFile) {
    if (currentFile.empty()) {
        return indexFileName(id, 1);
    }
    if (!isValidIndexFileName(currentFile, id)) {
        throw RegistryError("bad index file name: " + currentFile);
    }
    // The digits between "-g" and ".idx".
    const std::size_t start = id.size() + 2;
    const std::size_t end = currentFile.size() - 4;
    std::uint32_t generation = 0;
    // from_chars reads a number from text. It fails if the text isn't a
    // number or the number doesn't fit in `generation`.
    const auto [stop, error] =
        std::from_chars(currentFile.data() + start, currentFile.data() + end, generation);
    if (error != std::errc() || stop != currentFile.data() + end ||
        generation == std::numeric_limits<std::uint32_t>::max()) {
        throw RegistryError("can't count up from index file " + currentFile);
    }
    return indexFileName(id, generation + 1);
}

std::string newRepoId(const Registry& registry) {
    // random_device asks the system for random numbers. It's slow, but we
    // only need one.
    std::random_device random;
    while (true) {
        const std::uint32_t value = random();
        std::string id(kIdPrefix);
        id += fingerprintToHex(value).substr(kFingerprintHexDigits - kIdHexDigits);
        const bool taken =
            std::any_of(registry.repos.begin(), registry.repos.end(), [&](const RepoEntry& repo) {
                return repo.id == id;
            });
        if (!taken) {
            return id;
        }
    }
}

std::string registryToJson(const Registry& registry) {
    Json repos = Json::array();
    for (const RepoEntry& repo : registry.repos) {
        Json entry;
        entry["id"] = repo.id;
        entry["name"] = repo.name;
        if (repo.source == RepoSource::Local) {
            entry["source"] = "local";
            entry["path"] = repo.location;
        } else {
            entry["source"] = "github";
            entry["url"] = repo.location;
        }
        entry["index"] = {
            {"file", repo.index.file},
            {"commit", repo.index.commit.hex()},
            {"format_version", repo.index.formatVersion},
            {"policy_version", repo.index.policyVersion},
            {"config_fingerprint", fingerprintToHex(repo.index.configFingerprint)},
        };
        repos.push_back(entry);
    }
    const Json json = {{"version", kRegistryVersion}, {"repos", repos}};
    try {
        return json.dump(2) + "\n";           // 2 spaces of indent
    } catch (const Json::exception& error) {  // only invalid UTF-8 can fail here
        throw RegistryError(std::string("can't save the registry: ") + error.what());
    }
}

Registry registryFromJson(std::string_view text) {
    Json json;
    try {
        json = Json::parse(text);
    } catch (const Json::exception& error) {
        bad(std::string("not valid JSON: ") + error.what());
    }
    const std::uint32_t version = u32Field(json, "version");
    if (version > kRegistryVersion) {
        throw NewerRegistryError("it has version " + std::to_string(version) +
                                 ", but this code-search reads version " +
                                 std::to_string(kRegistryVersion));
    }
    if (version != kRegistryVersion) {
        bad("it has version " + std::to_string(version) + ", but this program reads version " +
            std::to_string(kRegistryVersion));
    }
    const Json& repos = field(json, "repos");
    if (!repos.is_array()) {
        bad("\"repos\" must be a list");
    }

    Registry registry;
    std::set<std::string> ids;
    std::set<std::string> names;
    for (const Json& entry : repos) {
        RepoEntry repo = repoFromJson(entry);
        // insert(...).second is false if the value was already in the set.
        if (!ids.insert(repo.id).second) {
            bad("repo ID " + repo.id + " is used twice");
        }
        if (!names.insert(repo.name).second) {
            bad("repo name " + repo.name + " is used twice");
        }
        registry.repos.push_back(std::move(repo));
    }
    std::sort(registry.repos.begin(), registry.repos.end(),
              [](const RepoEntry& a, const RepoEntry& b) {
                  return a.name < b.name;
              });
    return registry;
}

Registry JsonRegistryStore::load() const {
    const std::ifstream in(path_, std::ios::binary);
    if (!in) {
        if (!std::filesystem::exists(path_)) {
            return {};  // nothing added yet
        }
        throw RegistryError("could not read " + path_.string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    try {
        return registryFromJson(text.str());
    } catch (const NewerRegistryError& error) {
        throw NewerRegistryError(path_.string() + " was written by a newer code-search: " +
                                 error.what() + ". Install the newer version to use it");
    } catch (const RegistryError& error) {
        throw RegistryError(path_.string() + " is damaged: " + error.what() +
                            ". Put back a copy that works, or remove the folder " +
                            path_.parent_path().string() + " to start over");
    }
}

void JsonRegistryStore::save(const Registry& registry) {
    writeFileAtomically(path_, registryToJson(registry));
}

}  // namespace codesearch
