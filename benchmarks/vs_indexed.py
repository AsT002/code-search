#!/usr/bin/env python3
"""code-search against other indexed code search tools. See docs/BENCHMARKS.md.

    vs_indexed.py <work dir> --code-search <binary> --go-bin <dir> [--runs 7]
                  [--corpus linux|three|both]

The tools:

- csearch: Russ Cox's codesearch (cindex builds the index, csearch searches),
  the trigram index this project's design comes from. It keeps only posting
  lists, and reads candidate files from disk.
- zoekt: Sourcegraph's code search. Its index keeps file contents and
  trigram positions. Searched with zoekt-all (benchmarks/zoekt_all), its own
  CLI with the match limits raised, and with zoekt-webserver.

--go-bin is a folder with cindex, csearch, zoekt-index, zoekt-webserver and
zoekt-all, built with Go:

    GOBIN=<dir> go install github.com/google/codesearch/cmd/...@v1.2.0
    GOBIN=<dir> go install github.com/sourcegraph/zoekt/cmd/zoekt-index@<version>
    GOBIN=<dir> go install github.com/sourcegraph/zoekt/cmd/zoekt-webserver@<version>
    (cd benchmarks/zoekt_all && go build -o <dir>/zoekt-all .)

Uses the checkouts run_benchmarks.py and vs_ripgrep.py make in <work dir>:
linux (the kernel fork ripgrep's benchsuite uses) and googletest, brew and
go ("three"). Prints Markdown: what each index costs, whether the tools find
the same lines, fresh-process speed, warm servers, and peak memory.
"""

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from run_benchmarks import case_clashes, clone, ms, plain_text, read_tsv, run_stream, table  # noqa: E402
from vs_ripgrep import BENCHSUITE, LINUX, collect, machine_info, p50_p90, timed_peak, timings  # noqa: E402

# code-search leaves files over 1 MB out of its index; zoekt is told the same.
FILE_LIMIT = 1024 * 1024
ZOEKT_PORT = 6072
CS_PORT = 7812


def folder_bytes(path):
    return sum(f.stat().st_size for f in Path(path).rglob("*") if f.is_file())


def tracked_bytes(checkout):
    names = subprocess.run(["git", "-C", str(checkout), "ls-files", "-z"], capture_output=True,
                           check=True).stdout.split(b"\0")
    total = 0
    for name in names:
        path = checkout / os.fsdecode(name)
        if name and path.is_file() and not path.is_symlink():
            total += path.stat().st_size
    return len([n for n in names if n]), total


def escape_text(text):
    """`text` as a regex that matches it exactly, in RE2 syntax."""
    return re.sub(r"([\\.+*?()|\[\]{}^$])", r"\\\1", text)


def zoekt_regex(pattern):
    """A regex as one zoekt query term: zoekt splits its query at every
    space, even inside [ ], and reads '"' as the start of a quoted string,
    so both are written as hex escapes, which Go's regexp reads."""
    return "regex:" + pattern.replace(" ", r"\x20").replace('"', r"\x22")


def translate(query):
    """For a code-search query csearch and zoekt can run the same way:
    (csearch arguments, zoekt query), or None."""
    plain = plain_text(query)
    if plain is not None:
        text, case_sensitive = plain
        pattern = escape_text(text)
    elif re.fullmatch(r"(case:yes )?/[^\s].*/", query) and "/ " not in query:
        case_sensitive = query.startswith("case:yes ")
        pattern = query.removeprefix("case:yes ")[1:-1].replace("\\/", "/")
    else:
        return None
    csearch = ([] if case_sensitive else ["-i"]) + ["--", pattern]
    zoekt = ("case:yes " if case_sensitive else "case:no ") + zoekt_regex(pattern)
    return csearch, zoekt


def queries_for(corpus):
    """(name, code-search query) pairs that all three tools can run."""
    if corpus == "linux":
        # Two of the suite's benchmarks differ only in rg's options, so
        # they're the same search here: each query once.
        seen, out = set(), []
        for name, _, query in BENCHSUITE:
            if query not in seen:
                seen.add(query)
                out.append((name, query))
        return out
    # A '|' in a name is escaped so it stays in its Markdown table cell.
    pairs = [(f"{group}: {query}".replace("|", "\\|"), query) for group, query in read_tsv(HERE / "queries.tsv")]
    return [(name, query) for name, query in pairs if translate(query) is not None]


# ---------------------------------------------------------------------------
# The three tools, as commands


