#include "codesearch/app/search_service.hpp"

#include <string_view>
#include <utility>

#include "codesearch/index/index_file_error.hpp"
#include "codesearch/index/shard_reader.hpp"
#include "codesearch/query/parser.hpp"
#include "codesearch/query/planner.hpp"

namespace codesearch {

SearchService::SearchService(Workspace workspace, ThreadPool* pool, IndexCache* cache)
    : workspace_(std::move(workspace)), pool_(pool), cache_(cache) {}

PreparedSearch SearchService::prepare(const SearchSettings& settings) const {
    const ParsedQuery parsed = parseQuery(settings.query);
    PreparedSearch search{
        .request =
            {
                .query = parsed.root,
                .options = {.caseSensitive = parsed.caseSensitive.value_or(settings.caseSensitive)},
                .maxResults = settings.maxResults,
                .contextLines = settings.contextLines,
                .filesOnly = settings.filesOnly,
                .pool = pool_,
            },
        .repos = {},
        .damaged = {},
        .warnings = parsed.warnings,
    };
    if (!settings.repo.empty()) {
        search.request.query = allOf({repoFilter(settings.repo), search.request.query});
    }

    if (loadRegistry(workspace_).repos.empty()) {
        throw NoReposError("no repos added yet. Add one with: code-search add <folder>");
    }
    search.repos = openRepos(workspace_, search.request.query, &search.damaged, cache_);
    if (search.repos.empty() && !search.damaged.empty()) {
        // Every repo the query could match is damaged: nothing to search.
        std::string message = search.damaged[0].what();
        for (std::size_t i = 1; i < search.damaged.size(); ++i) {
            message += "\n" + std::string(search.damaged[i].what());
        }
        throw IndexFileError(message);
    }
    return search;
}

std::optional<ResultFile> SearchService::readFile(const std::string& repo,
                                                  const std::string& path) const {
    // repo:<name> opens only that repo, and throws if it isn't added.
    const std::vector<OpenRepo> repos = openRepos(workspace_, repoFilter(repo), nullptr, cache_);
    for (const OpenRepo& open : repos) {
        const ShardReader& index = *open.index;
        // Each file is looked at once, which is quick next to reading it.
        for (DocId doc = 0; doc < index.docCount(); ++doc) {
            if (index.path(doc) != path) {
                continue;
            }
            ResultFile file{.repo = open.name, .path = path, .lines = {}};
            index.withContent(doc, [&file](std::string_view content) {
                file.lines = allLines(content);
            });
            return file;
        }
    }
    return std::nullopt;
}

SearchStatus SearchService::run(PreparedSearch& search, const std::atomic<bool>* cancelled,
                                const SearchEvents& events) {
    for (const std::string& warning : search.warnings) {
        events.onWarning(warning);
    }
    for (const RepoIndexError& error : search.damaged) {
        events.onSkipped(error.repo(), error.what());
    }

    // Damage found during the search skips the rest of that repo too,
    // unless it's the last repo left: then it's an error, as in prepare().
    // `mutable` lets the lambda change its own copy of `left`.
    search.request.cancelled = cancelled;
    search.request.onDamaged = [&events,
                                left = search.repos.size()](const RepoIndexError& error) mutable {
        const std::string message = error.what() + rebuildHint(error.repo());
        --left;
        if (left == 0) {
            throw IndexFileError(message);
        }
        events.onSkipped(error.repo(), message);
    };

    std::vector<SearchTarget> targets;
    targets.reserve(search.repos.size());
    for (const OpenRepo& repo : search.repos) {
        targets.push_back({.name = repo.name, .index = repo.index.get()});
    }
    return searchRepos(targets, search.request, events.onFile);
}

}  // namespace codesearch
