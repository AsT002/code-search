# Development plan

Each milestone ends with something that works and a clear "done when" check. The
design is in [ARCHITECTURE.md](ARCHITECTURE.md).

| #    | Milestone | Result |
|------|-----------|--------|
| M0   | Scaffold | Project builds, tests run, sanitizers work |
| M1   | In-memory core | Trigram index and exact search, tested against brute force |
| M2   | Reading from Git | Search one commit of a real repo, start to finish |
| M2.5 | Storage experiments | Numbers to pick the index file defaults |
| M3   | Index files | Index saved to disk, mmap'd and checked |
| M4   | Managing repos | `add`, `list`, `remove`, `search --repo`, safe publishing |
| M5   | Update | Rebuild only when the commit or our rules changed |
| M6   | Query language and output | Full AND/OR/NOT, filters, stable JSON |
| M7   | Performance | Baselines, limited parallelism, measured speedups |
| M8   | Regex | RE2 on every file, then a safe way to skip files |
| M9   | HTTP API and UI | Server and React frontend on the same engine |

## M0: Scaffold

Status: done.

- CMake with the `codesearch_core` library, the `code-search` program, tests and
  benchmarks.
- GoogleTest and Google Benchmark through FetchContent, pinned to exact versions.
- Supported: macOS and Linux, Clang (and GCC on Linux), C++20.
- Build presets for debug, release, sanitizers (ASan and UBSan) and clang-tidy.
  `clang-format` for formatting.

Done when: `ctest` passes in the normal and sanitizer builds.

## M1: In-memory core

Status: done.

- `trigram.hpp`: pull out trigrams, lowercase ASCII only (no locale), pack each
  trigram into a `uint32_t`, and keep each trigram once per file.
- `IndexBuilder` in memory, using `unordered_map<uint32_t, vector<uint32_t>>`.
  Doc IDs go up by one, so posting lists come out sorted.
- Posting cursors (`next`, `advance(target)`). Intersect rarest list first and stop
  when one runs out.
- A small query tree with `Literal` and `And`. Text shorter than 3 bytes becomes
  `All`.
- `LiteralMatcher`, and `Evaluator` with `matches` and `collectSpans`.
  Matches are byte offsets; line and column are worked out for display.
- A brute-force reference search that shares no code with the planner.
- A basic benchmark that records build time, search time and peak memory. We
  track the numbers but don't fail on them.

Done when: unit tests pass; a seeded random test shows every real match survives
the trigram step; random files and queries (short, common, missing, non-ASCII,
CRLF) always give the same results as brute force.

## M2: Reading from Git

Status: done.

- `GitRunner`: `posix_spawn` with an argument list, `--` before arguments, stderr
  always drained.
- Look up the commit ID once and pass it to every Git command after that.
- `ls-tree -r -z <commit-id>`, split on NUL. Paths stay raw bytes.
- `BlobStream` over `cat-file --batch`, asking by blob ID. Read each reply by its
  exact size, handle missing objects, check size before reading, and don't let
  the pipes block each other.