class Tools:
    def __init__(self, cs, go_bin, work):
        self.cs = cs
        self.go = Path(go_bin)
        self.work = work  # where this corpus's indexes go

    @property
    def cs_env(self):
        return dict(os.environ, CODE_SEARCH_HOME=str(self.work / "code-search"))

    @property
    def csearch_env(self):
        return dict(os.environ, CSEARCHINDEX=str(self.work / "csearchindex"))

    @property
    def zoekt_dir(self):
        return self.work / "zoekt"

    def cs_search(self, query, files_only=False):
        return [self.cs, "search", "--color=never", "--max-results", "0"] + \
            (["-l"] if files_only else []) + ["--", query]

    def csearch(self, args, files_only=False):
        return [str(self.go / "csearch")] + (["-l"] if files_only else ["-n"]) + args

    def zoekt(self, query, files_only=False):
        return [str(self.go / "zoekt-all"), "-index_dir", str(self.zoekt_dir)] + \
            (["-l"] if files_only else []) + [query]


def build_indexes(tools, checkouts):
    """Builds each tool's index from scratch. Returns table rows."""
    if tools.work.exists():
        shutil.rmtree(tools.work)
    tools.zoekt_dir.mkdir(parents=True)

    def add_all(make_cmd, env):
        seconds, peak = 0.0, 0.0
        for _, path in checkouts:
            s, p = timed_peak(make_cmd(path), env)
            seconds += s
            peak = max(peak, p)
        return seconds, peak

    cs_time, cs_peak = add_all(lambda path: [tools.cs, "add", str(path)], tools.cs_env)
    paths = [str(path) for _, path in checkouts]
    c_time, c_peak = timed_peak([str(tools.go / "cindex")] + paths, tools.csearch_env)
    z_time, z_peak = add_all(lambda path: [str(tools.go / "zoekt-index"), "-disable_ctags",
                                           "-file_limit", str(FILE_LIMIT), "-parallelism", "8",
                                           "-index", str(tools.zoekt_dir), str(path)], None)
    sizes = {
        "code-search": folder_bytes(tools.work / "code-search" / "indexes"),
        "csearch": (tools.work / "csearchindex").stat().st_size,
        "zoekt": folder_bytes(tools.zoekt_dir),
    }
    return [
        ["code-search", f"{cs_time:.1f} s", f"{cs_peak:.0f} MiB", f"{sizes['code-search'] / 2**20:,.0f} MiB",
         "posting lists and file contents; reads the commit from Git"],
        ["csearch (Cox)", f"{c_time:.1f} s", f"{c_peak:.0f} MiB", f"{sizes['csearch'] / 2**20:,.0f} MiB",
         "posting lists only; files are read from disk when searching"],
        ["zoekt", f"{z_time:.1f} s", f"{z_peak:.0f} MiB", f"{sizes['zoekt'] / 2**20:,.0f} MiB",
         "file contents and trigram positions; 8 shards built at once"],
    ]


# ---------------------------------------------------------------------------
# Same results


CS_LINE = re.compile(rb"^([^:]+):(.*?):(\d+):\d+:")
GREP_LINE = re.compile(rb"^(.*?):(\d+):")


def as_repo_path(path, checkouts):
    """csearch prints the paths it indexed (absolute); zoekt and code-search
    print repo/path. Makes them all repo/path."""
    for name, root in checkouts:
        prefix = str(root).encode() + b"/"
        if path.startswith(prefix):
            return name.encode() + b"/" + path[len(prefix):]
    return path


def matches_of(out, kind, checkouts):
    found = set()
    for line in out.splitlines():
        m = (CS_LINE if kind == "cs" else GREP_LINE).match(line)
        if not m:
            continue
        if kind == "cs":
            found.add((m.group(1) + b"/" + m.group(2), int(m.group(3))))
        else:
            found.add((as_repo_path(m.group(1), checkouts), int(m.group(2))))
    return found


def same_results(tools, queries, checkouts):
    rows = []
    for name, query in queries:
        c_args, z_query = translate(query)
        ours = matches_of(collect(tools.cs_search(query), tools.cs_env), "cs", checkouts)
        cox = matches_of(collect(tools.csearch(c_args), tools.csearch_env), "grep", checkouts)
        zo = matches_of(collect(tools.zoekt(z_query)), "grep", checkouts)
        rows.append([name, f"{len(ours):,}", f"{len(cox):,}", f"{len(ours - cox):,} / {len(cox - ours):,}",
                     f"{len(zo):,}", f"{len(ours - zo):,} / {len(zo - ours):,}"])
    return rows


# ---------------------------------------------------------------------------
# Speed


