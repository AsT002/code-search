#include <csignal>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "codesearch/app/index_commit.hpp"
#include "codesearch/search/searcher.hpp"
#include "codesearch/version.hpp"

namespace {

using namespace codesearch;

void printUsage(std::ostream& out) {
    out << "usage:\n"
           "  code-search search --path <repo> [--case-sensitive] [-l] [--] <text>\n"
           "\n"
           "Use -- before text that starts with a dash, like: -- --flag\n"
           "\n"
           "  code-search --version\n"
           "  code-search --help\n";
}

struct SearchArgs {
    std::string repoPath;
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
        if (arg == "--path" && i + 1 < args.size()) {
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
    if (result.repoPath.empty() || !haveText) {
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

int runSearch(const SearchArgs& args) {
    const GitRepo repo(args.repoPath);
    const std::optional<ObjectId> commit = repo.resolveHead();
    if (!commit.has_value()) {
        std::cerr << "code-search: the repository has no commits yet\n";
        return 1;
    }

    const CommitIndex built = indexCommit(repo, commit.value(), FileFilter());
    printReport(commit.value(), built.report);

    SearchOptions options;
    options.caseSensitive = args.caseSensitive;
    const SearchResults results = search(built.index, literal(args.text), options);

    for (const FileMatch& file : results.files) {
        if (!std::cout) {
            break;  // output failed, so the rest would be lost too
        }
        if (args.filesOnly) {
            std::cout << file.path << '\n';
            continue;
        }
        built.index.withContent(file.doc, [&file](std::string_view content) {
            std::size_t lastLine = 0;
            for (const Match& match : file.matches) {
                if (match.line == lastLine) {
                    continue;  // print each line once, even with several matches on it
                }
                lastLine = match.line;
                std::cout << file.path << ':' << match.line << ':' << match.column << ':'
                          << lineAt(content, match.bytes.begin) << '\n';
            }
        });
    }
    return results.files.empty() ? 1 : 0;  // like grep: 1 means nothing found
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

    if (args.size() >= 2 && std::string_view(args[1]) == "search") {
        const std::optional<SearchArgs> searchArgs = parseSearchArgs(args.subspan(2));
        if (!searchArgs.has_value()) {
            printUsage(std::cerr);
            return 2;
        }
        return runSearch(searchArgs.value());
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
