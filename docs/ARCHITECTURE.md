# Architecture

code-search is a local code search engine. It indexes Git repositories, either
local ones or public ones cloned from GitHub, and searches them with literals,
filters and regular expressions.

It's built for one machine with tens to a few hundred repositories, so we balance
disk space against search speed.

The milestones are in [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md).

## Principles

1. The engine is a library. The CLI and the HTTP server are thin layers on top.
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
│  FRONTENDS          cli/                   server/ + web/ (React)   │
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
| `git/`    | Run Git without a shell. Clone, fetch, look up a commit ID, list files, read file contents by ID | `GitRunner`, `GitRepo`, `BlobStream` |
| `repo/`   | Workspace folders, registry, publishing new index files, cleanup, locking | `Workspace`, `Registry`, `RegistryStore`, publishing steps (`writeToTmp`, `moveIntoIndexes`, `cleanUp`) |
| `index/`  | Decide which files to index, extract trigrams, write and read index files | `FileFilter`, `IndexBuilder`, `ShardWriter`, `ShardReader`, `PostingCursor` |
| `query/`  | Parse a query into a tree and plan how to run it | `parseQuery`, `QueryNode`, `resolveRepo`, `planCandidates`, `TrigramQuery` |
| `search/` | Find candidate files, check them, collect matches, limit and format results | `LiteralMatcher`, `Evaluator`, `searchRepos`, `JsonResultWriter` |
| `util/`   | Small shared helpers, and `ObjectId`, which both `git/` and `index/` use | `Process`, `MmapFile`, `ObjectId`, `AtomicFile`, `ThreadPool`, `FileLock` |
| `app/`    | Tie the modules together | `RepoManager`, `openRepos`, `SearchService`, `IndexCache` |

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
ParsedQuery parseQuery(std::string_view text);  // throws QueryError
// The query for one repo: repo:X becomes true or false, then it's simplified.
// False means nothing in that repo can match, so the repo is skipped.
QueryNode resolveRepo(const QueryNode& query, std::string_view repoName);
TrigramQuery planCandidates(const QueryNode& query);  // finds at least every match

using TrigramQuery =
    std::variant<AllQuery, NoneQuery, TrigramTerm, AndQuery, OrQuery>;

// search/
// Two matchers with the same calls. No shared base class: the evaluator
// knows which one each part of the query has, so it never needs one.
class LiteralMatcher {
public:
    LiteralMatcher(std::string_view pattern, bool caseSensitive);
    // Calls onMatch for each match. Return false from onMatch to stop.
    void findAll(std::string_view text, const std::function<bool(Span)>& onMatch) const;
};

class RegexMatcher {  // RE2. Throws std::invalid_argument for a bad pattern
public:
    RegexMatcher(std::string_view pattern, bool caseSensitive);
    bool foundIn(std::string_view text) const;  // even an empty match
    // Non-empty matches only, left to right, without overlaps.
    void findAll(std::string_view text, const std::function<bool(Span)>& onMatch) const;
};

// Built once per repo from its resolved query, then used on each candidate file.
class Evaluator {
public:
    Evaluator(const QueryNode& query, const SearchOptions& options);
    bool matches(std::string_view path, std::string_view content) const;
    std::vector<Span> collectSpans(std::string_view path, std::string_view content) const;
};

// Searches repo after repo and hands each matching file to onFile as soon as
// it's found, so a limit keeps memory small.
SearchStatus searchRepos(std::span<const SearchTarget> targets, const SearchRequest& request,
                         const std::function<void(const ResultFile&)>& onFile);
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

- `policy_version` goes up when we change which files get indexed or how their
  content is read. Version 2 ignores Git's replacement objects.