def faster(cs_seconds, other_seconds, other):
    """Which tool was faster, and by how much: "cs 12x", "zoekt 4.2x" or
    "same" within 10%."""
    if cs_seconds <= other_seconds:
        ratio, name = other_seconds / cs_seconds, "cs"
    else:
        ratio, name = cs_seconds / other_seconds, other
    if ratio < 1.1:
        return "same"
    return f"{name} {ratio:.0f}x" if ratio >= 10 else f"{name} {ratio:.1f}x"


def fresh_speed(tools, queries, runs):
    rows = {False: [], True: []}
    for files_only in (False, True):
        for name, query in queries:
            c_args, z_query = translate(query)
            got = timings([
                ("cs", lambda q=query: run_stream(tools.cs_search(q, files_only), tools.cs_env)[1]),
                ("csearch", lambda a=c_args: run_stream(tools.csearch(a, files_only), tools.csearch_env)[1]),
                ("zoekt", lambda q=z_query: run_stream(tools.zoekt(q, files_only))[1]),
            ], runs)
            cs_t = statistics.median(got["cs"])
            rows[files_only].append([
                name, p50_p90(got["cs"]),
                p50_p90(got["csearch"]), faster(cs_t, statistics.median(got["csearch"]), "csearch"),
                p50_p90(got["zoekt"]), faster(cs_t, statistics.median(got["zoekt"]), "zoekt"),
            ])
    return rows


def drain(answer):
    """Reads an HTTP answer to the end in 1 MiB pieces, keeping none of it.
    read() would join it all into one string, which for a big answer is
    slower than the server: zoekt-webserver gives a request 20 s."""
    while answer.read(1 << 20):
        pass


def wait_until_up(url, seconds=120):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=1):
                return
        except OSError:
            time.sleep(0.2)
    raise RuntimeError(f"{url} didn't come up")


