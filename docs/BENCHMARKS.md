# Benchmarks

## Against other indexed tools: codesearch and zoekt

ripgrep has no index: it reads every file on every search, so beating it
with an index built beforehand says little. The comparison that matters is
with tools built the same way, and it has to include what the index costs.

- Russ Cox's codesearch (`cindex` and `csearch`, v1.2.0): the trigram index
  this project's design comes from ("Regular Expression Matching with a
  Trigram Index", 2012). Its index holds only posting lists, and it reads
  candidate files from disk.
- Sourcegraph's zoekt (commit `a5a933bc`): its index holds file contents and
  trigram positions, in shards. Searched with `zoekt-webserver`, and from the
  command line with `benchmarks/zoekt_all`, zoekt's own CLI with its match
  limits raised: by default zoekt stops after 100,000 matches per shard, then
  ranks, so it doesn't return every match the way the others do.

livegrep, another indexed tool, isn't included: it builds with Bazel, which
wasn't set up on this machine, and its build often fails on macOS. Its index
is a suffix array rather than trigrams, so it would be a comparison worth
adding on Linux.

Setup: Apple M4 (10 cores), 16 GiB, macOS, release build, warm file cache,
load average about 2. 7 runs of each command, the tools taking turns. Two
sets of files: the Linux kernel fork ripgrep's benchsuite uses
(BurntSushi/linux at `84e57d2922`: 79,435 tracked files, 1,244 MiB) with the
suite's `linux_*` patterns, and googletest, brew and go at the commits in
`benchmarks/repos.tsv` (19,866 files, 171 MiB) with the queries in
`benchmarks/queries.tsv` all three can run. Each tool leaves out files over
1 MB, except csearch, which has no size limit but skips files with invalid
UTF-8, a line over 2,000 bytes or too many trigrams. zoekt's symbol index
(ctags) is off. Made by `benchmarks/vs_indexed.py` (see "Run it yourself"
under M7).

