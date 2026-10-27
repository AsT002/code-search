# Architecture

code-search is a local code search engine. It indexes Git repositories, either
local ones or public ones cloned from GitHub, and searches them with literals,
filters and regular expressions.

It's built for one machine with tens to a few hundred repositories, so we balance
disk space against search speed.

The milestones are in [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md).

## Principles

1. The engine is a library. The CLI and the later HTTP server are thin layers on top.
2. Every source is a Git repo. A GitHub URL gets cloned first; a local path must
   already be a Git repo. After that, both go through the same code.
3. Each index is a snapshot of one commit. We look up the commit ID once and use
   that ID for every Git command after it.
4. Each build writes a new, read-only index file. The registry says which file is
   current.
5. The index can let extra files through, but it must never drop a real match:

   ```text
   exact_matches(query) ⊆ candidate_documents(query)
   verify(candidate_documents(query), query) == brute_force(query)
   ```

6. Get it right first, then make it fast. Every optimization is tested against a
   simple brute-force version, and every speed claim is measured.

## Layers

```
┌─────────────────────────────────────────────────────────────────────┐
│  FRONTENDS          cli/  (now)            server/ + React  (later) │
└───────────────────────────────┬─────────────────────────────────────┘
                                │ calls
┌───────────────────────────────▼─────────────────────────────────────┐
│  APPLICATION        RepoManager            SearchService            │
│                     add/list/remove/update search across repos      │
└──────┬──────────────────────┬──────────────────────┬────────────────┘
       │                      │                      │
┌──────▼───────┐   ┌──────────▼─────────┐   ┌────────▼────────────────┐
│  repo/       │   │  index/            │   │  query/      search/    │
│  Workspace   │   │  FileFilter        │   │  Parser      Matcher    │
│  Registry    │   │  IndexBuilder      │   │  AST         Evaluator  │
│  Publisher   │   │  ShardWriter       │   │  Planner     Searcher   │
│  (JSON)      │   │  ShardReader(mmap) │   │  TrigramQuery           │
└──────┬───────┘   └──────────┬─────────┘   └────────┬────────────────┘
       │                      │                      │
┌──────▼───────┐   ┌──────────▼──────────────────────▼────────────────┐
│  git/        │   │  util/   MmapFile · AtomicFile · varint ·        │
│  GitRunner   │   │          ThreadPool · FileLock · LittleEndian    │
│  GitRepo     │   │                                                  │
└──────────────┘   └──────────────────────────────────────────────────┘
```

`index/`, `query/` and `search/` never include `git/` or `repo/`. The indexer
takes `(path, bytes)` pairs and doesn't care where they came from. That lets us
test and benchmark the core without Git.

## Modules

| Module    | Job | Main types |
|-----------|-----|------------|
| `git/`    | Run Git without a shell. Clone, fetch, look up a commit ID, list files, read file contents by ID | `GitRunner`, `GitRepo`, `ObjectId`, `BlobStream` |
| `repo/`   | Workspace folders, registry, publishing new index files, cleanup, locking | `Workspace`, `Registry`, `RegistryStore`, `RepoId`, `Publisher` |
| `index/`  | Decide which files to index, extract trigrams, write and read index files | `FileFilter`, `IndexBuilder`, `ShardWriter`, `ShardReader`, `PostingCursor` |
| `query/`  | Parse a query into a tree and plan how to run it | `Parser`, `QueryNode`, `Planner`, `RepoSelection`, `TrigramQuery` |
| `search/` | Find candidate files, check them, collect matches, sort and limit results | `Matcher`, `Evaluator`, `Searcher` |
| `util/`   | Small shared helpers | `Process`, `MmapFile`, `AtomicFile`, `ThreadPool`, `FileLock` |
| `app/`    | Tie the modules together | `RepoManager`, `SearchService` |

### Main interfaces

A sketch. Names may change during implementation.

