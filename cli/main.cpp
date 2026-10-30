#include <algorithm>
#include <csignal>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/app/index_commit.hpp"
#include "codesearch/app/repo_manager.hpp"
#include "codesearch/git/git_runner.hpp"
#include "codesearch/index/index_file_error.hpp"
#include "codesearch/index/shard_reader.hpp"
#include "codesearch/search/searcher.hpp"
#include "codesearch/version.hpp"

namespace {

using namespace codesearch;

void printUsage(std::ostream& out) {
    out << "usage:\n"
           "  code-search add <folder | https://github.com/<owner>/<repo>> [--name <name>]\n"
           "  code-search list\n"
           "  code-search update <name> [--rebuild]\n"
           "  code-search remove <name>\n"
           "  code-search search [--repo <name>] [--case-sensitive] [-l] [--] <text>\n"
           "  code-search search --path <folder> [--case-sensitive] [-l] [--] <text>\n"
           "  code-search verify <name>\n"
           "  code-search --version\n"
           "  code-search --help\n"
           "\n"
           "search looks in every added repo, or only in --repo. With --path it\n"
           "indexes a repo you haven't added, in memory, every time.\n"
           "Use -- before text that starts with a dash, like: -- --flag\n"
           "Data is kept in ~/.code-search, or in $CODE_SEARCH_HOME if it's set.\n";
}

struct SearchArgs {
    std::string repoName;  // only this added repo; empty means all of them
    std::string repoPath;  // or a repo that isn't added, indexed in memory
    std::string text;
    bool caseSensitive = false;
    bool filesOnly = false;  // -l: print only the paths of matching files
};

// Reads the arguments after "search". Returns nothing if they don't make sense.
std::optional<SearchArgs> parseSearchArgs(std::span<char*> args) {
    SearchArgs result;
    bool haveText = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--repo" && i + 1 < args.size()) {
            ++i;
            result.repoName = args[i];
        } else if (arg == "--path" && i + 1 < args.size()) {
            ++i;
            result.repoPath = args[i];
        } else if (arg == "--case-sensitive") {
            result.caseSensitive = true;
        } else if (arg == "-l") {
            result.filesOnly = true;
        } else if (arg == "--" && !haveText && i + 2 == args.size()) {
            // Everything after "--" is the text, even if it starts with a dash.
            ++i;
            result.text = args[i];
            haveText = true;
        } else if (!haveText && (!arg.starts_with('-') || arg == "-")) {
            result.text = arg;
            haveText = true;
        } else {
            return std::nullopt;
        }
    }
    // --repo and --path can't be used together.
    if ((!result.repoName.empty() && !result.repoPath.empty()) || !haveText) {
        return std::nullopt;
    }
    return result;
}

struct AddArgs {
    std::string source;
    std::string name;
};

// Reads the arguments after "add": a source, and maybe --name <name>.
std::optional<AddArgs> parseAddArgs(std::span<char*> args) {
    AddArgs result;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--name" && i + 1 < args.size() && result.name.empty()) {
            ++i;
            result.name = args[i];
        } else if (result.source.empty() && !arg.starts_with('-') && !arg.empty()) {
            result.source = arg;
        } else {
            return std::nullopt;
        }
    }
    if (result.source.empty()) {
        return std::nullopt;
    }
    return result;
}

