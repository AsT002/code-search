# code-search

A local code search engine written in C++. It indexes Git repositories and finds code in them fast.

Work in progress. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and [docs/DEVELOPMENT_PLAN.md](docs/DEVELOPMENT_PLAN.md) for what's next.

## Requirements

- CMake 3.25 or newer
- Ninja
- A C++20 compiler (Clang or GCC)
- Optional: `clang-format` and `clang-tidy` (on macOS, `brew install llvm`)

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
./build/debug/code-search search -C 2 --max-results 20 TODO    # context, limit
./build/debug/code-search search --json authenticate
```

| Query | Meaning |
|-------|---------|
| `foo bar` | both, anywhere in the same file |
| `foo or bar` | either one |
| `-foo` | files without foo |
| `(a or b) c` | parentheses group |
| `"foo bar"`, `\-x` | text as it is |
| `repo:name` | only this repo |
| `path:text`, `file:text` | the path contains the text |
| `lang:go` | the language, from the file name |
| `case:yes`, `case:no` | match case or not |

The full rules are in [What a query means](docs/ARCHITECTURE.md#what-a-query-means).

Results look like `my-backend:src/auth.ts:12:14:export function authenticate`.
Exit codes work like grep: 0 found something, 1 found nothing, 2 error. A
search stopped with Ctrl-C prints what it found so far and exits with 130.
`verify` exits with 0 if the index is fine, 1 if it is damaged, and 2 if it
can't be read at all.

`add` indexes the repo's last commit, not uncommitted changes. New commits show
up after `update`. For a GitHub repo, `update` fetches the latest commit of its
default branch first. If nothing changed, it does nothing. Changing a single
file still rebuilds the whole index, which takes about 2 seconds for
golang/go. If a search or `verify` finds a damaged index, it tells you to run
`update <name> --rebuild`.
GitHub repos are cloned without a working copy into code-search's own folder.
Only `https://github.com/<owner>/<repo>` URLs are accepted.

Binary files, files over 1 MB, symlinks, submodules, Git LFS pointers and
minified JS and CSS are skipped. `add` prints a summary of what was skipped.

Data is kept in `~/.code-search`, or in `$CODE_SEARCH_HOME` if it's set. Only
one `add`, `update` or `remove` runs at a time; a second one waits. Searches never wait.

To search a repo without adding it, use `--path`. It indexes the repo in memory
every time, which is slow on big repos:

```sh
./build/debug/code-search search --path ~/projects/other "authenticate"
```

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