```cpp
// index/
struct SourceFile {
    std::string path;        // raw bytes from Git, not assumed to be UTF-8
    ObjectId blobId;
    std::string content;
};

class IndexBuilder {
public:
    void add(const SourceFile& file);  // doc IDs go up by one per file
    void writeTo(const std::filesystem::path& path, const ShardHeader& header) const;
};

class PostingCursor {  // walks a posting list without copying it into a vector
public:
    bool done() const;
    uint32_t doc() const;
    void next();
    void advance(uint32_t target);  // move to the first doc >= target
};

// What search code reads from. MemoryIndex (M1) and ShardReader (M3) both
// implement it, so search works the same on either.
class IndexReader {
public:
    virtual ~IndexReader() = default;
    virtual uint32_t docCount() const = 0;
    virtual std::optional<PostingCursor> postings(Trigram trigram) const = 0;
    virtual std::string_view path(DocId doc) const = 0;
    // The content may be mapped or decompressed. The view is only valid inside fn.
    virtual void withContent(DocId doc,
                             const std::function<void(std::string_view)>& fn) const = 0;
};

class ShardReader : public IndexReader {  // mmap'd, read-only, safe to share between threads
public:
    static ShardReader open(const std::filesystem::path& path);
    const ObjectId& commit() const;
};

// query/
struct AllRepos {};
struct SelectedRepos { std::vector<RepoId> ids; };
struct NoRepos {};
using RepoSelection = std::variant<AllRepos, SelectedRepos, NoRepos>;

using TrigramQuery =
    std::variant<TrigramTerm, AndQuery, OrQuery, AllQuery, NoneQuery>;

struct QueryPlan {
    RepoSelection repos;
    QueryNode expression;      // the whole query, kept as a tree
    SearchOptions options;     // case, limits, context lines
};

struct ShardPlan {             // one per repo
    QueryNode expression;      // with repo:X filters already resolved
    TrigramQuery candidates;   // finds at least every matching file
};

// search/
// M1 has LiteralMatcher only. A shared Matcher base class arrives with
// RegexMatcher in M8.
class LiteralMatcher {
public:
    LiteralMatcher(std::string_view pattern, bool caseSensitive);
    // Calls onMatch for each match. Return false from onMatch to stop.
    void findAll(std::string_view text, const std::function<bool(Span)>& onMatch) const;
};

// Built once per search from the whole query, then used on each candidate file.
class Evaluator {
public:
    Evaluator(const QueryNode& query, const SearchOptions& options);
    bool matches(std::string_view content) const;
    std::vector<Span> collectSpans(std::string_view content) const;
};

class Searcher {
public:
    SearchResults run(const QueryPlan& plan,
                      std::span<const ShardReader* const> shards) const;
};
```

## Reading files from Git

```
look up the commit ID (once)
  → git ls-tree -r -z <commit-id>      list every file in the commit
  → sort entries by type
  → FileFilter                          decide what to index
  → git cat-file --batch <blob-ids>     read file contents
  → IndexBuilder
```

Rules:

- Look up the commit ID once. Running `rev-parse HEAD` and then `ls-tree HEAD`
  can mix two commits if HEAD moves in between.
- Ask for file contents by blob ID, not by `<commit>:<path>`.
- Use `-z` so file names with spaces, tabs or newlines come through intact.
- Don't let the `cat-file` pipes fill up and block each other. Keep one request in
  flight at a time or read on a separate thread. Always drain stderr.
- Check a blob's size before reading it into memory.
- Keep paths as raw bytes. Convert them only when showing them or writing JSON.

### Entry types

| Git entry | What we do |
|-----------|------------|
| Normal file | Index it if the filter allows |
| Symlink | Skip and count |
| Submodule | Skip and count |
| Git LFS pointer | Skip and count. Never show the pointer as if it were the real file. Only files that fully match the pointer format in the LFS spec count |
| Missing contents | Stop with an error. Skipping would give results that look complete but aren't |

### What gets indexed

- Binary files are skipped. A file counts as binary if its first 8 KB contain a NUL
  byte. This is a guess, so the rest of the file is still handled safely.
- Files over 1 MB are skipped by default.
- Known generated files like `*.min.js` are skipped by default. Users can change
  this. Lockfiles are not skipped by name; the size limit catches the huge ones.