// The full line around byte `offset`, without its line ending.
std::string_view lineAt(std::string_view content, std::size_t offset) {
    std::size_t start = 0;
    if (offset > 0) {
        const std::size_t newline = content.rfind('\n', offset - 1);
        if (newline != std::string_view::npos) {
            start = newline + 1;
        }
    }
    std::size_t end = content.find('\n', offset);
    if (end == std::string_view::npos) {
        end = content.size();
    }
    std::string_view line = content.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

void printReport(const ObjectId& commit, const IngestReport& report) {
    std::cerr << "commit " << commit.hex() << ": indexed " << report.indexed.files << " files ("
              << report.indexed.bytes << " bytes)";
    for (const auto& [reason, count] : report.skipped) {
        std::cerr << ", " << skipReasonName(reason) << " " << count.files;
    }
    std::cerr << '\n';
}

struct BuiltRepo {
    ObjectId commit;
    CommitIndex built;
};

// Indexes the repo's HEAD in memory and prints what was indexed. Returns
// nothing, after saying why, if the repo has no commits.
std::optional<BuiltRepo> buildRepo(const std::string& path, const FileFilter& filter) {
    const GitRepo repo(path);
    const std::optional<ObjectId> commit = repo.resolveHead();
    if (!commit.has_value()) {
        std::cerr << "code-search: the repository has no commits yet\n";
        return std::nullopt;
    }
    BuiltRepo result{.commit = commit.value(), .built = indexCommit(repo, commit.value(), filter)};
    printReport(result.commit, result.built.report);
    return result;
}

// Prints the matches in one index. `prefix` goes before each path, like
// "my-backend:" when searching added repos. Returns true if anything matched.
bool printResults(const IndexReader& index, const SearchArgs& args, const std::string& prefix) {
    SearchOptions options;
    options.caseSensitive = args.caseSensitive;
    const SearchResults results = search(index, literal(args.text), options);

    for (const FileMatch& file : results.files) {
        if (!std::cout) {
            break;  // output failed, so the rest would be lost too
        }
        if (args.filesOnly) {
            std::cout << prefix << file.path << '\n';
            continue;
        }
        index.withContent(file.doc, [&](std::string_view content) {
            std::size_t lastLine = 0;
            for (const Match& match : file.matches) {
                if (match.line == lastLine) {
                    continue;  // print each line once, even with several matches on it
                }
                lastLine = match.line;
                std::cout << prefix << file.path << ':' << match.line << ':' << match.column << ':'
                          << lineAt(content, match.bytes.begin) << '\n';
            }
        });
    }
    return !results.files.empty();
}

int runSearch(const SearchArgs& args) {
    if (!args.repoPath.empty()) {
        const std::optional<BuiltRepo> repo = buildRepo(args.repoPath, FileFilter());
        if (!repo.has_value()) {
            return 1;
        }
        return printResults(repo->built.index, args, "") ? 0 : 1;
    }

    const std::vector<OpenRepo> repos =
        openRepos(Workspace(Workspace::defaultRoot()), args.repoName);
    if (repos.empty()) {
        std::cerr << "code-search: no repos added yet. Add one with: code-search add <folder>\n";
        return 2;
    }
    bool found = false;
    for (const OpenRepo& repo : repos) {
        try {
            found = printResults(*repo.index, args, repo.name + ":") || found;
        } catch (const IndexFileError& error) {
            throw IndexFileError(repo.name + ": " + error.what() + rebuildHint(repo.name));
        }
    }
    return found ? 0 : 1;  // like grep: 1 means nothing found
}

RepoManager makeManager() {
    RepoManager manager(Workspace(Workspace::defaultRoot()), cloneShallow);
    manager.setOnWait([] {
        std::cerr << "code-search: waiting for another code-search command to finish...\n";
    });
    return manager;
}

std::string_view sourceName(RepoSource source) {
    return source == RepoSource::Local ? "local" : "github";
}

int runAdd(const AddArgs& args) {
    const AddResult result = makeManager().add(args.source, args.name);
    printReport(result.repo.index.commit, result.report);
    std::cout << "added " << result.repo.name << " (" << result.repo.location << ")\n";
    return 0;
}

int runList() {
    const Registry registry = loadRegistry(Workspace(Workspace::defaultRoot()));
    if (registry.repos.empty()) {
        std::cerr << "code-search: no repos added yet. Add one with: code-search add <folder>\n";
        return 0;
    }
    std::size_t width = 0;
    for (const RepoEntry& repo : registry.repos) {
        width = std::max(width, repo.name.size());
    }
    constexpr std::size_t kShortCommit = 12;
    for (const RepoEntry& repo : registry.repos) {
        std::cout << repo.name << std::string(width - repo.name.size() + 2, ' ')
                  << sourceName(repo.source) << (repo.source == RepoSource::Local ? "   " : "  ")
                  << repo.index.commit.hex().substr(0, kShortCommit) << "  " << repo.location
                  << '\n';
    }
    return 0;
}

int runUpdate(const std::string& name, bool rebuild) {
    const UpdateResult result = makeManager().update(name, rebuild);
    if (result.reason.empty()) {
        std::cout << result.repo.name << " is up to date (commit " << result.repo.index.commit.hex()
                  << ")\n";
        return 0;
    }
    printReport(result.repo.index.commit, result.report);
    std::cout << "updated " << result.repo.name << " (" << result.reason << ")\n";
    return 0;
}

int runRemove(const std::string& name) {
    const RepoEntry removed = makeManager().remove(name);
    std::cout << "removed " << removed.name;
    if (removed.source == RepoSource::Local) {
        std::cout << " (" << removed.location << " itself was not touched)";
    }
    std::cout << '\n';
    return 0;
}

// Exit codes: 0 the index is fine, 1 it is damaged, 2 it can't be read at all.
int runVerify(const std::string& name) {
    const Workspace workspace(Workspace::defaultRoot());
    const Registry registry = loadRegistry(workspace);
    const RepoEntry* repo = registry.find(name);
    if (repo == nullptr) {
        std::cerr << "code-search: no repo named " << name << '\n';
        return 2;
    }
    const std::filesystem::path path = workspace.indexesDir() / repo->index.file;
    try {
        const ShardReader reader(path);
        reader.verify();
        std::cout << name << ": ok, " << reader.docCount() << " files, "
                  << reader.header().trigramCount << " trigrams, commit " << reader.commit().hex()
                  << '\n';
        return 0;
    } catch (const IndexFileError& error) {
        std::cerr << "code-search: " << name << " (" << path.string() << "): " << error.what()
                  << rebuildHint(name) << '\n';
        return 1;
    }
}

int run(std::span<char*> args) {
    if (args.size() == 2) {
        const std::string_view arg = args.back();
        if (arg == "--version") {
            std::cout << "code-search " << codesearch::version() << '\n';
            return 0;
        }
        if (arg == "--help") {
            printUsage(std::cout);
            return 0;
        }
    }

    const std::string_view command = args.size() >= 2 ? args[1] : "";
    if (command == "search") {
        const std::optional<SearchArgs> searchArgs = parseSearchArgs(args.subspan(2));
        if (searchArgs.has_value()) {
            return runSearch(searchArgs.value());
        }
    } else if (command == "add") {
        const std::optional<AddArgs> addArgs = parseAddArgs(args.subspan(2));
        if (addArgs.has_value()) {
            return runAdd(addArgs.value());
        }
    } else if (command == "list" && args.size() == 2) {
        return runList();
    } else if (command == "update" && args.size() == 3) {
        return runUpdate(args[2], false);
    } else if (command == "update" && args.size() == 4 &&
               std::string_view(args[3]) == "--rebuild") {
        return runUpdate(args[2], true);
    } else if (command == "remove" && args.size() == 3) {
        return runRemove(args[2]);
    } else if (command == "verify" && args.size() == 3) {
        return runVerify(args[2]);
    }

    printUsage(std::cerr);
    return 2;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Ignore SIGPIPE, so a pipe closing early (like `| head`) gives an error
    // we can handle instead of killing the program. static_cast<void> says
    // we mean to ignore the return value.
    static_cast<void>(std::signal(SIGPIPE, SIG_IGN));

    try {
        const int exitCode = run(std::span<char*>(argv, static_cast<std::size_t>(argc)));
        // Results that never arrived must not look like success. cout keeps
        // a "failed" flag after any write fails; flush() sends what's left.
        std::cout.flush();
        if (!std::cout) {
            std::cerr << "code-search: could not write the output\n";
            return 2;
        }
        return exitCode;
    } catch (const std::exception& error) {
        std::cerr << "code-search: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "code-search: unknown error\n";
    }
    return 2;
}
