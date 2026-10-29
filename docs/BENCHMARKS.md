# Benchmarks

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