- Every build reports how many files and bytes it indexed and skipped, and why.

Each index stores two values that describe the rules used to build it:

- `policy_version` goes up when we change what the filter code does.
- `config_fingerprint` is a hash of the actual settings (size limit, exclude
  patterns, overrides, codecs, normalization). If a user changes a setting, the
  fingerprint changes and the repo gets rebuilt.

`ls-tree` lists every file in the commit, including committed files that match
`.gitignore`. That's what we want, since we index the commit.

### Repo support

- SHA-1 repos work. SHA-256 repos are rejected with a clear error for now. The
  file format already has room for 32-byte IDs.
- An empty repo gives an empty, valid index.

## Data on disk

### Workspace

```
~/.code-search/
├── registry.json            the repos and which index file is current for each
├── lock                     held during add, update, remove and cleanup
├── repos/                   clones we own (GitHub repos only)
│   └── r_7f3a.git/          bare, shallow, one branch, no tags
├── indexes/
│   ├── r_1c2d-g0004.idx     current index for my-backend
│   ├── r_7f3a-g0002.idx     current index for frontend
│   └── r_7f3a-g0001.idx     old index, deleted once nothing points to it
└── tmp/                     builds in progress
```

- Files and folders are named by internal IDs like `r_7f3a`. The name a user gives
  with `--name` is only a label in the registry and never becomes a path.
- Local repos stay where they are. `remove` deletes the registry entry and index
  files, never the user's code.
- Clones we own have no checked-out files. We read from Git's object store, so
  a checkout would only waste disk.

`registry.json`:

```json
{
  "version": 1,
  "repos": [
    {
      "id": "r_1c2d", "name": "my-backend",
      "source": "local", "path": "/Users/me/projects/backend",
      "track": "HEAD",
      "index": { "file": "r_1c2d-g0004.idx", "commit": "a1b2c3…",
                 "format_version": 1, "policy_version": 1,
                 "config_fingerprint": "9e41…" }
    },
    {
      "id": "r_7f3a", "name": "frontend",
      "source": "github", "url": "https://github.com/example/frontend",
      "track": "remote-default-branch",
      "index": { "file": "r_7f3a-g0002.idx", "commit": "d4e5f6…",
                 "format_version": 1, "policy_version": 1,
                 "config_fingerprint": "9e41…" }
    }
  ]
}
```

The registry is accessed through a `RegistryStore` interface, so we can move it
to SQLite later without changing other code.

### Index file (`.idx`)

All numbers are little-endian with fixed sizes. We read each field explicitly and
never cast mapped bytes to C++ structs.

```
r_1c2d-g0004.idx
┌──────────────────────────────────────────────────────────────────┐ offset 0
│ HEADER (fixed size)                                              │
│   magic            "CSIX"                                        │
│   format_version   1                                             │
│   required_features / optional_features   (bit flags)            │
│   hash_algo, hash_len, commit[32]          (SHA-1 uses 20 bytes) │
│   policy_version   1                                             │
│   config_fingerprint[32]                                         │
│   doc_count, trigram_count                                       │
│   section table: offset, length and codec for each section       │
├──────────────────────────────────────────────────────────────────┤
│ TRIGRAM TABLE (sorted by trigram, so we can binary search it)    │
│   ┌─────────┬───────────┬─────────────────┬────────────┬──────┐  │
│   │ trigram │ doc_count │ postings_offset │ byte_len   │ rsvd │  │
│   │ u32     │ u32       │ u64             │ u32        │ u32  │  │
│   └─────────┴───────────┴─────────────────┴────────────┴──────┘  │
│   24 bytes per entry                                             │
├──────────────────────────────────────────────────────────────────┤
│ POSTINGS (sorted doc IDs, one list per trigram)                  │
│   stored as raw u32 or delta-varint, set by the section codec    │
├──────────────────────────────────────────────────────────────────┤
│ DOC TABLE (fixed-size entries, so doc_id is an array index)      │
│   path_off, path_len                                             │
│   content_off, stored_len, decoded_len, content_codec            │
│   lang_id, flags, blob_oid[32], reserved                         │
├──────────────────────────────────────────────────────────────────┤
│ PATHS (raw path bytes, back to back)                             │
├──────────────────────────────────────────────────────────────────┤
│ CONTENTS (one block per file, original bytes)                    │
│   v1 stores them raw. Later options: LZ4 or Zstandard per file   │
└──────────────────────────────────────────────────────────────────┘ EOF
   (each section starts on an 8-byte boundary)
```

