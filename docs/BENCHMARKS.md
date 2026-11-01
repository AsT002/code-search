# Benchmarks

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
| The pool, with all three limits below | 213 MiB |
| With only the 32 MiB budget | 599 MiB |
| With only chunks sized by result size | 319 MiB |

The pool now keeps results the caller hasn't taken to about 32 MiB, sizes
each chunk from the result size per file seen so far (16 files normally,
down to 1), and starts with one 1-file chunk per thread, until there's a
size to go by. The rest of the 213 MiB is each of 8 threads holding one
file's matches while it works on it. The speed of the benchmark queries
didn't change.

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