"Fresh" is a new process for each search, printing every matching line to a
pipe. "Warm" is a server that's already running, over HTTP: `code-search
serve` and `zoekt-webserver` (csearch has none). A warm "page" is 200
matching lines, as a UI asks for; zoekt finds up to its own limits, ranks,
then shows 200. "Faster" names the faster tool by median.

### Linux: build

| Tool | Build | Peak memory | Index size |
|---|---:|---:|---:|
| code-search | 5.1 s | 2159 MiB | 1,037 MiB |
| csearch (Cox) | 11.9 s | 1185 MiB | 150 MiB |
| zoekt | 68.9 s | 1035 MiB | 2,486 MiB |

### Three repos: build

| Tool | Build | Peak memory | Index size |
|---|---:|---:|---:|
| code-search | 1.1 s | 307 MiB | 157 MiB |
| csearch (Cox) | 2.9 s | 323 MiB | 28 MiB |
| zoekt | 4.4 s | 639 MiB | 359 MiB |

### Linux: search (ms, median of 7)

| Benchmark | Fresh: code-search | csearch | Faster | zoekt | Faster | Warm, all: code-search | zoekt | Faster |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| linux_literal_default | 17.3 | 204 | cs 12x | 41.3 | cs 2.4x | 4.95 | 1.18 | zoekt 4.2x |
| linux_literal_casei | 18.5 | 618 | cs 33x | 44.5 | cs 2.4x | 6.94 | 3.60 | zoekt 1.9x |
| linux_re_literal_suffix | 114 | 289 | cs 2.5x | 219 | cs 1.9x | 99.5 | 192 | cs 1.9x |
| linux_word | 56.2 | 204 | cs 3.6x | 41.6 | zoekt 1.4x | 57.3 | 2.09 | zoekt 27x |
| linux_unicode_greek | 271 | 3,262 | cs 12x | 2,229 | cs 8.2x | 304 | 3,273 | cs 11x |
| linux_unicode_greek_casei | 276 | 3,275 | cs 12x | 2,275 | cs 8.3x | 304 | 3,370 | cs 11x |
| linux_unicode_word | 288 | 3,335 | cs 12x | 4,067 | cs 14x | 304 | 4,941 | cs 16x |
| linux_no_literal (ASCII) | 330 | 3,336 | cs 10x | 8,157 | cs 25x | 298 | 9,000 | cs 30x |
| linux_alternates | 84.8 | 701 | cs 8.3x | 41.8 | zoekt 2.0x | 79.1 | 1.89 | zoekt 42x |
| linux_alternates_casei | 140 | 930 | cs 6.6x | 42.9 | zoekt 3.3x | 147 | 4.15 | zoekt 35x |

### Three repos: search by group (ms, median of the queries' medians)

| Group | Queries | Fresh: code-search | csearch | Faster | zoekt | Faster | Warm page: code-search | zoekt | Faster | Warm, all: code-search | zoekt | Faster |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| rare | 6 | 8.87 | 30.1 | cs 3.4x | 42.2 | cs 4.8x | 1.90 | 1.59 | zoekt 1.2x | 1.91 | 1.79 | same |
| missing | 4 | 7.15 | 4.75 | csearch 1.5x | 35.5 | cs 5.0x | 0.77 | 0.68 | zoekt 1.1x | 0.77 | 0.68 | zoekt 1.1x |
| common | 6 | 31.0 | 450 | cs 15x | 117 | cs 3.8x | 1.75 | 36.0 | cs 21x | 35.7 | 155 | cs 4.3x |
| short | 6 | 69.4 | 920 | cs 13x | 628 | cs 9.1x | 1.64 | 185 | cs 113x | 98.2 | 731 | cs 7.4x |
| case | 2 | 10.2 | 99.2 | cs 9.7x | 40.3 | cs 3.9x | 1.54 | 3.88 | cs 2.5x | 3.06 | 7.12 | cs 2.3x |
| long | 4 | 7.47 | 14.4 | cs 1.9x | 34.6 | cs 4.6x | 0.94 | 0.39 | zoekt 2.4x | 0.91 | 0.40 | zoekt 2.3x |
| regex | 8 | 20.6 | 78.5 | cs 3.8x | 464 | cs 23x | 2.96 | 436 | cs 147x | 13.5 | 444 | cs 33x |

### Linux: peak memory while searching (fresh process)

| Benchmark | code-search | csearch | zoekt |
|---|---:|---:|---:|
| linux_literal_default | 177 MiB | 9 MiB | 186 MiB |
| linux_literal_casei | 178 MiB | 17 MiB | 226 MiB |
| linux_re_literal_suffix | 415 MiB | 13 MiB | 235 MiB |
| linux_word | 177 MiB | 9 MiB | 185 MiB |
| linux_alternates | 299 MiB | 15 MiB | 166 MiB |
| linux_alternates_casei | 544 MiB | 24 MiB | 172 MiB |

### Linux: matching lines

| Benchmark | code-search | csearch | only cs / only csearch | zoekt | only cs / only zoekt |
|---|---:|---:|---:|---:|---:|
| linux_literal_default | 39 | 39 | 0 / 0 | 39 | 0 / 0 |
| linux_literal_casei | 536 | 536 | 0 / 0 | 536 | 0 / 0 |
| linux_re_literal_suffix | 2,132 | 2,160 | 0 / 28 | 2,132 | 0 / 0 |
| linux_word | 9 | 9 | 0 / 0 | 9 | 0 / 0 |
| linux_unicode_greek | 105 | 105 | 0 / 0 | 105 | 0 / 0 |
| linux_unicode_greek_casei | 245 | 245 | 0 / 0 | 245 | 0 / 0 |
| linux_unicode_word | 247 | 247 | 0 / 0 | 247 | 0 / 0 |
| linux_no_literal (ASCII) | 719 | 719 | 1 / 1 | 1,108 | 1 / 390 |
| linux_alternates | 92 | 140 | 0 / 48 | 92 | 0 / 0 |
| linux_alternates_casei | 193 | 241 | 0 / 48 | 193 | 0 / 0 |

The kernel has 13 pairs of files whose names differ only in case. On
macOS's disk, which ignores case, one of each pair exists, and that's what
csearch and zoekt index; code-search reads both from Git. The lines only
csearch finds come from files over 1 MB that the others leave out. The lines
only zoekt finds come from its regexes matching across lines (its `\s` can
match a newline); code-search and csearch match within a line, like grep.

### What it shows

- Building: code-search builds its index fastest (2.3x faster than
  csearch and 13x faster than zoekt on the kernel), and its index is 6 to 7x
  the size of csearch's and 0.4x zoekt's. Peak memory while building the
  kernel's index is the highest of the three (2.2 GiB).
- Against csearch, both started fresh: code-search is faster on every kernel
  benchmark (2.5x to 33x) and on every group of the other set but one,
  because it reads the contents of candidate files from its mapped index on
  8 threads, where csearch opens each file from disk on one. csearch is
  faster on words that aren't there (4.8 ms against 7.2 ms): with nothing to
  read, the cost is starting the program, and csearch's is lower. csearch
  uses far less memory while searching (9 to 24 MiB against 13 to 544 MiB),
  since it holds no contents.
- Against zoekt, both warm: zoekt is faster on plain text with few matches
  and on regexes made of plain words (`PM_RESUME` 1.2 ms against 5.0, `\bPM_RESUME\b`
  2.1 ms against 57, the 4-word alternation 1.9 ms against 79): its index
  keeps where each trigram is, so it confirms a match without scanning the
  file, where code-search runs RE2 over each candidate. code-search is faster
  on other regexes (1.9x to 30x on the kernel, 33x on the other set's median)
  and on pages of common words (21x to 113x), because it stops after 200
  lines where zoekt collects matches up to its limits and ranks them first.
- Started fresh, zoekt takes about 35 ms before searching, loading its
  shards; it's built to run as a server.

So the trade-off: code-search builds fastest and searches regexes fastest,
for an index 6 to 7x the size of codesearch's; zoekt answers literal searches
fastest, for an index 2.4x the size of code-search's and a build 13x longer.
The search method is Cox's: a trigram index and RE2. What's different is
around it: file contents in the index, a parallel search that keeps results
in order, a query language, commit snapshots that updates swap in while
searches run, and an index that's checked as untrusted. These numbers are
from macOS, where opening files costs more than on Linux, which favors the
two tools that keep contents in their index.

## After M9: output and regex threads

Profiled with `sample` on an Apple M4 (10 cores), release build, warm file
cache, over six repos (redis, fmt, express, flask, gin, ripgrep: 2,883
files). Times are medians, in ms, for all results. "CLI" runs a fresh
process each time, with output to /dev/null; "HTTP" is a running server.

What was slow, and what changed:

1. Text output wrote each line in about 15 `std::cout <<` calls, each a
   locked `fwrite`. Now each file's output is built in one string and
   written at once.
2. JSON escaping added one byte at a time, and numbers went through
   `std::to_string`. Now the bytes between escapes are copied in one append,
   the string is reserved up front, and numbers use `std::to_chars`.
3. Every pool thread searched with the same RE2 object, which takes a lock
   on each search. With dense matches the threads queued on it, so 10
   threads were as slow as one. Now each thread gets its own copy of a
   regex query's evaluator, with the pattern compiled again.
4. The web UI drew every result line. Now it draws whole files up to about
   2,000 lines and a file view up to 3,000 (or past the line it opens at),
   with a button for more, and plain text gets no element of its own.

| Query | CLI text, before | after | CLI JSON, before | after | HTTP, before | after |
|-------|----:|----:|----:|----:|----:|----:|
| `e` (2.1M lines) | 417 | 105 | 208 | 139 | 234 | 161 |
| `/.+/` | 380 | 81 | 246 | 115 | 261 | 127 |
| `the` | 44.7 | 16.9 | 35.2 | 21.3 | 33.5 | 19.3 |
| `func` | 14.1 | 9.4 | 12.1 | 9.3 | 7.8 | 5.3 |

Search alone (`search_bench`, no output), by threads:

| Query | 1, before | after | 4, before | after | 10, before | after |
|-------|----:|----:|----:|----:|----:|----:|
| `/.+/` | 225 | 213 | 98.0 | 61.6 | 218 | 40.2 |
| `e` | 181 | 180 | 58.7 | 52.3 | 38.4 to 44 | 38.7 to 44 |

Small queries were already bound by starting the program (about 3.5 ms,
against 1.5 ms for an empty program) and don't change.

Web UI, rendering to HTML in Node (a browser also builds the DOM, so it
takes longer):

| Response | Lines | Before | After |
|----------|------:|-------:|------:|
| limit 200 | 200 | 11 | 10 |
| limit 200, 3 lines of context | 1,103 | 25 | 23.5 |
| limit 5,000, 3 lines of context | 23,957 | 514 to 569 | 40 (2,018 drawn) |
| no limit, `func` | 15,221 | 592 | 82 (2,063 drawn) |
| file view, redis `src/server.c` | 8,734 | 139 | 51 (3,000 drawn) |

Memory: a regex that fills RE2's DFA cache now fills one per thread. For
`/(a|b).{30}(a|b){30}c/` peak memory went from 53.8 to 67.3 MiB (and the
time from 80 to 60 ms); for ordinary regexes it didn't change. Each copy
stays under the 64 MiB cap (see M8).

Not changed: the time limit on highlighting still reads the clock before
each regex search. It's about 7% of the pool threads' busy time on `/.+/`,
but reading it less often could let a pathological regex run past the
limit.

## M8: Regex

Same machine, repos and method as M7 below: warm runs with `search_bench`
(median of 5 rounds), fresh processes with `run_benchmarks.py` (median of 3).
The 8 regex queries are in `benchmarks/queries.tsv`. v1 checks every file
with RE2; v2 first narrows the files with RE2's prefilter tree.

### What the trigram step saves

Files and bytes of content RE2 has to check, out of 18,829 files and
130.2 MiB, and the time for all results, warm, in ms:

| Query | Files, v1 | v2 | MiB, v1 | v2 | 1 thread, v1 | v2 | 8 threads, v1 | v2 |
|-------|----:|----:|----:|----:|----:|----:|----:|----:|
| `/func \w+\(ctx context\.Context/` | 18,829 | 214 | 130.2 | 5.7 | 40.4 | 2.50 | 10.0 | 0.86 |
| `/(TODO\|FIXME\|XXX):/` | 18,829 | 751 | 130.2 | 20.7 | 213.3 | 46.5 | 42.1 | 8.77 |
| `/^import \(/` | 18,829 | 6,425 | 130.2 | 75.3 | 203.6 | 123.3 | 42.7 | 26.7 |
| `/\berr != nil \{$/` | 18,829 | 2,871 | 130.2 | 37.7 | 222.8 | 82.1 | 44.8 | 15.3 |
| `/[A-Z][a-z]+Error\b/` | 18,829 | 7,345 | 130.2 | 82.2 | 224.9 | 152.4 | 45.7 | 29.4 |
| `/MustCompile\("/` | 18,829 | 172 | 130.2 | 7.0 | 37.1 | 2.61 | 9.51 | 0.71 |
| `/\d{4}-\d{2}-\d{2}/` | 18,829 | 18,829 | 130.2 | 130.2 | 205.2 | 207.5 | 41.9 | 41.0 |
| `/^\s*#include <vector>/` | 18,829 | 36 | 130.2 | 2.2 | 201.2 | 3.82 | 40.9 | 1.70 |

v2 checks between 1.7% and 63% of the content when the regex has text it
needs, and the time follows. `\d{4}-\d{2}-\d{2}` has none, so both check
everything. That's also why v3, our own analysis of what a regex needs,
isn't worth building: it can't narrow a regex with no required text either,
and where there is some, v2 already finds it.

`func \w+\(...` and `MustCompile\("` are fast even in v1: RE2 skips ahead
quickly to a literal prefix like `func ` in a file.

The `MustCompile` row was measured again after a later fix (v2 was 124
files, 4.7 MiB). Without `case:yes`, RE2 matches KELVIN SIGN for `k` and
LONG S for `s`, so only the text between those letters is required:
`tcompile("`, not `mustcompile("`.

### Fresh-process regex searches

ms, median of 3. `rg -i -e` searches the same checkouts with the same
pattern, for reference; its regex syntax covers these. (`git grep`'s doesn't
have `\d` or `\b`, so it's left out.)

| Query | First output | All | rg |
|-------|----:|----:|----:|
| `/func \w+\(ctx context\.Context/` | 4.77 | 5.34 | 222.2 |
| `/(TODO\|FIXME\|XXX):/` | 11.9 | 15.2 | 219.8 |
| `/^import \(/` | 7.43 | 33.6 | 239.1 |
| `/\berr != nil \{$/` | 6.84 | 23.6 | 226.8 |
| `/[A-Z][a-z]+Error\b/` | 8.38 | 37.5 | 221.8 |
| `/MustCompile\("/` | 4.96 | 5.23 | 219.5 |
| `/\d{4}-\d{2}-\d{2}/` | 6.82 | 49.3 | 220.9 |
| `/^\s*#include <vector>/` | 6.08 | 6.37 | 223.5 |

p50 / p95 over the 8: 15.2 / 49.3 ms for all results, against 222 / 239 ms
for `rg`. The first output comes 2 to 3 ms later than for text queries,
mostly from compiling the regex.

## M7: Performance

### Setup

- Apple M4 (10 cores), 16 GB, macOS, release build, warm file cache.
- Repos, pinned in `benchmarks/repos.tsv`. All three are added to one
  workspace, so every search covers all of them:

  | Repo | Commit | Files indexed | Content | Index |
  |------|--------|--------------:|--------:|------:|
  | google/googletest | a8380b01a2 | 252 | 3.9 MiB | 5.2 MiB |
  | Homebrew/brew | 1f55ebf835 | 3,228 | 18.4 MiB | 23.2 MiB |
  | golang/go | 9dd31ba8ff | 15,349 | 107.9 MiB | 129.0 MiB |

- Queries: 40 in 8 groups, in `benchmarks/queries.tsv`: rare words, missing
  words, common words, 1 to 2 byte text, filter-heavy, `or`/`-`,
  case-sensitive, and long phrases with many trigrams. Case-insensitive unless
  the query says `case:yes`.
- Warm runs: `search_bench` opens the workspace once and runs each query 5
  times in the same process, after one run to warm the cache. It reports the
  median time to the first result, to the first 10, to all results, and to
  finish with `--max-results 10`. Groups show p50 / p95 over their queries,
  by the nearest-rank method. With 2 to 6 queries a group, p95 is the
  slowest query, and for the 2 case queries p50 / p95 is just the faster and
  the slower one.
- Fresh-process runs: `run_benchmarks.py` starts `code-search search` for
  each query (3 runs, median), reads all of its output and times the first
  byte and the end. Writing to a pipe, the program sends its output in 16 KiB
  blocks, so "first output" is when the first block is full or the program
  ends, not when the first result is found. For a few results, it's the end.
- `rg -F` and `git grep -F` search the same checkouts, for the queries that
  are plain text. `git grep` runs once per repo, and its time is the three
  runs added up. Neither is a like-for-like race: both read files code-search
  skips (minified, over 1 MB), and `rg` skips hidden files (like `.github/`),
  which code-search and `git grep` search. They're a reference.
- Stages: each speedup was built and measured on top of the ones before it,
  from the M6 code (stage 0) to the final code (stage 5).

Run it yourself:

```sh
cmake --preset release && cmake --build --preset release
python3 benchmarks/run_benchmarks.py <work dir> --code-search build/release/code-search
./build/release/benchmarks/search_bench <work dir>/ws benchmarks/queries.tsv --threads 8
```

To compare with other indexed tools (see "Against other indexed tools"
above), `benchmarks/vs_indexed.py` uses the same work dir and needs Go: its
docstring says how to build csearch, zoekt and `benchmarks/zoekt_all`.

```sh
python3 benchmarks/vs_indexed.py <work dir> --code-search build/release/code-search --go-bin <dir>
```

For a fuller comparison with ripgrep, `benchmarks/vs_ripgrep.py` uses the
same work dir. It checks that both tools find the same lines, times all
results, files only and the first 10 lines, and compares repo sizes, a
running server, peak memory and what the index costs. It also runs
ripgrep's own benchsuite patterns (the 11 `linux_*` ones) on the Linux kernel
fork that suite uses, which it clones (about 1.5 GB). `--no-linux` skips
that part. Run it on a quiet machine: it prints the load average, and above a
few the times are higher for both tools.

```sh
python3 benchmarks/vs_ripgrep.py <work dir> --code-search build/release/code-search
```

### Where the time went

The M6 code spent under 0.3 ms finding candidates for every query, even the
long phrases. The rest went to checking files. A profile of `the` on one
thread:

| Part | Share |
|------|------:|
| Case-insensitive `LiteralMatcher::find` | 55% |
| `resultLines` (finding every line start of each matching file) | 22% |
| `locate` and the search loop | 10% |
| malloc and free | 9% |

So the first two speedups below weren't on the plan's list, but were the
biggest ones.

### Each speedup, before and after

Warm, all results, ms. Stages 0 to 2 on one thread, the rest on 8.

| Query | 0: M6 | 1: find | 2: lines | 3: pool | 4: `-` and `or` | 5: intersect |
|-------|------:|--------:|---------:|--------:|----------------:|-------------:|
| `the` | 258.2 | 144.4 | 71.0 | 17.1 | 16.4 | 16.1 |
| `func` | 212.8 | 119.8 | 54.6 | 13.8 | 13.9 | 13.5 |
| `a` (2.1 million lines) | 530.4 | 489.3 | 438.2 | 107.3 | 103.2 | 99.7 |
| `ParseDuration` | 18.7 | 3.36 | 2.35 | 0.72 | 0.70 | 0.70 |
| `case:yes Context` | 18.3 | 18.3 | 7.81 | 2.04 | 2.02 | 1.97 |
| `lang:go -TODO` | 81.9 | 13.5 | 13.5 | 4.16 | 2.38 | 2.28 |
| `TODO or FIXME` | 147.3 | 42.7 | 20.3 | 4.39 | 3.20 | 3.26 |
| `func -return` | 71.4 | 23.3 | 14.1 | 4.59 | 4.13 | 3.93 |
| `notawordatall9` | 1.22 | 0.17 | 0.18 | 0.19 | 0.20 | 0.13 |
| `"func (s *Server) ServeHTTP("` | 0.97 | 0.26 | 0.25 | 0.27 | 0.27 | 0.17 |
| `"def initialize(name, version)"` | 5.90 | 1.04 | 1.02 | 0.54 | 0.59 | 0.46 |

1. **Literal search.** The case-insensitive search compared the pattern at
   every position, byte by byte. It now tests 32 start positions per step for
   the pattern's first and last bytes, in a loop with no branches that the
   compiler turns into vector instructions, and only compares the positions
   that pass in full. For a lowercase letter, `b | 0x20 == c` matches both
   cases in one compare.

   Case-sensitive search used `string_view::find` (memchr, then memcmp). The
   new filter was faster for patterns starting with a common byte and slower
   for a rare one (ms, all results, one thread, with the line changes of
   step 2):

   | Query | memchr | filter |
   |-------|-------:|-------:|
   | `case:yes the` | 100.5 | 62.0 |
   | `case:yes func` | 53.2 | 49.6 |
   | `case:yes Context` | 8.00 | 11.3 |
   | `case:yes TODO` | 7.49 | 9.16 |
   | `case:yes ParseDuration` | 1.25 | 2.05 |

   Lowercase letters and spaces are the most common bytes in code, so
   case-sensitive patterns starting with one of those use the filter, and
   the rest use memchr. That picks the faster one for all five.

   Both can compare a whole pattern at almost every position of repetitive
   text, which costs text size times pattern size: a 64 KiB pattern in
   1 MiB took about 19 s. Patterns over 64 bytes, rare in practice, now use
   Knuth-Morris-Pratt, which never looks back at the text (0.06 s there).

2. **Result lines.** `resultLines` found every line start of a matching file
   to show a few lines. It now jumps to each matching line (a match's column
   says where its line starts) and walks only the lines it shows. `locate`
   counts the newlines between matches with `std::count`, one vectorized
   pass, instead of a byte loop or one memchr per line.

3. **Thread pool.** One pool, one thread per core up to 8, checks files in
   chunks of 16 across all repos. Matching, highlighting and result lines all
   run on the pool. The calling thread passes the chunks on in order, so the
   output is the same as with one thread. Building the lines on the calling
   thread at first capped `the` at 28 ms however many threads there were.

   | Threads | `the` |
   |--------:|------:|
   | 1 | 70.7 |
   | 2 | 38.4 |
   | 4 | 21.2 |
   | 8 | 17.0 |

   The first result comes about 0.1 ms later than on one thread, the cost of
   handing work to the pool (table below). `a` stays at about 100 ms: 2.1
   million result lines are passed on by one thread.

4. **`-` and `or` use the index.** For a literal under `-` or in an `or`
   branch, the evaluator looks up which files have all of its trigrams. A
   file without them can't contain it, so the literal isn't searched for
   there. On one thread, `lang:go -TODO` went from 13.5 to 3.61 ms and
   `TODO or FIXME` from 20.3 to 14.6 ms. Queries without `-` or `or` don't
   change.

5. **Stop intersecting.** `And` used to step through all of its lists
   together. Now it goes list by list from the shortest, and stops before a
   list with more than 256 entries per file still left: checking those few
   files is cheaper than walking the list. Whole search, 8 threads, ms:

   | Query | Step through all | List by list | Stop at ≤ 32 files | Stop by cost, 1024 | Stop by cost, 256 |
   |-------|---:|---:|---:|---:|---:|
   | `xyzzyplugh` | 0.02 | 0.02 | 0.18 | 0.02 | 0.02 |
   | `notawordatall9` | 0.19 | 0.20 | 0.13 | 0.15 | 0.12 |
   | `"func (s *Server) ServeHTTP("` | 0.26 | 0.28 | 0.15 | 0.26 | 0.16 |
   | `"def initialize(name, version)"` | 0.53 | 0.55 | 0.39 | 0.55 | 0.46 |

   For these queries, finding candidates is most of the search. A fixed
   "stop at 32 files" hurt missing words, which end with 0 or 1 files after
   every list. The cost rule doesn't, so it's the one kept. The savings are a
   few tenths of a millisecond.

6. **Stop early with a limit.** Already done in M6: results stream out, so a
   limited search stops reading files once it has enough. The "Limit 10"
   column below shows it. With the pool, a few chunks past the limit are
   read: at most 4 per thread.

7. **Bitmaps or direct scans for common trigrams.** Not built. Finding
   candidates takes at most 0.3 ms for any of the 40 queries, while checking
   files takes the rest, so there's nothing for them to win.

8. **Parallel indexing** (see Build below).

### Warm searches, all groups

ms, p50 / p95. Before: stage 0 on one thread. After: the final code (stage 5
with the review fixes below) on 8 threads.

| Group | First, before | after | First 10, before | after | All, before | after | Limit 10, before | after |
|-------|----:|----:|----:|----:|----:|----:|----:|----:|
| rare | 0.28 / 5.71 | 0.28 / 0.65 | 0.68 / 10.0 | 0.28 / 0.79 | 10.6 / 33.8 | 0.53 / 0.97 | 0.69 / 10.1 | 0.34 / 0.83 |
| missing | 0.00 / 1.22 | 0.00 / 0.11 | 0.00 / 1.22 | 0.00 / 0.11 | 0.00 / 1.22 | 0.00 / 0.11 | 0.00 / 1.22 | 0.00 / 0.11 |
| common | 0.01 / 0.79 | 0.09 / 0.32 | 0.07 / 0.83 | 0.13 / 0.33 | 211.8 / 258.2 | 9.56 / 15.9 | 0.09 / 0.84 | 0.18 / 0.40 |
| short | 0.01 / 0.07 | 0.06 / 0.09 | 0.01 / 0.14 | 0.07 / 0.11 | 281.0 / 530.4 | 17.5 / 102.3 | 0.01 / 0.17 | 0.14 / 0.18 |
| filter | 0.06 / 0.46 | 0.11 / 0.74 | 0.20 / 0.48 | 0.12 / 0.86 | 25.4 / 81.9 | 1.89 / 2.68 | 0.20 / 0.51 | 0.20 / 0.94 |
| boolean | 0.07 / 8.60 | 0.11 / 0.84 | 0.53 / 8.60 | 0.17 / 0.84 | 71.4 / 147.3 | 3.01 / 4.07 | 0.54 / 8.60 | 0.23 / 1.10 |
| case | 0.01 / 0.04 | 0.06 / 0.23 | 0.10 / 0.10 | 0.15 / 0.24 | 18.3 / 28.6 | 1.63 / 1.93 | 0.11 / 0.12 | 0.31 / 0.32 |
| long | 0.35 / 5.90 | 0.29 / 0.49 | 0.97 / 5.90 | 0.34 / 0.49 | 5.32 / 11.0 | 0.48 / 0.69 | 0.98 / 5.91 | 0.38 / 0.49 |

Where a query finds its first results in the first few files, they come
about 0.1 ms later than on one thread: the cost of handing work to the pool.

### Fresh-process searches

ms, p50 / p95. Each run starts the program, opens the three index files and
prints the results as text to a pipe. Before: stage 0. After: the final
code. `rg` and `git grep` are from the same run as "after".

| Group | First output, before | after | All, before | after | rg | git grep |
|-------|----:|----:|----:|----:|----:|----:|
| rare | 11.9 / 19.9 | 3.40 / 4.04 | 12.3 / 39.5 | 3.82 / 4.68 | 217 / 291 | 252 / 256 |
| missing | 2.68 / 4.01 | 2.80 / 2.88 | 2.68 / 4.01 | 2.80 / 2.88 | 217 / 231 | 253 / 258 |
| common | 4.24 / 5.70 | 3.96 / 4.31 | 289.4 / 415.9 | 57.5 / 121.9 | 245 / 262 | 251 / 254 |
| short | 3.39 / 6.43 | 3.72 / 5.76 | 447.6 / 1633 | 140.1 / 1103 | 261 / 316 | 261 / 454 |
| filter | 3.16 / 6.43 | 3.30 / 5.03 | 38.7 / 120.5 | 9.08 / 16.0 | | |
| boolean | 11.4 / 21.5 | 3.41 / 5.93 | 92.0 / 163.0 | 7.68 / 13.6 | | |
| case | 4.27 / 4.29 | 3.53 / 3.56 | 26.6 / 36.3 | 6.64 / 7.18 | 225 / 231 | 257 / 258 |
| long | 8.44 / 15.2 | 3.51 / 3.59 | 8.44 / 15.5 | 3.55 / 3.74 | 218 / 231 | 255 / 258 |

The script first rejected `Deprecated:` as a filter, so `rg` and `git grep`
had 5 rare queries to code-search's 6. It now treats only `repo:`, `path:`,
`file:`, `lang:` and `case:` as filters, like code-search does.

About 3 ms of every fresh run is starting the program and opening the
indexes. For big result sets, printing is now most of the time: `the` takes
16 ms warm but 122 ms here, and `a` (2.1 million lines) takes 1.1 s, against
316 ms for `rg`.
The text output, on one thread, is the next thing to speed up.

The first run of the script passed `-s` to `git grep` for case-sensitive
queries. To `git grep` that's an unknown option, so it failed in 14 ms, and
the script counted that as a time. The script now checks exit codes.

### Build

| Repo | add, before | after | Peak memory, before | after | No-op update |
|------|----:|----:|----:|----:|----:|
| googletest | 0.09 s | 0.06 s | 18 MiB | 23 MiB | 15 ms |
| brew | 0.37 s | 0.17 s | 57 MiB | 71 MiB | 16 ms |
| go | 1.95 s | 0.73 s | 283 MiB | 306 MiB | 16 ms |

Half of a build went to finding each file's trigrams (mostly sorting them).
That now runs on the pool while the calling thread reads files from Git, and
the files are added in their usual order, so the index comes out the same.
At most 16 MiB of read content waits for the pool, which bounds the extra
memory. The first version allowed 64 MiB and kept each file's trigram vector
at its full starting size, 4 bytes per byte of content: go peaked at
597 MiB. Shrinking those vectors and lowering the limit brought it to
306 MiB. The no-op update is the same before and after: it checks the commit
and the index header, and builds nothing.

### Searches during an update

Fresh-process searches of the rare, common and boolean queries, while
`update go --rebuild` runs in a loop (ms, p50 / p95 over the queries):

| | Nothing else running | During the update |
|-|----:|----:|
| Before | 93.0 / 407.0 | 89.1 / 437.3 |
| After | 8.00 / 121.3 | 10.2 / 131.2 |

Searches don't take the lock, so they never wait. The median got 27% slower,
because the build uses the same cores.

### Memory for searches without a limit

A review found that the pool could hold far more results than one thread
does. It read ahead up to 4 chunks of 16 files per thread, and kept each
file's whole result until the caller took it. Searching 100 files of 4096
lines of `e` × 63 for `e`, with output to `/dev/null` (peak memory, whole
process):

| Version | Peak memory |
|---------|------:|
| One thread (stage 2) | 54 MiB |
| The pool, as first committed | 1342 MiB |
| The pool, with the limits below | 188 to 214 MiB |

The pool keeps results the caller hasn't taken to about 32 MiB, counting
the bytes as they're made: a task stops at the next file once that's
reached, and the rest of its chunk is checked next. It also sizes and hands
out chunks by the result size per file seen so far (16 files normally, down
to 1), and starts with one 1-file chunk per thread, until there's a size to
go by. The rest of the memory is each of 8 threads holding one file's
matches while it works on it. The speed of the benchmark queries didn't
change.

The first version of this fix relied on the sizes seen so far alone. A
second review showed why that isn't enough: after 64 files without matches,
files where every line matches looked cheap, and a test of that held about
65 MiB with 4 threads. Counting the bytes made keeps that test at 38 MiB.

### Targets

Set from the "after" numbers of this milestone, with room to spare, for this
machine and these repos. The baseline was far slower, so targets based on it
would say nothing about keeping these gains. A change that misses one should
say why.

| What | Target |
|------|--------|
| Warm search, rare, missing, long, filter and boolean groups | p95 under 5 ms |
| Warm search, common group | p95 under 25 ms |
| Warm search, first result or `--max-results 10` | under 2 ms |
| Fresh-process search, rare, missing and long groups | p95 under 10 ms |
| Fresh-process search, common group | p50 under 100 ms |
| Building go | under 1.0 s, peak memory under 3 times its content (324 MiB) |
| No-op update | under 50 ms |
| Searches during an update | p50 at most 30% slower than without |

### Limits

- One machine, warm cache, one run of each script.
- 40 queries is few; a p95 over 6 queries is the slowest one.
- The fresh-process numbers include printing the results to a pipe that
  Python reads, which is slower than writing to a file.

## M2.5: Storage experiments

These measurements pick the defaults for the M3 index file.

### Setup

- Apple M4, macOS, release build, one thread, warm file cache.
- `storage_experiment <repo>` indexes HEAD with the default filter. It then
  stores every posting list and every file in both formats, in memory.
- Queries: 300 strings of 4 to 12 characters (no spaces), copied from random
  places in the repo with a fixed seed. Most of them are common, so each one
  returns many candidates. Search is case-insensitive, like the CLI default.
- Each timing is the fastest of 5 rounds over all 300 queries. Every number in
  a row comes from that same round.
- "Intersect" means finding candidates in the posting lists. After that, every
  candidate file is checked in two ways:
  - "First match" stops at the first match, which is enough to list files (`-l`).
  - "Full search" does what `search()` does: finds every match and works out its
    line and column.
- Sizes use the formula in the architecture doc, with a 64-byte doc table
  entry and a 4 KiB header.

Run it yourself:

```sh
cmake --preset release
cmake --build --preset release
./build/release/benchmarks/storage_experiment <path-to-repo>
```

### Repos

| Repo | Commit | Files | Content | Trigrams (T) | Posting entries (P) |
|------|--------|------:|--------:|-------------:|--------------------:|
| google/googletest | cd8e73f | 252 | 3.9 MiB | 34,429 | 456,306 |
| Homebrew/brew | c6069d2a94 | 3,205 | 18.1 MiB | 66,752 | 2,854,505 |
| golang/go | 33e2e22c | 15,331 | 107.7 MiB | 183,783 | 14,817,334 |

### Postings: raw u32 vs delta-varint

Delta-varint stores the first doc ID, then the gap to each next one, 7 bits
per byte. The raw cursor jumps ahead by galloping (1, 2, 4, 8... entries, then
a binary search). The varint cursor has to step through every entry.

| Repo | Raw | Varint | Varint size | Intersect raw | Intersect varint |
|------|----:|-------:|------------:|--------------:|-----------------:|
| googletest | 1.7 MiB | 0.4 MiB | 25% | 0.22 ms | 0.43 ms |
| brew | 10.9 MiB | 2.9 MiB | 27% | 1.52 ms | 2.91 ms |
| go | 56.5 MiB | 15.3 MiB | 27% | 5.62 ms | 9.48 ms |

Varint is about a quarter of the size and about half as fast to intersect. In
both formats, intersecting takes under 1% of the search time (see the content
timings below).

An earlier raw cursor did a binary search over the rest of the list on every
jump. It was slower than varint (31.8 ms on go), because most jumps are short.
`PostingCursor` worked that way too, until M3 switched it to galloping.

### Contents: raw vs LZ4 per file

| Repo | Raw | LZ4 | LZ4 size |
|------|----:|----:|---------:|
| googletest | 3.9 MiB | 1.6 MiB | 40% |
| brew | 18.1 MiB | 6.8 MiB | 37% |
| go | 107.7 MiB | 43.6 MiB | 40% |

Compressing everything took under 0.01 s, 0.02 s and 0.14 s.

Time to check every candidate:

| Repo | Candidates | First match, raw | First match, LZ4 | Full search, raw | Full search, LZ4 | Decompressing |
|------|-----------:|-----------------:|-----------------:|-----------------:|-----------------:|--------------:|
| googletest | 19,334 | 213 ms | 340 ms (+60%) | 857 ms | 909 ms (+6%) | 104 to 110 ms |
| brew | 102,282 | 660 ms | 942 ms (+43%) | 1,768 ms | 2,036 ms (+15%) | 278 to 283 ms |
| go | 598,469 | 2,641 ms | 4,172 ms (+58%) | 8,987 ms | 10,631 ms (+18%) | 1,518 to 1,548 ms |

The full search found 198,094, 748,071 and 2,760,682 matches.

Decompressing costs the same in both modes, about 2.6 µs per candidate on go.
A full search does more work per file, so the extra cost is a smaller share of
it: 6 to 18% slower, against 43 to 60% when stopping at the first match. LZ4
makes the contents 60% smaller.

### Index size by section

| Repo | Trigram table | Doc table | Paths | Raw + raw | Varint + raw | Varint + LZ4 |
|------|--------------:|----------:|------:|----------:|-------------:|-------------:|
| googletest | 0.8 MiB | <0.1 MiB | <0.1 MiB | 6.5 MiB (1.65×) | 5.2 MiB (1.32×) | 2.8 MiB (0.73×) |
| brew | 1.5 MiB | 0.2 MiB | 0.2 MiB | 30.9 MiB (1.71×) | 22.9 MiB (1.27×) | 11.6 MiB (0.64×) |
| go | 4.2 MiB | 0.9 MiB | 0.6 MiB | 170.0 MiB (1.58×) | 128.8 MiB (1.20×) | 64.7 MiB (0.60×) |

The × is the size compared with the indexed source. Postings and contents are
almost all of it. The tables and paths are small.

### Peak memory while building

| Repo | Content | Peak memory |
|------|--------:|------------:|
| googletest | 3.9 MiB | 14 MiB |
| brew | 18.1 MiB | 51 MiB |
| go | 107.7 MiB | 227 MiB |

This is the whole process after building the in-memory index, about 2.1× the
content for go. It was the same over three runs, though one earlier run read
179 MiB, so treat it as approximate. The in-memory index keeps a copy of every
file and a `u32` per posting entry.

### Defaults for M3

- Postings: delta-varint. A quarter of the size. Intersecting is slower, but it
  is a tiny part of a search. The raw codec stays available.
- Contents: raw. A full search is 6 to 18% faster and listing files is 43 to
  60% faster. A raw file can also be read straight from the mapped index, with
  no copy and no decompressed-size checks. LZ4 would halve the whole index, so
  it's a closer call than the postings. It can come later as a codec option for
  large repos where disk space matters more than speed.
- With these, an index is about 1.2 to 1.3× the size of the source.

### Limits

- One machine, one run per repo, warm cache.
- Contents and postings were in memory, not in a mapped file.
- The queries are random substrings. Real searches are often rarer words with
  fewer candidates, where intersecting is a bigger share of the time.