Why it looks like this:

- We mmap the whole file. Opening it reads only the header and section table.
- The trigram table is sorted and every entry is the same size, so a lookup is a
  binary search.
- Doc table entries are the same size too, so finding a file by doc ID is
  `table_start + id × entry_size`.
- Trigrams are built from lowercased text, but contents are stored as written.
  One index serves both case-sensitive and case-insensitive search.
- Codec fields exist from the start. Adding compression later means a new codec
  ID, not a new layout.
- A file with a required feature flag we don't know fails to open with a clear
  error.

### Checking index files

When opening: magic, version, feature flags, section bounds (with overflow
checks, no overlaps) and counts against section sizes.

When reading: doc IDs in range, path and content bounds, posting list bounds,
varints that end properly, and a limit on decompressed size.

Full checks, like sorted trigram keys, increasing doc IDs and checksums, live in a
separate `code-search verify` command. That keeps normal opens fast.

### Index size

We measure index size instead of guessing. For the raw format:

```text
C = bytes of indexed content
P = for each file, its number of distinct trigrams, summed
T = distinct trigrams in the index
N = number of files
D = size of one doc table entry
L = bytes of paths

index_bytes = C + 4·P + 24·T + N·D + L + header and padding
```

Raw postings can be several times the content size for some files, and the
stored contents add another 1×. M2.5 gives us real numbers. Every build reports:

- bytes per section, and bytes per byte of indexed source
- total disk used by the workspace, including clones
- peak disk during a rebuild (old index, new index and temp files)
- peak memory while building
- what was skipped and why

### Publishing a new index

The registry says which index file is current. Renaming one file and then
updating another isn't atomic, so we do it in a fixed order.

`add`, `update`, `remove` and cleanup each hold one lock from start to finish.
Without it, one process's cleanup could delete another process's file in `tmp/`,
or a finished index before the registry points to it. Searches don't take this
lock and keep working while a change runs.

1. Take the lock.
2. Build into `tmp/` under a unique name.
3. Check the new file, fsync it, and rename it into `indexes/` with its final
   name. Nothing points to it yet.
4. Write the new registry to a temp file, fsync it, and rename it over the old one.
5. Delete index files the registry no longer points to, and anything left in
   `tmp/`. Release the lock.

Since `remove` can't run during a build, a build can't finish after a `remove` and
bring the repo back.

Searches open the file the registry names. If cleanup deleted it in the meantime,
they read the registry again and retry once. A file that's already open stays
readable after it's deleted, because POSIX keeps it alive until it's closed.

At startup we clean `tmp/` and unused index files, also while holding the lock.

One lock means only one build runs at a time, even for different repos. That's
fine to start with. If it becomes a problem, we'll switch to a lock per repo plus
a short lock for the registry.

What we promise about crashes: if the process is killed at any point, the current
state is still complete and usable. We don't promise anything about power loss
(on macOS that would need `F_FULLFSYNC`). Checks run lazily and won't catch every
kind of damage. If we do find a damaged index, on open, on read or with `verify`,
we rebuild that repo.

## Main flows

```
code-search add <url|path> [--name n]
  check the input (local: a Git repo; remote: https://github.com/<owner>/<repo>)
  → pick a RepoId → (clone --bare --depth 1 --single-branch --no-tags)
  → look up the commit ID once → read files → build → publish

code-search update <name> [--rebuild]
  (fetch if we own the clone) → look up the commit ID once
  → skip if commit, format_version, policy_version and config_fingerprint all
    match and the index opens fine, unless --rebuild
  → otherwise build → publish

code-search search "<query>"
  parse → plan
  → for each selected repo (a limited number at a time):
       plan for this repo → find candidates → check each one → collect matches
  → merge in a fixed order, apply limits → print
```