- Skip and count symlinks, submodules and LFS pointers.
- `FileFilter`: NUL in the first 8 KB means binary, 1 MB size limit, configurable
  excludes for generated files (lockfiles aren't excluded by name),
  `policy_version` and `config_fingerprint`.
- A report of files and bytes indexed and skipped, with reasons.
- Reject SHA-256 repos with a clear error.
- A temporary command, `code-search search --path <repo> "<q>"`, that builds the
  index in memory and searches it.

Done when: tests on small Git repos made for testing pass. They cover HEAD moving
mid-build, uncommitted changes, file names with newlines, tabs or a leading dash,
symlinks, submodules, LFS pointers and huge files. On a real repo, results match
`git grep -F -i <commit-id>` on the same files.

## M2.5: Storage experiments

Status: done. Results in [BENCHMARKS.md](BENCHMARKS.md). M3 defaults:
delta-varint postings, raw contents.

A short round of measurements on two or three repos (small, medium, large) before
we fix the M3 defaults.

- Postings: raw `u32` vs delta-varint. Size and intersection speed.
- Contents: raw vs LZ4 per file. Size, search speed with a warm cache, and time
  spent decompressing.
- Peak memory while building.
- Size of each section, using the formula in the architecture doc.

Done when: results are in `docs/BENCHMARKS.md` and the M3 defaults are picked
from them. Raw stays available either way.

## M3: Index files

Status: done. Notes:

- The CLI has temporary commands until M4: `index --path <repo> --out <file>`,
  `search --index <file>` and `verify <file>`.
- The damaged-file tests are a seeded GoogleTest (thousands of random changes to
  a valid file), not libFuzzer, so they run with the normal tests and under ASan.
- The doc table has room for each file's blob ID, but it's left as zeros until
  something needs it.
- `writeShard` writes a temp file in the same folder and renames it over the
  target, so a failed write keeps the old file. It writes only through the
  descriptor `mkstemp` returns, and checks before the rename that the temp name
  still points at that file, so swapping the name can't redirect the write. M4's `Publisher` adds the lock,
  fsync and registry on top.
- `MmapFile` opens with `O_NONBLOCK`, so a FIFO is rejected instead of hanging.
- `ObjectId` moved from `git/` to `util/`, so `index/` doesn't include `git/`.

- `shard_format.hpp`: little-endian fixed-size fields, header with feature flags,
  a 32-byte hash field with its algorithm and length, `policy_version`,
  `config_fingerprint`, and a section table with offset, length and codec.
- `ShardWriter` and `ShardReader` on top of `MmapFile`. Fields are read one by one,
  never by casting mapped bytes.
- `withContent(docId, fn)` for reading contents. Posting cursors over the mapped
  file.
- Checks on open (header, sections, overflow) and on read (bounds, varints,
  decompressed size limit). `code-search verify` for a full check.
- Fuzz tests that open damaged files and call every reader method on them.

Done when: an index written to disk and loaded again gives the same results as
M2; damaged files fail with a clear error, in limited time and memory; sanitizers
find nothing.

## M4: Managing repos

Status: done. Notes:

- The temporary `index` command and `search --index` are gone. `search --path`
  stays, for searching a repo without adding it. `verify` takes a repo name.
- Data lives in `$CODE_SEARCH_HOME`, or `~/.code-search` if that isn't set. The
  tests always set it, so they never touch the real one.
- The check before publishing opens the new file (header, commit, file count).
  The full `verify` takes about as long as the build, so it stays a command.
- `add` refuses a folder with no commits. The registry always points to a real
  index, so it never has to describe an empty one.
- Kill tests fork the test process and stop the child with `_exit` right after
  each publishing step, then check the registry and every index it names, and
  that the next cleanup leaves only what the registry uses.
- GitHub tests use a fake cloner that copies a local repo, so they don't need
  the network. `git clone` runs with `GIT_TERMINAL_PROMPT=0`, so a private or
  missing repo fails instead of asking for a password.
- When a search finds that an index file was deleted after it read the
  registry, it reads the registry again and retries once. There's no test that
  forces this race yet.
- The registry uses nlohmann/json 3.12.0, pinned by hash like the other
  dependencies.

- `Workspace` (`~/.code-search` with `indexes/`, `repos/` and `tmp/`), internal
  repo IDs for every path, `FileLock`.
- `Registry` saved as JSON through `RegistryStore`. It names the current index
  file for each repo.
- One lock held for the whole of every `add`, `update`, `remove` and cleanup.
  Searches don't take it.
- `Publisher`, under the lock: build in `tmp/`, check the file, fsync, rename it
  into `indexes/`, write the registry with a temp file and rename, delete unused
  index files and leftovers in `tmp/`. Cleanup runs at the start and end of
  every `add`, `update` and `remove`, under the same lock.
- Clones: `git clone --bare --depth 1 --single-branch --no-tags`. Only accept
  `https://github.com/<owner>/<repo>` (with or without `.git`).
- Commands: `add` (local Git repo or GitHub URL, `--name`), `list`, `remove`, and
  `search` across all repos or with `--repo`.
- Clear errors when a folder isn't a Git repo or `git` isn't installed.
- Before deleting a clone, check its path is inside the workspace.

Done when: every command works; `remove` on a local repo leaves its code alone;
killing the process at any publishing step leaves a complete current state; a
second `add`, `update` or `remove` started during a build waits and never deletes
the running build's files; searches keep working during a change.

## M5: Update

Status: done. Notes:

- `fetchLatest` runs `git fetch --depth 1 --no-tags origin HEAD` and then
  `git update-ref HEAD FETCH_HEAD`. Our clones are bare and have no fetch
  rules, so a plain fetch would leave HEAD where it was. Fetching the remote's
  `HEAD` follows its default branch, and force-pushes too.
- Both commands use `--git-dir` instead of `-C`. With `-C`, Git looks in the
  folders above a broken clone for another repo, and could move that repo's
  branch.
- Before fetching, update checks that the clone is a real folder with no
  symlinks anywhere inside it, and asks Git where it will write
  (`rev-parse --git-common-dir`). A symlinked clone, a symlinked `refs/`, or a
  `commondir` file could otherwise make Git move another repo's branch. A
  missing or redirected clone is an error that says to remove the repo and add
  it again.
- Update skips the build only when the commit, `format_version`,
  `policy_version` and `config_fingerprint` all match, and the index file
  opens with a header that matches the registry. It says why it rebuilt: "new
  commit", "new index format", "new filter rules", "changed settings", "the
  index file can't be used: ..." or "asked to rebuild".
- Damage found on open gets rebuilt by a plain `update`. Damage found on read
  or by `verify` can't be seen by opening the file, so those errors name the
  fix: `code-search update <name> --rebuild`. We don't rebuild during a
  search, since searches don't take the lock.
- Each build gets the next file name (`-g0001.idx`, then `-g0002.idx`), so the
  new file never replaces the one searches may have open.
- `add` and `update` share `buildAndPublish`. Kill tests cover update at every
  step, including right after the fetch.
- A one-file change on golang/go (15,331 files) takes 1.94 s, the same as
  `add` (2.00 s), because the whole index is rebuilt. An update with nothing
  to do takes 0.02 s. Release build, warm cache.
- Users can't change filter settings yet. Tests change them through
  `RepoManager::setFilterConfig`.

- Fetch clones we own (remote default branch), then look up the commit ID once.
- Skip only when commit, `format_version`, `policy_version` and
  `config_fingerprint` all match and the index opens fine. `--rebuild` forces a
  build.
- Rebuild a repo when its index is found to be damaged (on open, on read or by
  `verify`).
- Build and publish the M4 way.

Done when: a new commit gets picked up; an unchanged repo does nothing; changing
the format, the filter code or a setting (like the size limit) triggers a
rebuild; a damaged index gets rebuilt; an update that's killed partway leaves the
old index usable. We record how long a one-file change takes.

## M6: Query language and output

Status: done. Notes:

- The meaning table in the architecture doc is locked in, with rows added for
  `or`, `-`, quotes, parentheses inside words, highlighting, `lang:`, errors
  and line endings. The output formats are described there too.
- `parseQuery` is a recursive descent parser over shell-like words. Only
  `repo`, `path`, `file`, `lang` and `case` typed plainly before a `:` are
  filters, so `std::string` and `"path:x"` are text. Nesting is limited to 100
  levels. A random-input test checks it only ever throws `QueryError`.
- There's no `RepoSelection` type. Each repo gets the query with `repo:`
  resolved (`resolveRepo`), and is skipped when that makes it false.
  `openRepos` does the same, so a damaged index in one repo doesn't stop a
  search limited to another.
- The differential test found a bug in the first version: an `or` that
  became true for a repo was replaced by true, so its other branches lost
  their highlights. An `or` now keeps the true next to its other children.
- `or` merges posting lists. `-`, `path:` and `lang:` plan as `All`.
- `searchRepos` streams files out and stops at the limit, so a limited
  search reads only the files it shows plus one. On golang/go, `--max-results
  5` used 2.7 MB of memory.
- Results are in path order because Git lists a tree in the byte order of
  full paths (a folder sorts as its name plus `/`). A test pins this.
- Ctrl-C sets a flag the search checks before each file. The second Ctrl-C
  stops the program right away. There's no automated test of the signal
  itself; `searchRepos` tests the flag.
- `file:` is the same as `path:` until regex arrives in M8.

- Lock in the query meaning table from the architecture doc: matching within a
  file, what limits count, result order, what `path:`, `file:` and `repo:` mean,
  escaping and precedence.
- `Parser`: `repo:`, `path:`, `file:`, `lang:`, `case:yes`, `-` to exclude, quoted
  text, `or`, parentheses.
- The planner keeps the whole query and simplifies it per repo.
- Turn queries into trigrams using the table in the architecture doc. `or` must
  give correct results here, either by merging posting lists or by falling back
  to `All`. M7 only makes it faster.
- Output: fixed order (repo, path, position), `--max-results`, context lines
  (`-C n`), color, and `--json` that says whether results are complete, cut off
  or cancelled. Paths that aren't valid UTF-8 are encoded in a defined way.

Done when: random query trees (including `or` across `repo:` and `path:`) give
the same results as brute force; output tests pass; searches with a limit use
limited memory; `--json` output stops changing.

## M7: Performance

Status: done. Results in [BENCHMARKS.md](BENCHMARKS.md). Notes:

- Setup: `benchmarks/repos.tsv` pins googletest, brew and go by commit, and
  `benchmarks/queries.tsv` has 40 queries in 8 groups. `search_bench` runs
  them warm in one process; `run_benchmarks.py` runs them as fresh processes
  next to `rg` and `git grep`, and measures builds, updates and searches
  during an update.
- The first profile showed that finding candidates takes under 0.3 ms for
  every query. The time went to checking files: the case-insensitive literal
  search (55%), building result lines (22%), and `locate` and the search
  loop (10%). So the
  biggest wins weren't on the plan's list:
  - `LiteralMatcher::find` tests 32 start positions per step for the
    pattern's first and last bytes, in a loop the compiler vectorizes.
    Case-sensitive patterns that start with a rare byte use `memchr`.
  - `resultLines` jumps to each matching line instead of finding every line
    start, and `locate` counts newlines with `std::count`.
- From the plan's list:
  - one shared `ThreadPool` (one thread per core, at most 8) checks files in
    chunks of 16, across repos, while the calling thread passes results on
    in order. Result lines are built on the pool too.
  - parallel indexing: trigrams are found on the pool while Git is read,
    with at most 16 MiB of content waiting.
  - intersecting stops before a list with more than 256 entries per file
    left.
  - stopping early with a limit was already done in M6 (results stream out).
    The benchmarks report it as "Limit 10".
  - a literal under `-` or in an `or` uses its own posting lists, so a file
    without its trigrams isn't scanned for it.
  - bitmaps and direct scans weren't built: finding candidates is at most
    0.3 ms, so there's nothing for them to win.
- Reviews after M7 found more in the parallel search, all fixed with
  tests: a limited search made every result line of the files it read
  ahead; damage or cancelling in a chunk could hide earlier results or give
  the wrong status; a task that couldn't be queued hung the cleanup; and
  without a limit the pool could hold the results of hundreds of files
  (1.3 GB where one thread used 54 MiB). It now keeps them to about 32 MiB.
  When a pool can't be started, or there's one core, searches and builds
  run on the calling thread.
- A cleanup pass after M7 closed the smaller findings of earlier reviews,
  each with a test: a match starting in a line ending is shown on the next
  line; `--json` output ends with `"status":"error"` when a search fails
  partway; text to a terminal escapes control bytes; a closed output stops
  the search at once (`search the | head -1`: 132 ms to 20 ms); a missing
  `registry.json` no longer lets cleanup delete everything; GitHub URLs are
  compared without case; and `--max-results 0` is documented.
- A second round of reviews of that pass found more, all fixed with tests:
  `--json` gave no document for errors before the search starts; repo
  locations, error messages and JSON could still send the terminal control
  characters; a line's JSON matches could repeat; `r.git.git` URLs added a
  GitHub repo twice; any folder with a `registry.json` was taken for a
  workspace; and `.DS_Store` in `indexes/` blocked a first `add`.
- The first version of the parallel search had a race: a task signaled the
  condition variable after letting go of the lock, so the search could be
  destroyed first. The differential test hung on it now and then. Tasks now
  signal while holding the lock. There's a `tsan` preset now.

- Benchmark setup: a fixed list of repos at exact commit IDs and a fixed list of
  queries, grouped by type (rare words, missing words, common trigrams, 1 to 2
  byte text, filter-heavy, AND/OR/NOT). Warm runs and fresh-process runs.
  Release builds only.
- Baselines: build time, peak memory, index size, time for an update with no
  changes, time to first result, to the first K results and to all results
  (p50, p95). Compare with `rg` and `git grep` on the same files, and optionally
  Zoekt.
- Speedups, each measured before and after:
  - one shared, limited thread pool to search repos in parallel
  - parallel indexing within a memory limit
  - stop intersecting and just check the files once few are left
  - stop early when a search has a limit and a fixed order
  - use a missing trigram to rule files in for `-term`
  - bitmaps or direct scans where they measure faster
- Benchmark searches while an update runs.

Done when: results are in `docs/BENCHMARKS.md` by query type, every speedup has
before and after numbers, targets are set from the baseline, and the brute-force
tests still pass.

## M8: Regex

Status: done. Results in [BENCHMARKS.md](BENCHMARKS.md). Notes:

- RE2 2025-11-05 with Abseil 20250814.2, pinned by hash like the other
  dependencies. They add about 16 s to a clean debug build.
- Syntax: `/re/` for content, `path:/re/` and `file:/re/` for paths. A word
  only becomes a regex if it ends with `/` before a space, `)` or the end, so
  `/usr/bin` and `// TODO` stay text. Backslashes reach RE2 as typed, except
  `\/`. `^` and `$` match per line (`(?m)`).
- An empty match makes a file match without highlighting anything, like an
  empty literal. Without `case:yes`, case folding is RE2's, which covers
  non-ASCII letters too. Text search still folds only ASCII.
- v1: a regex plans as `All`. v2: RE2's prefilter tree (`re2/prefilter.h`,
  what `FilteredRE2` uses) becomes the candidates. It lowercases with Unicode
  rules and the index folds only ASCII, so patterns that can name non-ASCII
  characters, atoms with non-ASCII bytes, and (without case) atoms with `k`
  fall back to `All`. Targeted tests show a missed match for each without
  its rule. The random differential test didn't find these by itself.
