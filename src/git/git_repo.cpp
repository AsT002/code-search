#include "codesearch/git/git_repo.hpp"

#include <charconv>
#include <string_view>
#include <utility>

namespace codesearch {

namespace {

// Splits on spaces and drops empty pieces, so "a  b" gives {"a", "b"}.
std::vector<std::string_view> splitWords(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == ' ') {
            ++pos;
            continue;
        }
        std::size_t end = text.find(' ', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        words.push_back(text.substr(pos, end - pos));
        pos = end;
    }
    return words;
}

std::string_view trimNewline(std::string_view text) {
    if (!text.empty() && text.back() == '\n') {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<std::uint64_t> parseSize(std::string_view text) {
    std::uint64_t value = 0;
    // from_chars turns digits into a number without exceptions or locales.
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

ObjectId parseId(std::string_view hex) {
    const std::optional<ObjectId> id = ObjectId::fromHex(hex);
    if (!id.has_value()) {
        throw GitError("git printed an unexpected object ID: " + std::string(hex));
    }
    return id.value();
}

GitResult runOrThrow(const std::filesystem::path& dir, const std::vector<std::string>& args) {
    GitResult result = runGit(dir, args);
    if (result.exitCode != 0) {
        throw GitError("git " + args.front() + " failed: " + std::string(trimNewline(result.err)));
    }
    return result;
}

// One record from `ls-tree -r -l -z`:
//   <mode> <type> <id> <size>\t<path>
// The size is padded with spaces. It's "-" for submodules and "BAD" when
// the file's contents are missing from the repo. Only the path can
// contain tabs, so we split at the first one.
std::optional<TreeEntry> parseTreeRecord(std::string_view record) {
    const std::size_t tab = record.find('\t');
    if (tab == std::string_view::npos) {
        throw GitError("unexpected git ls-tree output");
    }
    const std::vector<std::string_view> words = splitWords(record.substr(0, tab));
    if (words.size() != 4) {
        throw GitError("unexpected git ls-tree output");
    }
    const std::string_view mode = words[0];
    const std::string_view type = words[1];

    TreeEntry entry{EntryKind::File, parseId(words[2]), 0, std::string(record.substr(tab + 1))};
    if (type == "commit") {
        entry.kind = EntryKind::Submodule;
        return entry;
    }
    if (type != "blob") {
        return std::nullopt;  // -r walks into folders, so there are no others
    }
    if (mode == "120000") {
        entry.kind = EntryKind::Symlink;
    }
    if (words[3] == "BAD") {
        // Git prints "BAD" when it can't find the file's contents to measure.
        throw GitError("the repository is missing the contents of " + entry.path + " (" +
                       entry.id.hex() + ")");
    }
    const std::optional<std::uint64_t> size = parseSize(words[3]);
    if (!size.has_value()) {
        throw GitError("unexpected size in git ls-tree output");
    }
    entry.size = size.value();
    return entry;
}

}  // namespace

GitRepo::GitRepo(std::filesystem::path dir) : dir_(std::move(dir)) {
    const GitResult result = runGit(dir_, {"rev-parse", "--show-object-format"});
    if (result.exitCode != 0) {
        throw GitError("not a Git repository: " + dir_.string());
    }
    const std::string_view format = trimNewline(result.out);
    if (format != "sha1") {
        throw GitError("this repository uses " + std::string(format) +
                       " object IDs; only sha1 is supported for now");
    }
}

std::optional<ObjectId> GitRepo::resolveHead() const {
    // Step 1: read the ID HEAD points to. This only fails if there is none.
    const GitResult head = runGit(dir_, {"rev-parse", "--verify", "--quiet", "HEAD"});
    if (head.exitCode != 0) {
        // The normal reason: HEAD names a branch with no commits yet, so
        // symbolic-ref can still read the branch name. Anything else is broken.
        const GitResult branch = runGit(dir_, {"symbolic-ref", "--quiet", "HEAD"});
        if (branch.exitCode == 0) {
            return std::nullopt;
        }
        throw GitError("HEAD can't be read in " + dir_.string());
    }
    const ObjectId id = parseId(trimNewline(head.out));

    // Step 2: check that the ID is a commit we can read. We use the ID from
    // step 1, not HEAD, so a commit made in between can't change the answer.
    const GitResult commit =
        runGit(dir_, {"rev-parse", "--verify", "--quiet", id.hex() + "^{commit}"});
    if (commit.exitCode != 0) {
        throw GitError("HEAD points to " + id.hex() +
                       ", which can't be read as a commit; the repository may be damaged");
    }
    return parseId(trimNewline(commit.out));
}

std::vector<TreeEntry> GitRepo::listFiles(const ObjectId& commit) const {
    // -r: walk into folders. -l: include sizes. -z: end each record with NUL
    // and print paths as they are. --full-tree: list from the repo root.
    const GitResult result =
        runOrThrow(dir_, {"ls-tree", "-r", "-l", "-z", "--full-tree", commit.hex()});

    std::vector<TreeEntry> entries;
    const std::string_view out = result.out;
    std::size_t pos = 0;
    while (pos < out.size()) {
        std::size_t end = out.find('\0', pos);
        if (end == std::string_view::npos) {
            end = out.size();
        }
        std::optional<TreeEntry> entry = parseTreeRecord(out.substr(pos, end - pos));
        if (entry.has_value()) {
            entries.push_back(std::move(entry.value()));
        }
        pos = end + 1;
    }
    return entries;
}

BlobStream::BlobStream(const GitRepo& repo)
    : git_("git", gitArgs(repo.dir(), {"cat-file", "--batch"})) {}

Blob BlobStream::read(const ObjectId& id, std::uint64_t maxBytes) {
    git_.write(id.hex() + "\n");

    // The reply starts with "<id> blob <size>\n" or "<id> missing\n".
    std::string header;
    if (!git_.readLine(header)) {
        throw GitError("git cat-file stopped unexpectedly");
    }
    const std::vector<std::string_view> words = splitWords(header);
    if (words.size() == 2 && words[1] == "missing") {
        return Blob{BlobStatus::Missing, 0, {}};
    }
    if (words.size() != 3 || words[0] != id.hex()) {
        throw GitError("unexpected reply from git cat-file: " + header);
    }
    if (words[1] != "blob") {
        throw GitError(id.hex() + " is a " + std::string(words[1]) + ", not a file");
    }
    const std::optional<std::uint64_t> size = parseSize(words[2]);
    if (!size.has_value()) {
        throw GitError("unexpected reply from git cat-file: " + header);
    }

    // Then come exactly <size> bytes and one '\n'. Check the size before
    // reading, so a huge file is never loaded into memory.
    Blob blob{BlobStatus::Found, size.value(), {}};
    if (blob.size > maxBytes) {
        git_.skip(blob.size + 1);
        blob.status = BlobStatus::TooLarge;
        return blob;
    }
    git_.readExact(static_cast<std::size_t>(blob.size), blob.content);
    std::string newline;
    git_.readExact(1, newline);
    if (newline != "\n") {
        throw GitError("unexpected reply from git cat-file");
    }
    return blob;
}

}  // namespace codesearch