Local repos follow their `HEAD`. Clones we own follow the remote's default
branch. A force-push is fine, because every build looks up a fresh commit.

## What a query means

These are the starting choices. We'll lock them in before M6, when the JSON
output becomes stable.

| Question | Answer |
|----------|--------|
| `foo bar` | Both words anywhere in the same file |
| Queries with no search text, like `lang:go` or `-foo` | Return matching files, with nothing highlighted |
| What `--max-results` counts | Matching lines |
| Result order | Repo, then path, then position in the file. Same every run |
| Case | Case-insensitive by default. `case:yes` makes it case-sensitive. Only ASCII letters are folded, without using the system locale (no `tolower`) |
| Case-insensitive non-ASCII | Not supported in v1 |
| Positions | Byte offsets inside the engine. Line and column are worked out for display |
| Line endings | A line ends at LF. A CR before it stays in the content and is hidden on display |
| `path:` and `file:` | Substring match on the path. `file:` also takes a regex |
| `repo:` | Exact repo name |
| Incomplete results | Results say if they're complete, cut off by a limit, or cancelled |

Checking a file has two steps. `matches` decides yes or no using the
whole query. `collectSpans` then finds what to highlight, only for files that
matched. For `foo -bar`, a file isn't shown until we've checked it has no `bar`.

## How a search runs

```
 query string
     │
     ▼
 ┌──────────┐   "auth lang:go -path:_test"
 │  PARSER  │
 └────┬─────┘
      ▼
 ┌──────────┐   AND( Literal("auth"), Lang(go), Not(Path("_test")) )
 │ QUERY AST│
 └────┬─────┘
      ▼
 ┌──────────┐   1. which repos could match
 │ PLANNER  │   2. for each repo: replace repo:X with true or false, simplify
 └────┬─────┘   3. for each repo: build a TrigramQuery that finds at least
      │            every matching file
      ▼  for each repo
 ┌──────────────────────────────────────────────────────────────┐
 │ candidates = run(TrigramQuery)                               │
 │   for each candidate:                                        │
 │     Evaluator.matches(content)       uses the whole query    │
 │     Evaluator.collectSpans(content)                          │
 └───────────────────────────────┬──────────────────────────────┘
                                 ▼
                     results, in order, within limits
```

### Why we keep the whole query

If we split a query into separate "repos", "filters" and "text" parts, we can't
handle `(repo:A foo) or (repo:B bar)`. Split up, it becomes
`(repo:A or repo:B) and (foo or bar)`, which wrongly returns `bar` from A. So:

- The final check always sees the whole query, simplified for that repo.
- The planner only pulls out parts that hold for every branch.
- We don't expand queries into an OR of ANDs. That can blow up in size.
- If a part is hard to turn into trigrams, we use `All` and let the final check
  decide. Slower, but always correct.

### Turning a query into trigrams

| Query part | TrigramQuery |
|------------|--------------|
| Text of 3+ bytes | `And` of its trigrams (lowercased) |
| Text shorter than 3 bytes | `All` |
| `A and B` | `And[A, B]` |
| `A or B` | `Or[A, B]`. If either side is `All`, the result is `All` |
| `not A` | `All` in v1. A missing trigram does prove `A` is absent; we can use that later |
| Filters like `lang:` or `path:` | `All` here. The final check applies them |
| Regex | `All` in v1. See below |

Never take the opposite of a candidate list. Its extra files would turn into
missed matches.

How each part runs:

- `And`: step through the posting lists together, rarest first, and stop as soon
  as one runs out.
- `Or`: merge the posting lists.
- `All`: every file. `None`: no files.

A trigram that isn't in the table means `None` for that part. If we ever drop
very common trigrams from the index, we must mark them as dropped and treat them
as `All`, not as missing.

### Example

