#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdlib>
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
#include "codesearch/query/parser.hpp"
#include "codesearch/query/planner.hpp"
#include "codesearch/search/json_output.hpp"
#include "codesearch/search/repo_search.hpp"
#include "codesearch/util/utf8.hpp"
#include "codesearch/version.hpp"

namespace {

using namespace codesearch;

void printUsage(std::ostream& out) {
    out << "usage:\n"
           "  code-search add <folder | https://github.com/<owner>/<repo>> [--name <name>]\n"
           "  code-search list\n"
           "  code-search update <name> [--rebuild]\n"
           "  code-search remove <name>\n"
           "  code-search search [options] [--] <query>\n"
           "  code-search verify <name>\n"
           "  code-search --version\n"
           "  code-search --help\n"
           "\n"
           "search options:\n"
           "  --repo <name>         only this added repo (same as repo:<name>)\n"
           "  --path <folder>       a repo you haven't added, indexed in memory each time\n"
           "  --case-sensitive      match case (same as case:yes)\n"
           "  -l                    print only the paths of matching files\n"
           "  -C <n>                show n lines before and after each match\n"
           "  --max-results <n>     stop after n matching lines (files with -l); 0, the\n"
           "                        default, means no limit\n"
           "  --json                print the results as JSON\n"
           "  --color=<when>        auto (the default), always or never\n"
           "\n"
           "query:\n"
           "  foo bar               both, anywhere in the same file\n"
           "  foo or bar            either one\n"
           "  -foo                  files without foo\n"
           "  (a or b) c            parentheses group\n"
           "  \"foo bar\"             quotes keep spaces and words like or\n"
           "  repo:x path:x file:x lang:go case:yes\n"
           "\n"
           "Put the query in quotes so the shell passes it as one argument. Use --\n"
           "before a query that starts with a dash, like: -- '-test foo'\n"
           "Data is kept in ~/.code-search, or in $CODE_SEARCH_HOME if it's set.\n";
}

enum class Color {
    Auto,
    Always,
    Never,
};

struct SearchArgs {
    std::string repoName;  // only this added repo; empty means all of them
    std::string repoPath;  // or a repo that isn't added, indexed in memory
    std::string query;
    bool caseSensitive = false;
    bool filesOnly = false;  // -l: print only the paths of matching files
    std::size_t maxResults = 0;
    std::size_t contextLines = 0;
    bool json = false;
    Color color = Color::Auto;
};

// Reads a whole non-negative number. Nothing if `text` is anything else.
std::optional<std::size_t> parseCount(std::string_view text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// Sets a search option that takes no value, like -l. Returns false if
// `arg` isn't one.
bool readFlag(std::string_view arg, SearchArgs& result) {
    if (arg == "--case-sensitive") {
        result.caseSensitive = true;
    } else if (arg == "-l") {
        result.filesOnly = true;
    } else if (arg == "--json") {
        result.json = true;
    } else if (arg == "--color=auto") {
        result.color = Color::Auto;
    } else if (arg == "--color=always") {
        result.color = Color::Always;
    } else if (arg == "--color=never") {
        result.color = Color::Never;
    } else {
        return false;
    }
    return true;
}

// Sets a search option that takes a value, like --repo <name>. Returns false
// if `arg` isn't one, or the value is wrong.
bool readOption(std::string_view arg, std::string_view value, SearchArgs& result) {
    if ((arg == "--repo" || arg == "--path") && value.empty()) {
        return false;  // an empty name or path would mean "every repo"
    }
    if (arg == "--repo") {
        result.repoName = value;
    } else if (arg == "--path") {
        result.repoPath = value;
    } else if (arg == "--max-results" || arg == "-C") {
        const std::optional<std::size_t> count = parseCount(value);
        if (!count.has_value()) {
            return false;
        }
        if (arg == "-C") {
            result.contextLines = count.value();
        } else {
            result.maxResults = count.value();
        }
    } else {
        return false;
    }
    return true;
}

// Reads the arguments after "search". Returns nothing if they don't make sense.
std::optional<SearchArgs> parseSearchArgs(std::span<char*> args) {
    SearchArgs result;
    bool haveQuery = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (readFlag(arg, result)) {
            continue;
        }
        if (i + 1 < args.size() && readOption(arg, args[i + 1], result)) {
            ++i;  // the value is used up too
            continue;
        }
        if (haveQuery) {
            return std::nullopt;  // a second query, or a bad option after it
        }
        if (arg == "--" && i + 2 == args.size()) {
            // Everything after "--" is the query, even if it starts with a dash.
            result.query = args[++i];
        } else if (!arg.starts_with('-') || arg == "-") {
            result.query = arg;
        } else {
            return std::nullopt;  // an unknown option, or one missing its value
        }
        haveQuery = true;
    }
    // --repo and --path can't be used together.
    if ((!result.repoName.empty() && !result.repoPath.empty()) || !haveQuery) {
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
        // An empty --name would quietly mean the default name.
        if (arg == "--name" && i + 1 < args.size() && result.name.empty() &&
            !std::string_view(args[i + 1]).empty()) {
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
// `pool` can be nullptr, to build on this thread.
std::optional<BuiltRepo> buildRepo(const std::string& path, const FileFilter& filter,
                                   ThreadPool* pool) {
    const GitRepo repo(path);
    const std::optional<ObjectId> commit = repo.resolveHead();
    if (!commit.has_value()) {
        std::cerr << "code-search: the repository has no commits yet\n";
        return std::nullopt;
    }
    BuiltRepo result{
        .commit = commit.value(),
        .built = indexCommit(repo, commit.value(), filter, pool),
    };
    printReport(result.commit, result.built.report);
    return result;
}

// Escape codes that color text in a terminal, like grep's colors.
struct Colors {
    std::string_view path;
    std::string_view number;
    std::string_view separator;
    std::string_view match;
    std::string_view reset;
};

Colors pickColors(Color color) {
    bool on = color == Color::Always;
    if (color == Color::Auto) {
        // Only on a terminal, and not if the user turned colors off
        // (https://no-color.org) or the terminal can't show them.
        const char* noColor = std::getenv("NO_COLOR");  // NOLINT(concurrency-mt-unsafe)
        const char* term = std::getenv("TERM");         // NOLINT(concurrency-mt-unsafe)
        on = ::isatty(STDOUT_FILENO) == 1 && (noColor == nullptr || *noColor == '\0') &&
             (term == nullptr || std::string_view(term) != "dumb");
    }
    if (!on) {
        return {};
    }
    return {
        .path = "\033[35m",       // magenta
        .number = "\033[32m",     // green
        .separator = "\033[36m",  // cyan
        .match = "\033[1;31m",    // bold red
        .reset = "\033[0m",
    };
}

// `text` as it should be printed: escaped if `escape`, so a file's bytes
// can't control the terminal (see escapeForTerminal).
std::string shown(std::string_view text, bool escape) {
    return escape ? escapeForTerminal(text) : std::string(text);
}

// Prints a line's text with its matches colored. Each piece is escaped on
// its own, so the highlights stay where they were.
void printHighlighted(const ResultLine& line, const Colors& colors, bool escape) {
    std::size_t done = 0;
    for (const Span& span : line.matches) {
        // Matches can overlap ("ab or bc" in "abc"). Color only the part of
        // this one that isn't colored yet.
        const std::size_t begin = std::max(span.begin, done);
        if (span.end <= begin) {
            continue;
        }
        std::cout << shown(std::string_view(line.text).substr(done, begin - done), escape)
                  << colors.match
                  << shown(std::string_view(line.text).substr(begin, span.end - begin), escape)
                  << colors.reset;
        done = span.end;
    }
    std::cout << shown(std::string_view(line.text).substr(done), escape);
}

// Prints one file's results, grep style:
//   repo:path:line:column:text   a line with a match
//   repo:path-line-text          a context line (with -C)
//   repo:path                    with -l, or when there's nothing to highlight
// With -C, "--" goes between groups of lines that aren't next to each other.
class TextPrinter {
public:
    // `escape`: make the text safe for a terminal (see escapeForTerminal).
    TextPrinter(std::size_t contextLines, Colors colors, bool escape)
        : contextLines_(contextLines), colors_(colors), escape_(escape) {}

    void file(const ResultFile& file) {
        const std::string prefix = file.repo.empty() ? "" : file.repo + ":";
        const std::string path = shown(file.path, escape_);
        if (file.lines.empty()) {
            std::cout << colors_.path << prefix << path << colors_.reset << '\n';
            return;
        }
        std::size_t previous = 0;
        for (const ResultLine& line : file.lines) {
            const bool gap = previous == 0 || line.number != previous + 1;
            if (contextLines_ > 0 && gap && printedLines_) {
                std::cout << colors_.separator << "--" << colors_.reset << '\n';
            }
            const char separator = line.matches.empty() ? '-' : ':';
            std::cout << colors_.path << prefix << path << colors_.reset << colors_.separator
                      << separator << colors_.reset << colors_.number << line.number
                      << colors_.reset << colors_.separator << separator << colors_.reset;
            if (!line.matches.empty()) {
                std::cout << colors_.number << line.matches[0].begin + 1 << colors_.reset
                          << colors_.separator << ':' << colors_.reset;
            }
            printHighlighted(line, colors_, escape_);
            std::cout << '\n';
            previous = line.number;
            printedLines_ = true;
        }
    }

private:
    std::size_t contextLines_;
    Colors colors_;
    bool escape_;
    bool printedLines_ = false;
};

// Set by Ctrl-C while a search runs. Signal handlers may only touch
// lock-free atomics like this one, and a handler can't be given arguments,
// so it has to be a global.
std::atomic<bool> gCancelled = false;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static_assert(std::atomic<bool>::is_always_lock_free);

extern "C" void onInterrupt(int /*signal*/) {
    gCancelled = true;
    // A second Ctrl-C stops the program right away. signal() is safe to
    // call in a handler.
    static_cast<void>(std::signal(SIGINT, SIG_DFL));
}

// Runs the search and prints the results, as text or JSON. Sets `found` if
// there was any. If the search fails partway, JSON output is still ended as
// one whole document, saying why.
SearchStatus printResults(std::span<const SearchTarget> targets, const SearchRequest& request,
                          const SearchArgs& args, bool& found) {
    std::optional<JsonResultWriter> json;
    if (args.json) {
        json.emplace(std::cout);
    }
    // On a terminal, a file's bytes could send it commands (like ESC
    // sequences), so they're escaped. Colors mean a terminal too, maybe
    // behind a pager like `less -R`. Into a pipe or file, bytes stay as
    // they are, for other programs to read.
    const Colors colors = pickColors(args.color);
    const bool escape = ::isatty(STDOUT_FILENO) == 1 || !colors.reset.empty();
    TextPrinter text(args.contextLines, colors, escape);

    static_cast<void>(std::signal(SIGINT, onInterrupt));
    SearchStatus status = SearchStatus::Complete;
    try {
        status = searchRepos(targets, request, [&](const ResultFile& file) {
            found = true;
            if (json.has_value()) {
                json->file(file);
            } else {
                text.file(file);
            }
            // The reader went away (like `| head` after enough lines): stop
            // now instead of searching on for output nobody reads.
            if (!std::cout) {
                throw std::runtime_error("could not write the output");
            }
        });
    } catch (const RepoIndexError& error) {
        const std::string message = error.what() + rebuildHint(error.repo());
        if (json.has_value()) {
            json->fail(message);  // so what's printed so far is still whole JSON
        }
        throw IndexFileError(message);
    } catch (const std::exception& error) {
        if (json.has_value()) {
            json->fail(error.what());
        }
        throw;
    }
    static_cast<void>(std::signal(SIGINT, SIG_DFL));

    if (json.has_value()) {
        json->finish(status);
    } else if (status == SearchStatus::Truncated) {
        std::cerr << "code-search: stopped after " << args.maxResults
                  << " results (--max-results); there are more\n";
    }
    return status;
}

int runSearch(const SearchArgs& args) {
    const ParsedQuery parsed = parseQuery(args.query);
    SearchRequest request{
        .query = parsed.root,
        .options = {.caseSensitive = parsed.caseSensitive.value_or(args.caseSensitive)},
        .maxResults = args.maxResults,
        .contextLines = args.contextLines,
        .filesOnly = args.filesOnly,
        .cancelled = &gCancelled,
    };

    // Files are checked on several threads; results still come out in order.
    const std::unique_ptr<ThreadPool> pool = startPool();
    request.pool = pool.get();

    std::vector<SearchTarget> targets;
    std::optional<BuiltRepo> built;  // with --path
    std::vector<OpenRepo> repos;     // otherwise
    if (!args.repoPath.empty()) {
        if (!repoNamesIn(request.query).empty()) {
            throw QueryError("repo: only works on added repos, not with --path");
        }
        built = buildRepo(args.repoPath, FileFilter(), pool.get());
        if (!built.has_value()) {
            return 1;
        }
        targets.push_back({.name = "", .index = &built->built.index});
    } else {
        const Workspace workspace(Workspace::defaultRoot());
        if (loadRegistry(workspace).repos.empty()) {
            std::cerr
                << "code-search: no repos added yet. Add one with: code-search add <folder>\n";
            return 2;
        }
        if (!args.repoName.empty()) {
            request.query = allOf({repoFilter(args.repoName), request.query});
        }
        repos = openRepos(workspace, request.query);
        for (const OpenRepo& repo : repos) {
            targets.push_back({.name = repo.name, .index = repo.index.get()});
        }
    }

    bool found = false;
    const SearchStatus status = printResults(targets, request, args, found);
    if (status == SearchStatus::Cancelled) {
        std::cerr << "code-search: cancelled\n";
        return 130;  // what shells use for "stopped by Ctrl-C"
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
        if (!matchesRegistry(reader, repo->index)) {
            throw IndexFileError("the index file doesn't match the registry");
        }
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
