#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "codesearch/app/index_cache.hpp"
#include "codesearch/app/repo_manager.hpp"
#include "codesearch/repo/workspace.hpp"
#include "codesearch/search/repo_search.hpp"
#include "codesearch/util/thread_pool.hpp"

namespace codesearch {

// Thrown when a search starts and no repo has been added yet.
class NoReposError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What the user asked for. The CLI fills it from its options, the server
// from the request's URL, so both search the same way.
struct SearchSettings {
    std::string query;
    std::string repo;            // only this repo (like repo:<name>); empty means all of them
    bool caseSensitive = false;  // case:yes or case:no in the query wins
    std::size_t maxResults = 0;  // 0 means no limit
    std::size_t contextLines = 0;
    bool filesOnly = false;
};

// A search with its query parsed and its index files open, ready to run.
struct PreparedSearch {
    SearchRequest request;
    std::vector<OpenRepo> repos;
    std::vector<RepoIndexError> damaged;  // repos left out because their index is damaged
};

struct SearchEvents {
    // Each matching file, in result order.
    std::function<void(const ResultFile& file)> onFile;
    // A repo left out because its index is damaged. `message` names the
    // repo and says how to rebuild it.
    std::function<void(const std::string& repo, const std::string& message)> onSkipped;
};

// Searches the added repos. The CLI and the HTTP server both use it, so a
// finished search gives the same results through either.
//
// Searching has two steps, so a caller can report errors in the query or
// the repos before it starts sending results.
class SearchService {
public:
    // `pool` checks files on several threads; nullptr checks them on the
    // calling thread. `cache` keeps index files open between searches;
    // nullptr opens them for each search. Both must outlive the service.
    SearchService(Workspace workspace, ThreadPool* pool, IndexCache* cache);

    // Parses the query and opens the index of each repo it could match.
    // Throws QueryError for a bad query, UnknownRepoError if it names a
    // repo that isn't added, NoReposError if none is, and IndexFileError if
    // every repo it would search is damaged. Other damaged repos are left
    // out and listed in `damaged`.
    PreparedSearch prepare(const SearchSettings& settings) const;

    // Runs a prepared search. Reports the repos prepare() left out first,
    // then calls events.onFile for each result. A repo found damaged during
    // the search is skipped too, unless it's the last one left: then this
    // throws IndexFileError. Stops before the next file once `cancelled`
    // is set.
    // Static, since it needs nothing from the service: `search` holds it all.
    static SearchStatus run(PreparedSearch& search, const std::atomic<bool>* cancelled,
                            const SearchEvents& events);

private:
    Workspace workspace_;
    ThreadPool* pool_;
    IndexCache* cache_;
};

}  // namespace codesearch