- v3 (our own regex analysis) isn't needed: with v2, regexes with required
  text check 2 to 82 MiB instead of 130 MiB, and those without any (like
  `\d{4}-\d{2}-\d{2}`) can't be narrowed by any analysis of required text.
- Regexes under `-` or in an `or` branch don't use the index shortcut that
  literals there get.
- A review after M8 found the prefilter rules incomplete (LONG S for `s`,
  `(?i)` inside the pattern, octal escapes), the group around the pattern
  broke `\Q`, finding every match could take quadratic time, `// /foo/` read
  as one regex, and many `/usr/bin` words parsed in quadratic time. All fixed
  with tests. Matches now stay within a line (RE2's `never_nl`), and on lines
  over 1,024 bytes the work of highlighting is capped.

- `RegexMatcher` with RE2, and a brute-force RE2 reference with the same options.
- v1: a regex becomes `All`, and RE2 checks every file the rest of the query
  allows.
- v2: skip files using `FilteredRE2`'s AND/OR tree (`a|b` stays `Or`). Use `All`
  where it gives nothing useful.
- v3, only if measurements say it's worth it: our own regex-to-trigram analysis.

Done when: no missed matches across random and tricky regexes (alternation,
optional and empty branches, repeats, anchors, empty matches); we report how
many bytes the trigram step saves and the end-to-end speed.

## M9: HTTP API and UI

- C++ HTTP server (cpp-httplib) around `SearchService` and `RepoManager`.
- Listen on localhost only, limit request size, support cancelling and timeouts,
  protect anything that changes state. Define errors, versioning and what
  partial results look like.
- React and TypeScript UI: search box, filters, results with highlights that are
  escaped safely.
- Limits on open index files and caches in the long-running server.
- Look at SQLite for the registry again if the server and CLI fight over writes.

Done when: the CLI and HTTP give the same results for finished searches; tests
for cancelling, updating during a search and escaping pass.

## Later

In rough order:

1. Indexing uncommitted changes, as a separate snapshot type
2. Faster updates using `git diff` and reusing unchanged files
3. Compressed contents, if M2.5 didn't already turn them on
4. Reading contents from Git instead of storing them, after testing `git gc`
5. Symbol search (universal-ctags or tree-sitter)
6. Ranking results
7. A trigram index on file names
8. SHA-256 repos