def warm_speed(tools, queries, runs):
    """Both servers already running, asked over HTTP: a page of 200 lines,
    as a UI asks for, and every result. zoekt's page uses its own defaults
    (it stops at 100,000 matches per shard, then ranks) and shows 200
    lines; for every result its limits are raised as in zoekt-all."""
    cs_server = subprocess.Popen([tools.cs, "serve", "--port", str(CS_PORT)], env=tools.cs_env,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    z_server = subprocess.Popen([str(tools.go / "zoekt-webserver"), "-index", str(tools.zoekt_dir),
                                 "-listen", f"127.0.0.1:{ZOEKT_PORT}", "-html=false", "-rpc"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rows = []
    try:
        wait_until_up(f"http://127.0.0.1:{CS_PORT}/api/v1/version")
        wait_until_up(f"http://127.0.0.1:{ZOEKT_PORT}/readyz")

        def cs_get(query, limit):
            url = f"http://127.0.0.1:{CS_PORT}/api/v1/search?" + urllib.parse.urlencode(
                {"q": query, "max_results": str(limit)})
            start = time.perf_counter()
            with urllib.request.urlopen(url) as answer:
                drain(answer)
            return time.perf_counter() - start

        def zoekt_post(query, opts):
            body = json.dumps({"Q": query, "Opts": opts}).encode()
            request = urllib.request.Request(f"http://127.0.0.1:{ZOEKT_PORT}/api/search", data=body,
                                             method="POST")
            start = time.perf_counter()
            with urllib.request.urlopen(request) as answer:
                drain(answer)
            return time.perf_counter() - start

        many = 1 << 30
        for name, query in queries:
            _, z_query = translate(query)
            got = timings([
                ("cs page", lambda q=query: cs_get(q, 200)),
                ("zoekt page", lambda q=z_query: zoekt_post(q, {"MaxMatchDisplayCount": 200})),
                ("cs all", lambda q=query: cs_get(q, 0)),
                ("zoekt all", lambda q=z_query: zoekt_post(q, {"ShardMaxMatchCount": many,
                                                               "TotalMaxMatchCount": many})),
            ], runs)
            page, z_page = statistics.median(got["cs page"]), statistics.median(got["zoekt page"])
            every, z_every = statistics.median(got["cs all"]), statistics.median(got["zoekt all"])
            rows.append([name, ms(page), ms(z_page), faster(page, z_page, "zoekt"),
                         ms(every), ms(z_every), faster(every, z_every, "zoekt")])
    finally:
        for server in (cs_server, z_server):
            server.terminate()
            server.wait()
    return rows


def memory(tools, queries):
    rows = []
    for name, query in queries:
        c_args, z_query = translate(query)
        rows.append([name] + [f"{timed_peak(cmd, env)[1]:.0f} MiB" for cmd, env in (
            (tools.cs_search(query), tools.cs_env),
            (tools.csearch(c_args), tools.csearch_env),
            (tools.zoekt(z_query), None),
        )])
    return rows


# ---------------------------------------------------------------------------


def run_corpus(corpus, checkouts, tools, runs):
    files, data = 0, 0
    for _, path in checkouts:
        f, d = tracked_bytes(path)
        files += f
        data += d
    names = ", ".join(name for name, _ in checkouts)
    queries = queries_for(corpus)
    print(f"## {names}: {files:,} tracked files, {data / 2**20:,.0f} MiB\n")
    clashes = sum(len(case_clashes(path)) for _, path in checkouts)
    if clashes:
        print(f"{clashes} pairs of tracked files have names that differ only in case. On this "
              "disk, which ignores case, one of each pair is on disk, and that's what csearch and "
              "zoekt index; code-search reads both from Git.\n")

    print("### What each index costs\n")
    print(f"Built from scratch. Each tool leaves out files over {FILE_LIMIT // 2**20} MB except "
          "csearch, which has no size limit but skips files with invalid UTF-8, a line over "
          "2,000 bytes or too many trigrams.\n")
    table(["Tool", "Build", "Peak memory", "Index size", "What the index holds"], build_indexes(tools, checkouts))

    print("### Same results?\n")
    print("Matching lines as (file, line), and how many only code-search / only the other tool "
          "found. Differences come from which files each tool indexes, and from zoekt matching a regex "
          "across the whole file: its \\s can match a newline, so a match can span lines. "
          "code-search and csearch match within a line, like grep.\n")
    table(["Query", "code-search", "csearch", "only cs / only csearch", "zoekt", "only cs / only zoekt"],
          same_results(tools, queries, checkouts))

    speed = fresh_speed(tools, queries, runs)
    for files_only, title in ((False, "every matching line"), (True, "matching files only (-l)")):
        print(f"### Fresh process, {title} (ms, p50 / p90 of {runs} runs)\n")
        print("Each search is a new process printing to a pipe. \"Faster\" names the faster tool by "
              "median, and by how much.\n")
        table(["Query", "code-search", "csearch", "Faster", "zoekt", "Faster"], speed[files_only])

    print(f"### Warm servers (ms, median of {runs} runs)\n")
    print("code-search serve and zoekt-webserver, already running, over HTTP. A page is 200 "
          "matching lines; zoekt finds up to its own limits, ranks, then shows 200. \"All\" is every "
          "matching line. csearch has no server.\n")
    table(["Query", "Page: code-search", "zoekt", "Faster", "All: code-search", "zoekt", "Faster"],
          warm_speed(tools, queries, runs))

    print("### Peak memory while searching\n")
    table(["Query", "code-search", "csearch", "zoekt"], memory(tools, queries[:4] + queries[-2:]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("work_dir", type=Path)
    parser.add_argument("--code-search", required=True)
    parser.add_argument("--go-bin", required=True, type=Path)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--corpus", choices=["linux", "three", "both"], default="both")
    args = parser.parse_args()

    for tool in ("cindex", "csearch", "zoekt-index", "zoekt-webserver", "zoekt-all"):
        if not (args.go_bin / tool).exists():
            sys.exit(f"{args.go_bin / tool} is missing; see the top of this file")
    repo_dir = args.work_dir / "repos"
    cs = str(Path(args.code_search).resolve())
    machine_info(cs)
    print(f"- Other tools: {', '.join(version_of(args.go_bin / t) for t in ('csearch', 'zoekt-all'))}\n")

    if args.corpus in ("linux", "both"):
        name, url, commit = LINUX
        clone(url, commit, repo_dir / name)
        run_corpus("linux", [(name, (repo_dir / name).resolve())],
                   Tools(cs, args.go_bin, args.work_dir / "indexed" / "linux"), args.runs)
    if args.corpus in ("three", "both"):
        repos = read_tsv(HERE / "repos.tsv")
        for name, url, commit in repos:
            clone(url, commit, repo_dir / name)
        run_corpus("three", [(name, (repo_dir / name).resolve()) for name, _, _ in repos],
                   Tools(cs, args.go_bin, args.work_dir / "indexed" / "three"), args.runs)


def version_of(binary):
    """The Go module a binary was built from, like "codesearch v1.2.0"."""
    info = subprocess.run(["go", "version", "-m", str(binary)], capture_output=True, text=True).stdout
    for line in info.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] in ("mod", "dep") and parts[1].endswith(("/codesearch", "/zoekt")):
            return f"{parts[1].rsplit('/', 1)[-1]} {parts[2]}"
    return binary.name


if __name__ == "__main__":
    main()