- `config_fingerprint` is a hash of the actual settings (for now the size
  limit and exclude patterns; overrides, codecs and normalization will join
  them when they become settings). If a user changes a setting, the
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
├── code-search-workspace    marks the folder as ours
├── registry.json            the repos and which index file is current for each
├── lock                     held during add, update, remove and cleanup
├── repos/                   clones we own (GitHub repos only)
│   └── r_7f3a90b1.git/      bare, shallow, one branch, no tags
├── indexes/
│   ├── r_1c2d4e5f-g0004.idx current index for my-backend
│   ├── r_7f3a90b1-g0002.idx current index for frontend
│   └── r_7f3a90b1-g0001.idx old index, deleted once nothing points to it
└── tmp/                     builds in progress
```

The folder is `$CODE_SEARCH_HOME` if that's set (it must be a full path),
otherwise `~/.code-search`.
It must be missing, empty, or already a workspace (it has
`code-search-workspace`, or `registry.json` for workspaces made before the
marker). Otherwise `add`, `update` and `remove` refuse to start, so pointing
`$CODE_SEARCH_HOME` at a folder like `~` can't hand its `tmp/` to cleanup.
The marker is the first thing written, and it's written in place rather than
through a temp file, so a first `add` killed at any point leaves a folder that
the next one accepts.

`add`, `update` and `remove` write an empty `registry.json` before they
build anything. If it's missing later while `indexes/` or `repos/` have
files in them, they refuse to run instead of cleaning up, since cleanup
would delete every index file and clone the registry doesn't list.

- Files and folders are named by internal IDs like `r_7f3a90b1` (8 random hex
  digits). The name a user gives
  with `--name` is only a label in the registry and never becomes a path.
- Local repos stay where they are. `remove` deletes the registry entry, the
  index files and, for a GitHub repo, our clone; never the user's code.
- Clones we own have no checked-out files. We read from Git's object store, so
  a checkout would only waste disk.
- Repo names may use letters, digits, `.`, `_` and `-`, and can't start with `.`
  or `-`. The default is the folder's or the GitHub repo's name.
- A local repo's path is stored after following symlinks. Folders inside the
  workspace can't be added.
- A local repo must be added by its top folder (`git rev-parse --show-toplevel`,
  or `--absolute-git-dir` for a bare repo). Git finds a repo from any folder
  inside it, so a subfolder would index the whole repo, and the same repo could
  be added twice under two names.
- Search and `verify` check that an index file's header matches its registry
  entry (commit, format, filter rules and settings). A valid index of another
  repo copied over it is reported as not matching, not searched as this repo.
- Every Git we start runs without the variables `git rev-parse --local-env-vars`
  lists, like `GIT_DIR` and `GIT_WORK_TREE`. Git sets them for hooks and
  aliases, and they would override `-C` and `--git-dir`.
- Every Git we start also gets `GIT_NO_REPLACE_OBJECTS=1`. Otherwise Git
  shows `git replace` objects in place of the real ones, and those can change
  while the commit id the index is saved under stays the same.
- `update` checks that a local repo's folder is still the top folder of a
  repo. If its `.git` was deleted, Git would find a repo around it, and index
  that one in its place.

`registry.json`:

```json
{
  "version": 1,
  "repos": [
    {
      "id": "r_1c2d4e5f", "name": "my-backend",
      "source": "local", "path": "/Users/me/projects/backend",
      "index": { "file": "r_1c2d4e5f-g0004.idx", "commit": "a1b2c3…",
                 "format_version": 1, "policy_version": 2,
                 "config_fingerprint": "9e41c0d2a7b35f16" }
    },
    {
      "id": "r_7f3a90b1", "name": "frontend",
      "source": "github", "url": "https://github.com/example/frontend",
      "index": { "file": "r_7f3a90b1-g0002.idx", "commit": "d4e5f6…",
                 "format_version": 1, "policy_version": 2,
                 "config_fingerprint": "9e41c0d2a7b35f16" }
    }
  ]
}
```

Local repos follow `HEAD` and GitHub clones follow the remote's default branch,
so the source says what to track. Loading checks every field: IDs, names, the
index file name (which must be `<id>-g<number>.idx`, so it can never point
outside `indexes/`), and that no ID or name is used twice. Any problem is an
error naming the file; we never guess.

The registry is accessed through a `RegistryStore` interface, so we can move it
to SQLite later without changing other code. The JSON store uses nlohmann/json.

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
│   policy_version   2                                             │
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
│   delta-varint by default, or raw u32. Set by the section codec  │
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

Full checks live in a separate `code-search verify` command, which keeps normal
opens fast. It checks that trigram keys are sorted, that every posting list
decodes to exactly its length with increasing doc IDs, that every path and file
is in range, and that the posting lists match the trigrams of the stored
contents. That last check works like a checksum for the postings and most
content damage. There is no separate checksum field yet.

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
stored contents add another 1×. In M2.5, delta-varint postings with raw contents
came to 1.2 to 1.3× the source size (see [BENCHMARKS.md](BENCHMARKS.md)). Each
build prints how many files and bytes it indexed, and what it skipped and why.
Sizes per section, index size and peak memory are measured by the benchmarks,
not printed by every build.

### Publishing a new index

The registry says which index file is current. Renaming one file and then
updating another isn't atomic, so we do it in a fixed order.

`add`, `update`, `remove` and cleanup each hold one lock from start to finish.
Without it, one process's cleanup could delete another process's file in `tmp/`,
or a finished index before the registry points to it. Searches don't take this
lock and keep working while a change runs.

1. Take the lock (`flock`, so it's released if the process dies). Clean up.
2. Build into `tmp/`. The file is written with fsync before its final rename.
3. Open the new file to check its header, commit and file count, and rename it
   into `indexes/`. Nothing points to it yet.
4. Write the new registry to a temp file, fsync it, and rename it over the old one.
5. Delete index files the registry no longer points to, clones of repos it
   doesn't list, and builds left in `tmp/`. Release the lock.

If a step fails, the same cleanup runs right away against the registry on
disk. Cleanup first checks that `tmp/`, `indexes/` and `repos/` are real
folders inside the workspace, not symlinks, and stops with an error if not. It
only deletes names code-search makes (`r_7f3a90b1-g0001.idx`, its
`.tmp-XXXXXX` file while it's written, and `r_7f3a90b1.git`), and leaves
anything else alone. It only deletes entries directly inside the folders, checks that after following
symlinks, and deletes a symlink itself, never what it points to. It also
deletes `registry.json.tmp-*` files, which a registry save killed partway
leaves next to `registry.json`, and `code-search-workspace.tmp-*` files, which
an earlier version left the same way when writing the marker. If it can't
delete something, the error names it and suggests checking its permissions.

Since `remove` can't run during a build, a build can't finish after a `remove` and
bring the repo back.

Searches open the file the registry names. If cleanup deleted it in the meantime,
they read the registry again and retry once. A file that's already open stays
readable after it's deleted, because POSIX keeps it alive until it's closed.

There's no cleanup when the program starts: `add`, `update` and `remove` clean
up at their start and end, while holding the lock. Searches and `list` never
delete anything.

One lock means only one build runs at a time, even for different repos. That's
fine to start with. If it becomes a problem, we'll switch to a lock per repo plus
a short lock for the registry.

What we promise about crashes: if the process is killed at any point, the current
state is still complete and usable. We don't promise anything about power loss
(on macOS that would need `F_FULLFSYNC`). Checks run lazily and won't catch every
kind of damage. If `update` finds the index won't open, it rebuilds it. Damage
found on read or with `verify` doesn't show on open, so those errors tell the
user to run `update <name> --rebuild`.

## Main flows

```
code-search add <url|path> [--name n]
  check the input (local: a Git repo; remote: https://github.com/<owner>/<repo>)
  → pick a RepoId → (clone --bare --depth 1 --single-branch --no-tags)
  → look up the commit ID once → read files → build → publish

code-search update <name> [--rebuild]
  (if we own the clone: check it has no symlinks and Git writes only inside
   it, then fetch origin HEAD and move our branch to it)
  → look up the commit ID once
  → skip if commit, format_version, policy_version and config_fingerprint all
    match and the index opens fine, unless --rebuild
  → otherwise build → publish

code-search search "<query>"
  parse → plan
  → open every selected repo's index, then for each one:
       plan for this repo → find candidates → check each one → collect matches
  → merge in a fixed order, apply limits → print
```

Local repos follow their `HEAD`. Clones we own follow the remote's default
branch. A force-push is fine, because every build looks up a fresh commit.

## What a query means

Locked in with M6. The JSON output depends on these, so changing one later
is a breaking change.

| Question | Answer |
|----------|--------|
| `foo bar` | Both words anywhere in the same file |
| `foo or bar` (or `OR`) | Either one. `or` binds looser than the space: `a b or c` is `(a b) or c` |
| `-foo` | Files without `foo`. A `-` on its own, or before another `-` (`--verbose`), is text |
| `( )` | Groups. Inside a word, parentheses are text when they balance: `printf(` and `f(x)` |
| Quotes and backslashes | `"foo bar"` keeps spaces, `or`, `-` and `:` as text, with `\"` and `\\` inside. Outside quotes, `\` makes the next character text. Pieces join like in a shell: `pre"fix"` |
| Queries with no search text, like `lang:go` or `-foo` | Return matching files, with nothing highlighted |
| What's highlighted | The text in the parts of the query that matched: not under a `-`, and not in an `or` branch that didn't match |
| `path:` and `file:` | Substring of the file's path, with the query's case rule. `path:/re/` and `file:/re/` take a regex |
| `/re/` | A regex in RE2 syntax (never `std::regex`), so no pattern can make a search slow. A match never spans lines, as with grep: `\n` never matches, and `[^x]+` and `\C+` stop at a line's end. `^` and `$` match at each line; `$` is before the LF, so with CRLF it's after the CR (use `\r?$`). Without `case:yes`, RE2 folds case its own way, non-ASCII letters too (unlike text, which folds only ASCII). Backslashes reach RE2 as typed, except `\/`, which stands for `/` |
| What makes a regex | A word that starts with `/`, up to the first `/` after it that's followed by a space, `)` or the end. If there's none, or the pattern would be empty, it's text: `/usr/bin` and `// TODO` search for those words, and `// /foo/` is `//` and the regex `foo`. A regex RE2 can't compile is an error with RE2's message. A plain word that ends in `/.../` with regex syntax inside (`[](){}*+?|^$`), like `4./[x]*/`, is still text, but gets a warning that suggests the regex (`/4\.[x]*/`). Paths like `src/server/` don't |
| Regex matches | The leftmost match, then the next one after it, like grep. An empty match makes the file match, but only non-empty matches are highlighted: `/x*/` matches every file. For a pathological regex and file, after a time limit only the first match in each line is highlighted (see Regex below) |
| `lang:` | The language from the file name (extension, or names like `Makefile`). Names and aliases are case-insensitive: `lang:c++` is `lang:cpp`. An unknown language is an error |
| `repo:` | Exact repo name. A name that isn't added is an error. Not allowed with `--path` |
| Case | Case-insensitive by default. `case:yes` or `case:no` anywhere in the query applies to all of it, and wins over `--case-sensitive`. Only ASCII letters are folded, without using the system locale (no `tolower`) |
| Case-insensitive non-ASCII | Not supported in v1 |
| What `--max-results` counts | Matching lines. A file counts as one when there's nothing to highlight, or with `-l`. A file can be cut partway. 0, the default, means no limit |
| Result order | Repo name, then path (by its bytes), then position in the file. Same every run |
| Positions | Byte offsets inside the engine. Line and column (in bytes, from 1) are worked out for display |
| Line endings | A line ends at LF. A CR before it stays in the content and is hidden on display. A match that runs past the end of its line is cut there. One that starts in a line ending, like `"\nfoo"`, is shown from its first visible byte, on the next line; one made only of line endings is an empty highlight at the end of its line |
| Incomplete results | Results say if they're complete, cut off by a limit, or cancelled (Ctrl-C) |
| Errors | Empty queries, unbalanced parentheses or quotes, a filter with no value, and nesting deeper than 100 levels are errors, with a message saying what's wrong |

Checking a file has two steps. `matches` decides yes or no using the
whole query. `collectSpans` then finds what to highlight, only for files that
matched. For `foo -bar`, a file isn't shown until we've checked it has no `bar`.

### Output

Text output looks like grep's:

```
repo:path:line:column:text     a line with a match (column of its first match)
repo:path-line-text            a context line, with -C n
--                             between groups that aren't next to each other, with -C
repo:path                      with -l, or when there's nothing to highlight
```

With `--path` there's no `repo:` in front. Colors follow grep: on for a
terminal, off for a pipe, `NO_COLOR` or `TERM=dumb`; `--color=always` or
`--color=never` decides.

On a terminal, or with colors, bytes that could control the terminal are
shown as `\xHH`: control bytes other than tab, DEL, the C1 controls U+0080 to
U+009F, and bytes that aren't valid UTF-8. So a file can't change the title
or clear the screen through an escape sequence. Into a pipe or a file
without colors, the bytes are written as they are.

Repo locations and error messages are escaped the same way when they go to
a terminal. JSON is always written with DEL and the C1 controls as `\u`
escapes, which JSON reads as the same text. A highlight that starts or ends
inside a character covers the whole character.

If the output closes early, like `| head`, the search stops at the next
file and exits with 2.

`--json` prints one document. Files are written as they're found, so a big
result never has to sit in memory:

```json
{"files":[
{"repo":"demo","path":"a.go","lines":[
  {"line":1,"text":"one","matches":[]},
  {"line":2,"text":"two needle","matches":[{"start":4,"end":10}]}]}
],"status":"complete"}
```

- `status` is `complete`, `truncated` (a limit cut it), `cancelled`, or
  `error`. Errors before the search starts, like a bad query, give a
  document with no files and `status` `error`. With `error`, an `error`
  field holds the message, and the files before it are still there. The
  program also prints the message to stderr and exits with 2. (`error` was
  added after M6.)
- A repo whose index turns out to be damaged, when it's opened or partway
  through the search, is skipped: its results stop there and the other
  repos are still searched. Each one goes in `skipped`, after `files`, as
  `{"repo":"demo","error":"demo: ... Rebuild it with: ..."}`, and the same
  message goes to stderr. The exit code still says whether anything was
  found. If the damaged repo is the last one left to search, it's an error
  instead, as above.
- Warnings about the query, like a regex that doesn't start its word, go in
  `warnings`, after `skipped`, as a list of messages. The same messages go to
  stderr. The search still runs as typed. (Added after M9.)
- `lines` holds matching lines and context lines; context lines have no
  matches. It's empty with `-l` and for files with nothing to highlight.
- `start` and `end` are byte offsets inside the line's original bytes. A
  line's matches are in order of `start`, then `end`, each only once.
- JSON text must be valid UTF-8. If a path or line isn't, its bad bytes become
  U+FFFD, and `path_base64` or `text_base64` holds the exact bytes.

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
      ▼  for each repo, on the calling thread
 ┌──────────────────────────────────────────────────────────────┐
 │ candidates = run(TrigramQuery), handed out in chunks         │
 └───────────────────────────────┬──────────────────────────────┘
                                 ▼  each chunk, on the thread pool
 ┌──────────────────────────────────────────────────────────────┐
 │ for each candidate:                                          │
 │   Evaluator.matches(content)       uses the whole query      │
 │   Evaluator.collectSpans(content)                            │
 │   the result lines                                           │
 └───────────────────────────────┬──────────────────────────────┘
                                 ▼  chunks taken in order, on the calling thread
                     results, in order, within limits
```

Searches use one pool of threads (one per core, at most 8). The calling
thread finds every repo's candidates first, which takes well under a
millisecond. The pool's threads then check the files in chunks, and the
calling thread passes the chunks on in order. So results come out in the same
order as with one thread, and the limit cuts them at the same place.

Memory stays bounded however slowly the results are read:

- At most 4 chunks per thread are in flight.
- Results not taken yet are kept to about 32 MiB, counted as they're made.
  Once that's reached, no more chunks are handed out, and a task stops at
  the next file; the rest of its chunk is checked next. The chunk the caller
  waits for always checks at least one file, so the search keeps moving.
- Chunks are also sized and handed out by the result size per file seen so
  far (16 files, down to 1), so the threads aren't all partway through big
  files when the limit is reached. The first chunk on each thread has 1
  file, to find out.
- With a limit, a file's result never has more matching lines than the
  limit. Context lines come on top.

Damage found in a repo, and cancelling, show up when the results get there,
as with one thread. A skipped repo's chunks still running are dropped as
they come up, and no more of it are handed out. On a machine with one core, or if the threads can't be
started, the search runs on the calling thread.

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
| `A or B` | `Or[A, B]`, which merges the posting lists. If either side is `All`, the result is `All` |
| `not A` | `All`. The final check uses the index instead (see below) |
| Filters like `lang:` or `path:` | `All` here. The final check applies them |
| Regex | RE2's prefilter tree of required text (see Regex below), or `All` |

Never take the opposite of a candidate list. Its extra files would turn into
missed matches.

Before planning, each repo gets its own copy of the query with `repo:` turned
into true or false and simplified (`resolveRepo`). In an `and`, false makes
it all false. In an `or`, true makes it match every file, but it stays next
to the other branches instead of replacing the `or`, because they still say
what to highlight.

The final check also uses the index, for text the candidates don't cover:
text under a `-`, and text in an `or` branch. For each such literal of 3+
bytes, it looks up which files have all of its trigrams. A file without them
can't contain the text, so it isn't searched for there. `lang:go -TODO` then
reads only the Go files that might contain `TODO`.

How each part runs:

- `And`: start from the shortest posting list and keep only its files that are
  in the next list, then the next. Stop as soon as none are left. Also stop
  before a list with more than 256 entries per file still left: checking those
  few files costs less than walking the list (measured in M7).
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
 4. INTERSECT keep the files in every list, stop early if none are left
              → candidates {3, 17, 42}
 5. CHECK     withContent(3, …) → matches → collectSpans
 6. OUTPUT    src/auth/login.ts:12:14:  export async function authenticate(user) {
```

### Regex

We use RE2, never `std::regex`. RE2 never backtracks, so a search takes at
most time linear in the text times the pattern's size, and for most patterns
linear in the text alone. RE2's fast matcher, a DFA, gets up to 64 MiB per
regex (the default is 8 MiB): on 1 MiB, `/(a|b).{1000}(a|b){1000}c/` takes
0.06 s instead of 17 s. A few patterns, like `/a.{1000}c/`, need more DFA
states than any limit allows, and RE2's fallback takes seconds on a long
file. Ctrl-C is checked between files, so it waits for that file.
`RegexMatcher` compiles each pattern once, as `(?m)pattern` so
`^` and `$` work per line, with RE2's `never_nl` so no match takes in a line
ending. There's no group around the pattern: a `\Q` without `\E` would
quote its `)`.

`never_nl` doesn't stop `\C`, which matches any one byte, from matching a
`\n`. So a regex with `\C` (found by walking RE2's parse tree) is searched
one line at a time: each search ends at its line's end, while RE2 still sees
the text around the line for `^`, `$` and `\b`. A `\C` match can also
start inside a UTF-8 character, so after an empty match the search moves on
by one byte instead of one character.

Each search for a match is linear, but finding every match on a line can
take time growing with the square of the line's length: for `/a*z|a/` on a
line of `a`s, each search looks to the line's end for a `z`. RE2 can't say
how much work a search did, so `findAll` has a time limit: 100 ms plus 1
microsecond per byte of the file. After it, `findAll` looks for only the
first match in each line that's left, which is linear again. Every matching
line is still in the results; only its later matches aren't highlighted. (A
line where the regex first matches empty, then matches text further on, can
be missed after the limit, since finding the later match is what's slow.)
The limit counts the thread's CPU time, so a paused process or a busy
machine doesn't use it up. The clock is read before every search, since
one search can take a second; it runs past the limit by one search at most.
The wall clock is read first, since it's cheaper: CPU time can't pass the
limit before it does.
A release build finds even a match on every byte in 0.04 microseconds per
byte, so only pathological cases reach it. (An
earlier limit counted work per line, which cut ordinary long lines, like
minified code, to about 300 highlights.) With `-l` the matches aren't looked
for at all.

1. v1 (done): a regex is `All`, and RE2 checks every file the rest of the query
   allows. The correct baseline.
2. v2 (done): RE2's prefilter, the AND/OR tree of text a match needs that
   `FilteredRE2` uses, becomes the regex's candidates. `/foo|bar/` becomes
   `foo OR bar`, never `foo AND bar`. Atoms shorter than 3 bytes become `All`.
3. v3, only if measurements say it's worth it: our own regex-to-trigram
   analysis. They don't yet (see [BENCHMARKS.md](BENCHMARKS.md)).

The prefilter lowercases with Unicode rules, and the index folds only ASCII.
Where those differ, the index could miss a match, so:

- A pattern that can name a non-ASCII character (with its bytes, `\x{...}`,
  an octal escape like `\577`, `\p` or `\P`) is `All`. Some lowercase into ASCII: U+212A KELVIN SIGN
  becomes `k`.
- Without `case:yes`, or with a `(?i)` flag in the pattern, RE2 also matches
  KELVIN SIGN for `k` and LONG S for `s`. Those are the only ASCII letters it
  folds with anything outside ASCII (checked against every code point). So
  an atom is split at each `k` and `s`, and only the pieces between are
  required: `mustcompile(` needs `mu` and `tcompile(`.
- An atom with non-ASCII bytes is `All`. After the first rule, these only
  come from case variants like LONG S for `s`, which the index keeps as they
  are; it's a second guard.

Regexes under a `-` or in an `or` branch are checked in full, without the
index shortcut literals get there.

### Symbol search (later)

`sym:authenticate` needs a symbol table, built at index time with universal-ctags
or tree-sitter. It would be one more kind of query part.

## HTTP API

`code-search serve` runs the server on 127.0.0.1:7700 (`--port` to change it)
until Ctrl-C. It serves the API under `/api/v1/` and the built web UI at `/`.
The `1` changes only for a change that breaks clients; new fields and routes
don't count.

| Route | Answer |
|-------|--------|
| `GET /api/v1/version` | `{"version":"0.1.0","api":1}` |
| `GET /api/v1/search?q=...` | The `search --json` document |
| `GET /api/v1/repos` | `{"repos":[{"name","source","location","commit"}]}` |
| `POST /api/v1/repos` `{"source","name"}` | Adds a repo: 201 with `{"repo","report"}` |
| `POST /api/v1/repos/<name>/update` `{"rebuild"}` | `{"repo","rebuilt","reason","report"}`; `report` only if rebuilt |
| `DELETE /api/v1/repos/<name>` | `{"repo"}` |

Search takes `q`, and optionally `repo`, `case_sensitive` and `files_only`
(`true` or `false`), `max_results` and `context`, with the same meaning as
the CLI's options. An unknown or repeated parameter is an error. The answer is
the same document the CLI prints, made by the same code (`SearchService`
and `JsonResultWriter`), so a finished search gives the same bytes either way.
It's sent in pieces as files are found, so a big result never sits in memory.

Errors:

- Search: errors found before it starts get a status and the CLI's error
  document, `{"files":[],"status":"error","error":"..."}`. 400 for a bad query
  or parameter, 404 for an unknown repo or none added, 500 for damaged
  indexes. Once results are being sent the status is 200, and an error ends the
  document with `"status":"error"`, as in the CLI.
- Everything else: `{"error":{"code":"...","message":"..."}}`. 400
  `bad_request`, 403 `forbidden`, 404 `not_found` or `unknown_repo`, 413 for a
  body over 64 KiB, 415 `unsupported_media_type`, 422 `failed` when a change
  couldn't be made (the message says why: not a Git repo, name taken, Git
  failed), 500 `internal`.

Stopping a search:

- A search stops at the next file once its client closes the connection. The
  web UI does that when a new search starts. A thread checks every 50 ms,
  since a search that finds little would otherwise not notice.
- After 30 seconds it stops, and the document ends with `"status":"error"`
  and a message saying so. The files before it are still there.
- A client that stops reading for 10 seconds counts as gone.

Limits for a long-running process: 8 requests are answered at once and up to
64 more wait; more are turned away. Searches share one pool of threads for
checking files, as in the CLI. `IndexCache` keeps up to 64 index files open
between searches, closing the one used longest ago, and closes files the
registry no longer names. A search holds its files until it's done, so an
update during a search doesn't change its results: the old file stays
readable after it's deleted.

The server and the CLI change repos through the same `RepoManager` and its
lock, and both read the registry without it, so there's no need for SQLite.

Requests a web page on another site could send are turned away:

- The `Host` header must be `127.0.0.1:<port>` or `localhost:<port>`. This
  stops DNS rebinding, where a site points its own name at 127.0.0.1 to read
  the answers.
- An `Origin` header, if there is one, must be this server's.
- A `POST` must have `Content-Type: application/json`. An HTML form can't send
  that, and a script on another site would have to ask first (a CORS
  preflight), which the server never allows.
- Every answer has `X-Content-Type-Options: nosniff`, so JSON can't be loaded
  as a script, and a `Content-Security-Policy` that lets the UI load only its
  own files and stops other sites from framing it.

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
- The HTTP server listens on 127.0.0.1 only and turns away requests a page on
  another site could send (see "HTTP API"). The web UI renders file content as
  text, never as HTML. Syntax colors come from highlight.js through lowlight,
  which gives a tree of tokens, not HTML, so they're text too. A block of
  lines over 50,000 characters is shown plain, so a huge minified line can't
  freeze the page.

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
| Resolve `repo:` per repo | A repo whose query becomes false is skipped, so a filter that matches no repo can never turn into "all repos" |
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
| Old index files piling up in the server | Done in M9: `IndexCache` keeps at most 64 open, and closes files the registry no longer names |

## Source tree

```
code-search/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/                      warnings, tools, pinned dependencies
├── include/codesearch/
│   ├── git/        git_runner.hpp  git_repo.hpp
│   ├── repo/       workspace.hpp  registry.hpp  publisher.hpp  github_url.hpp
│   ├── index/      trigram.hpp  file_filter.hpp  memory_index.hpp  index_reader.hpp
│   │               shard_format.hpp  shard_writer.hpp  shard_reader.hpp
│   │               posting_cursor.hpp  index_file_error.hpp
│   ├── query/      ast.hpp  parser.hpp  language.hpp  planner.hpp  trigram_query.hpp
│   ├── search/     literal_matcher.hpp  regex_matcher.hpp  candidates.hpp  evaluator.hpp  searcher.hpp
│   │               repo_search.hpp  json_output.hpp
│   ├── app/        index_commit.hpp  repo_manager.hpp  search_service.hpp  index_cache.hpp
│   └── util/       process.hpp  utf8.hpp  mmap_file.hpp  atomic_file.hpp  varint.hpp  endian.hpp
│                   object_id.hpp  thread_pool.hpp  file_lock.hpp  ascii.hpp
├── src/                        same layout as include/
├── cli/main.cpp                builds the `code-search` program
├── server/                     http_server.hpp/.cpp: the HTTP API, run by `code-search serve`
├── web/                        the React and TypeScript UI (Vite); `npm run build` makes web/dist
├── tests/
│   ├── unit/                   one file per module
│   ├── cli/                    runs the real `code-search` program
│   ├── differential/           index search vs brute force
│   ├── integration/            Git to search, start to finish
│   ├── server/                 the HTTP server, against the real CLI
│   ├── fuzz/                   index reader (the parser's random tests are in unit/)
│   └── support/                brute-force search, temp Git repos, a fake `git` for broken output
├── benchmarks/                 search_bench, run_benchmarks.py, repos.tsv, queries.tsv,
│                               index_bench, storage_experiment (M2.5)
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
code-search search --path ~/projects/other "authenticate"   # not added; indexed in memory
code-search search 'auth lang:go -path:_test'
code-search search '(repo:backend login) or (repo:frontend signIn)'
code-search search -C 2 --max-results 20 'TODO'
code-search search --json 'authenticate'
code-search search '/auth(enticate|orize)/'      # regex
code-search update my-backend
code-search update my-backend --rebuild
code-search remove my-backend                    # never deletes your code
code-search verify my-backend                    # full check of the index file
code-search serve                                # HTTP API and web UI on 127.0.0.1:7700
code-search serve --port 8080 --ui web/dist
```

## Testing

The main check is a brute-force scan of the same files. It must share no code
with the planner; otherwise a planner bug would hide itself.

| Tests | What they check |
|-------|-----------------|
| Unit | Each module |
| Candidates | Every real match survives the trigram step |
| Literal search vs brute force | Same files and matches. Includes empty, short, common and missing queries, non-ASCII bytes, CRLF, no final newline, long lines and duplicate files |
| Boolean queries | Random query trees over several repos, on one thread and on a thread pool, with and without a limit. `repo:A foo or repo:B bar`, path filters inside OR, queries with only `-` terms or only filters, unknown repo, precedence and quotes |
| Regex vs brute force | Random regexes (alternation, empty and optional branches, repeats, classes, anchors, `\b`, empty matches) inside the random query trees, against a reference that runs RE2 anchored at each character. The text includes KELVIN SIGN, LONG S and `é`/`É`, which RE2 folds differently from the index. Targeted tests for each case where the index step could miss a match |
| Git | Pinned commit while HEAD moves, uncommitted changes, file names with newlines, tabs or a leading dash, symlinks, submodules, LFS pointers, huge files, SHA-256 rejection. The user's repo is never changed |
| `git grep` | `git grep <commit-id>` on the same files, using syntax both support |
| Publishing | Kill the process after every write, fsync and rename. The current state stays complete, cleanup removes leftovers, a second change waits for the lock, and searches keep working |
| Fuzzing | Opening and reading damaged index files; the query parser with deep nesting and long input. We keep the inputs that found bugs |
| Sanitizers | ASan and UBSan, and TSan for the threaded search and build (`asan` and `tsan` presets) |
| HTTP server | The same document as `search --json`, byte for byte, for finished searches and for errors before the search starts. Cancelling when the client goes away, the time limit, an update deleting the index file a search has open, searches at the same time, and requests from other sites turned away |
| Web UI | `npm test` in `web/`: byte offsets to highlights (non-ASCII, cut characters, invalid UTF-8), syntax colors (by file name, across lines, cut at match edges), and that file content, paths and messages come out escaped |
