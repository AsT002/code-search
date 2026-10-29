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

- `Workspace` (`~/.code-search` with `indexes/`, `repos/` and `tmp/`), internal
  repo IDs for every path, `FileLock`.
- `Registry` saved as JSON through `RegistryStore`. It names the current index
  file for each repo.
- One lock held for the whole of every `add`, `update`, `remove` and cleanup.
  Searches don't take it.
- `Publisher`, under the lock: build in `tmp/`, check the file, fsync, rename it
  into `indexes/`, write the registry with a temp file and rename, delete unused
  index files and leftovers in `tmp/`. Cleanup at startup takes the lock too.
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

- Lock in the query meaning table from the architecture doc: matching within a
  file, what limits count, result order, what `path:`, `file:` and `repo:` mean,
  escaping and precedence.
- `Parser`: `repo:`, `path:`, `file:`, `lang:`, `case:yes`, `-` to exclude, quoted
  text, `or`, parentheses.
- The planner keeps the whole query, simplifies it per repo, and uses
  `RepoSelection` (`AllRepos`, `SelectedRepos`, `NoRepos`).
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