```
code-search search "authenticate"

 1. EXTRACT   "authenticate" → lowercase → trigrams
              aut uth the hen ent nti tic ica cat ate
 2. LOOK UP   binary search each one in the trigram table
 3. ORDER     rarest first: "nti"(41) "tic"(88) "aut"(212) …
 4. INTERSECT step through the lists together, stop early if empty
              → candidates {3, 17, 42}
 5. CHECK     withContent(3, …) → matches → collectSpans
 6. OUTPUT    src/auth/login.ts:12:14:  export async function authenticate(user) {
```

### Regex

We use RE2, never `std::regex`. RE2 runs in linear time, so a bad pattern can't
hang a search.

1. v1: a regex becomes `All`, and RE2 checks every file the rest of the query
   allows. Correct, and something to measure against.
2. v2: use RE2's `FilteredRE2` to skip files. It returns pieces of text that
   *might* be needed, combined through its own AND/OR tree. They aren't all
   required: `/foo|bar/` must become `foo OR bar`, never `foo AND bar`. We only
   add rules that tests show are safe.
3. v3, only if measurements say it's worth it: our own regex-to-trigram analysis.

We lowercase a regex for the trigram step only if RE2 folds case the same way
(ASCII only). Otherwise the regex falls back to `All`.

### Symbol search (later)

`sym:authenticate` needs a symbol table, built at index time with universal-ctags
or tree-sitter. It would be one more kind of query part.

## Security

- Git runs without a shell (`posix_spawn` with an argument list). That stops shell
  injection, but Git still reads options, URL schemes and config, so we also:
  - only accept `https://github.com/<owner>/<repo>` (with or without `.git`)
  - put `--` before file and ref arguments where Git supports it
  - never store passwords or tokens in URLs or logs
- Local repos use the user's own Git config. The user already trusts it.
- Folder names come from internal IDs, never from user input. Before deleting a
  clone, we check that its real path (after following symlinks) is inside
  `~/.code-search/repos/`.
- The reader treats index files as untrusted. See "Checking index files".
- The HTTP server (M9) listens on localhost only, limits request size, supports
  cancelling, escapes file content in HTML, and protects anything that changes
  state.

## Decisions

| Decision | Why |
|----------|-----|
| C++20, CMake, library plus CLI | Speed, and one engine for every frontend |
| Git repos only | Commit snapshots, cheap change checks, Git stores the files |
| Look up the commit ID once | HEAD can move between commands |
| Index the commit, not the working tree | Each index is one consistent snapshot. Uncommitted changes can come later |
| Bare, shallow, one-branch clones | We read from Git objects, so checked-out files are wasted disk |
| A new read-only file per build | Remove is a delete, update is a build plus publish, repos can be searched in parallel |
| Registry points to the current file | Two files can't be swapped together, but one pointer can |
| Rebuild when commit, format, filter code or settings change | Changes to our code or the user's settings must take effect |
| One lock for add, update, remove and cleanup | Cleanup can't delete another build's files. Searches don't need it |
| Little-endian, fields read one by one | Works everywhere, and avoids undefined behavior from casting mapped bytes |
| Codec fields from v1 | M2.5 can pick compression without changing the layout |
| `withContent` callback | Works the same for mapped and decompressed content |
| Posting cursors | No copying a list into a vector for every query term |
| Keep the whole query for the final check | Splitting it gives wrong results for OR across filters |
| `RepoSelection` type | A filter that matches no repo must not turn into "all repos" |
| ASCII-only case folding | Doesn't depend on the system locale, and matches how trigrams are built |
| RE2, full scan first | A correct baseline before any shortcuts |
| JSON registry behind `RegistryStore` | Little data, one writer at a time. SQLite if we ever need it |
| Internal IDs for paths | User-given names never become file paths |

### Not doing for now

| Idea | Why not |
|------|---------|
| Surviving power loss (directory fsync, `F_FULLFSYNC`, fault testing) | Surviving a killed process is enough for a personal tool. A damaged index we find gets rebuilt |
| Compression on by default | Start raw. M2.5 measurements pick the default |
| Speed targets that fail the build, starting in M1 | We record numbers from M1 and set targets after the M7 baseline |
| SHA-256 repos | Rejected with a clear error. The format already has room |
| Full benchmark method from day one | Moved to M7 |
| Thread-safety tests, codec tests, fuzzing the Git reader | Added when that code exists |
| Position-based index, merging index segments, sharing data across repos, custom regex analysis, ranking, background service | Not until real use shows we need them |

