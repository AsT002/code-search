# code-search

A local code search engine written in C++. It indexes Git repositories and finds code in them fast.

Work in progress. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and [docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md) for what's next.

## Requirements

- CMake 3.25 or newer
- Ninja
- A C++20 compiler (Clang or GCC)
- Optional: `clang-format` and `clang-tidy` (on macOS, `brew install llvm`)
- For the web UI: Node.js 20 or newer

Tested on macOS with Apple Clang. Linux should work but hasn't been tested yet.

## Build and test

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Other presets:

| Preset    | What it does |
|-----------|--------------|
| `debug`   | Debug build |
| `release` | Optimized build, used for benchmarks |
| `asan`    | Debug build with AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan`    | Debug build with ThreadSanitizer, for the threaded search and build |
| `tidy`    | Debug build that runs clang-tidy on every file |

All presets treat compiler warnings as errors.

## Add repos and search them

```sh
./build/debug/code-search add ~/projects/backend               # a local Git repo
./build/debug/code-search add https://github.com/example/frontend
./build/debug/code-search add ~/projects/backend --name my-backend
./build/debug/code-search list
./build/debug/code-search search "authenticate"                # every added repo
./build/debug/code-search search --repo my-backend "authenticate"
./build/debug/code-search search -l "TODO"                     # file names only
./build/debug/code-search search --case-sensitive "Foo"
./build/debug/code-search search -- '-test foo'                # a query starting with -
./build/debug/code-search update my-backend                    # pick up new commits
./build/debug/code-search update my-backend --rebuild          # build again anyway
./build/debug/code-search verify my-backend                    # full check of the index
./build/debug/code-search remove my-backend                    # never deletes your code
```

Put the query in quotes, so the shell passes it as one argument:

```sh
./build/debug/code-search search 'auth lang:go -path:_test'    # all three
./build/debug/code-search search 'login or signin'
./build/debug/code-search search '(repo:backend login) or (repo:frontend signIn)'
./build/debug/code-search search '"or" "two words"'            # quotes make it text
./build/debug/code-search search 'case:yes Foo'
./build/debug/code-search search -C 2 --max-results 20 TODO    # context, limit (0: none)
./build/debug/code-search search --json authenticate
./build/debug/code-search search '/auth(enticate|orize)\(/ lang:go'   # a regex
./build/debug/code-search search --color=always TODO | less -R  # colors into a pager
```

Colors are on for a terminal and off for a pipe, as with grep; `--color=always`
or `--color=never` decides. Without either, `NO_COLOR` turns them off. On a terminal, bytes
from files that could control it, like escape sequences, are shown as `\xHH`.
With `--json` the output is always one JSON document, even after an error.

| Query | Meaning |
|-------|---------|
| `foo bar` | both, anywhere in the same file |
| `foo or bar` | either one |
| `-foo` | files without foo |
| `(a or b) c` | parentheses group |
| `"foo bar"`, `\-x` | text as it is |
| `/fo+ (bar\|baz)/` | a regex, in RE2 syntax; matches stay within a line, and `^` and `$` match at each line |
| `repo:name` | only this repo |
| `path:text`, `file:text` | the path contains the text |
| `path:/\.go$/` | the path matches the regex |
| `lang:go` | the language, from the file name |
| `case:yes`, `case:no` | match case or not |

The full rules are in [What a query means](docs/ARCHITECTURE.md#what-a-query-means).

Results look like `my-backend:src/auth.ts:12:14:export function authenticate`.
Exit codes work like grep: 0 found something, 1 found nothing, 2 error. A
search stopped with Ctrl-C prints what it found so far and exits with 130,
even while `--path` is still indexing.
`verify` exits with 0 if the index is fine, 1 if it is damaged, and 2 if it
can't be read at all.

`add` indexes the repo's last commit, not uncommitted changes. New commits show
up after `update`. For a GitHub repo, `update` fetches the latest commit of its
default branch first. If nothing changed, it does nothing. Changing a single
file still rebuilds the whole index, which takes under a second for
golang/go. If a search or `verify` finds a damaged index, it tells you to run
`update <name> --rebuild`. A search skips a damaged repo with a warning and
searches the others; only when no repo is left to search is it an error.
GitHub repos are cloned without a working copy into code-search's own folder.
Only `https://github.com/<owner>/<repo>` URLs are accepted.

Binary files, files over 1 MB, symlinks, submodules, Git LFS pointers and
minified JS and CSS are skipped. `add` prints a summary of what was skipped.

A local repo is added by its top folder, the one with `.git` in it (or the repo
folder itself for a bare repo). Subfolders are refused.

Data is kept in `~/.code-search`, or in `$CODE_SEARCH_HOME` (a full path) if it's set. That
folder must be new, empty or already used by code-search: it refuses a folder
that holds other files. Only one `add`, `update` or `remove` runs at a time; a
second one waits. Searches never wait.

Searches and builds use one thread per core, up to 8. Results still come out
in the same order. On an M4, with googletest, brew and go added, a search for
a rare word takes about 4 ms from start to finish, and indexing go takes under
a second. See
[docs/BENCHMARKS.md](docs/BENCHMARKS.md) for the numbers and how to run the
benchmarks.

To search a repo without adding it, use `--path`. It indexes the repo in memory
every time, which is slow on big repos:

```sh
./build/debug/code-search search --path ~/projects/other "authenticate"
```

## Web UI and HTTP API

```sh
npm --prefix web ci                     # once
npm --prefix web run build              # makes web/dist
./build/debug/code-search serve         # http://127.0.0.1:7700
```

Open http://127.0.0.1:7700 to search from the browser. `--port` picks
another port, and `--ui <folder>` serves the UI from somewhere other than
`web/dist`. Ctrl-C stops the server. It only listens on 127.0.0.1.

The API is under `/api/v1/`. A search gives the same JSON as `search --json`:

```sh
curl 'http://127.0.0.1:7700/api/v1/search?q=authenticate&max_results=20'
curl http://127.0.0.1:7700/api/v1/repos
curl -X POST -H 'Content-Type: application/json' \
     -d '{"source":"/path/to/repo"}' http://127.0.0.1:7700/api/v1/repos
```

All routes, parameters and errors are in
[HTTP API](docs/ARCHITECTURE.md#http-api).

To work on the UI, run `code-search serve` and `npm --prefix web run dev`
together, and open http://localhost:5173. Vite reloads the page on each change
and passes `/api` to the server. `npm --prefix web test` runs the UI tests.

## Formatting

```sh
cmake --build build/debug --target format        # fix formatting
cmake --build build/debug --target format-check  # check only
```

## Benchmarks

```sh
cmake --preset release
cmake --build --preset release
./build/release/benchmarks/codesearch_benchmarks
./build/release/benchmarks/storage_experiment <path-to-repo>  # index size and format choices
```

Results and what we decided from them are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).