### Things to watch

| Risk | When to act |
|------|-------------|
| One changed file rebuilds the whole repo | Update time or disk writes get too high |
| One file per very large repo | Build memory or time gets too high. Then split big repos into several files |
| Hundreds of tiny repos | Per-repo overhead dominates. Then pack small repos together |
| Common trigrams match too many files | Searches scan too much. Then try storing positions or splitting files into chunks |
| Reading contents from Git instead of storing them | Only after testing cold reads and `git gc` |
| Indexing uncommitted changes | Design it as a separate snapshot type when we want it |
| Old index files and caches piling up in memory | Add limits before the long-running server |

## Source tree

```
code-search/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/                      warnings, tools, pinned dependencies
├── include/codesearch/
│   ├── git/        git_runner.hpp  git_repo.hpp  object_id.hpp
│   ├── repo/       workspace.hpp  registry.hpp  publisher.hpp
│   ├── index/      trigram.hpp  file_filter.hpp  index_builder.hpp
│   │               shard_format.hpp  shard_writer.hpp  shard_reader.hpp  postings.hpp
│   ├── query/      ast.hpp  parser.hpp  planner.hpp  trigram_query.hpp
│   ├── search/     matcher.hpp  evaluator.hpp  searcher.hpp  result.hpp
│   ├── app/        index_commit.hpp  repo_manager.hpp  search_service.hpp
│   └── util/       process.hpp  utf8.hpp  mmap_file.hpp  atomic_file.hpp  varint.hpp  endian.hpp
│                   thread_pool.hpp  file_lock.hpp
├── src/                        same layout as include/
├── cli/main.cpp                builds the `code-search` program
├── tests/
│   ├── unit/                   one file per module
│   ├── cli/                    runs the real `code-search` program
│   ├── differential/           index search vs brute force
│   ├── integration/            Git to search, start to finish
│   ├── fuzz/                   index reader, query parser
│   └── support/                brute-force search, temp Git repos, a fake `git` for broken output
├── benchmarks/
└── docs/
```

## CLI

```
code-search add ~/projects/backend               # local Git repo
code-search add https://github.com/example/frontend
code-search add ~/projects/backend --name my-backend
code-search list
code-search search "authenticate"
code-search search "authenticate" --repo my-backend
code-search search 'auth lang:go -path:_test'
code-search search '/auth(enticate|orize)/'
code-search update my-backend
code-search update my-backend --rebuild
code-search remove my-backend                    # never deletes your code
code-search verify my-backend                    # full check of the index file
```

## Testing

The main check is a brute-force scan of the same files. It must share no code
with the planner; otherwise a planner bug would hide itself.

| Tests | What they check |
|-------|-----------------|
| Unit | Each module |
| Candidates | Every real match survives the trigram step |
| Literal search vs brute force | Same files and matches. Includes empty, short, common and missing queries, non-ASCII bytes, CRLF, no final newline, long lines and duplicate files |
| Boolean queries | `repo:A foo or repo:B bar`, path filters inside OR, queries with only `-` terms or only filters, unknown repo, precedence and quotes |
| Regex vs brute force | Same results as RE2 run on every file with the same options |
| Git | Pinned commit while HEAD moves, uncommitted changes, file names with newlines, tabs or a leading dash, symlinks, submodules, LFS pointers, huge files, SHA-256 rejection. The user's repo is never changed |
| `git grep` | `git grep <commit-id>` on the same files, using syntax both support |
| Publishing | Kill the process after every write, fsync and rename. The current state stays complete, cleanup removes leftovers, a second change waits for the lock, and searches keep working |
| Fuzzing | Opening and reading damaged index files; the query parser with deep nesting and long input. We keep the inputs that found bugs |
| Sanitizers | ASan and UBSan now. TSan once there's threaded code |
