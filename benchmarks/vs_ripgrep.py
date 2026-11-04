#!/usr/bin/env python3
"""A fuller comparison of code-search and ripgrep. See docs/BENCHMARKS.md.

    vs_ripgrep.py <work dir> --code-search <binary> [--runs 7]

Uses the checkouts and workspace that run_benchmarks.py makes in <work dir>
(it builds the workspace if it's missing). Prints Markdown:

1. Same results: for each query both can run, the (file, line) matches
   each tool finds, with rg as it comes and with rg set to search the files
   code-search indexes.
2. Speed: all results, files only (-l), and the first 10 lines, each a new
   process, the tools taking turns. rg as it comes, and matched with -j 8.
3. Repo size: each repo on its own.
4. A running server (code-search serve) against rg.
5. Peak memory.
6. What the index costs: build time, size, and how many searches pay for
   building it.
7. ripgrep's own benchsuite (benchsuite/benchsuite in its repo): its 11
   linux_* benchmarks, with the same patterns and rg commands, on the same
   Linux kernel fork, against code-search.

Needs git, rg and /usr/bin/time. Release builds only.
"""

import argparse
import os
import platform
import re
import statistics
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))  # for run_benchmarks, next to this file

from run_benchmarks import clone, ms, percentile, plain_text, read_tsv, run_stream, table  # noqa: E402

# Regexes with no text every match must contain, so the index can't skip
# files for them: the worst case for code-search. Added to queries.tsv's.
EXTRA_QUERIES = [
    ("regex, no required text", r"/[A-Z]{3,}_[A-Z]{3,}/"),
    ("regex, no required text", r"/\b[0-9]{4,}\b/"),
    ("regex, no required text", r"/\w+\(\w+, \w+\)/"),
]

# What code-search leaves out of its index (see FileFilter), as rg flags, so
# both search the same files: tracked files, hidden ones too, at most 1 MB,
# without minified or map files. --no-ignore: code-search indexes what Git
# tracks, even if a .gitignore names it.
MATCHED_FILES = [
    "--hidden", "--no-ignore", "--glob", "!.git", "--max-filesize", "1M",
    "--glob", "!*.min.js", "--glob", "!*.min.css", "--glob", "!*.js.map", "--glob", "!*.css.map",
]

# A code-search result line: repo:path:line:column:text, or repo:path-line-
# for context. Paths can hold ':', so the line and column are found from
# the left after the repo, without being greedy about the path.
CS_LINE = re.compile(rb"^([^:]+):(.*?):(\d+):\d+:")
RG_LINE = re.compile(rb"^(.*?):(\d+):")


def rg_query(query):
    """(rg arguments, label) for a query rg can run the same way, or None."""
    plain = plain_text(query)
    if plain is not None:
        text, case_sensitive = plain
        return ["-F", "-s" if case_sensitive else "-i", "--", text], "text"
    if re.fullmatch(r"/[^\s].*/", query) and "/ " not in query:
        # rg's regex syntax is close to RE2's for these, and its ^ and $
        # also work per line.
        return ["-i", "-e", query[1:-1].replace("\\/", "/")], "regex"
    return None


def timed_peak(cmd, env):
    """Runs cmd and returns (seconds, peak memory in MiB). Its output is
    thrown away: search output can be any bytes, not text."""
    start = time.perf_counter()
    done = subprocess.run(["/usr/bin/time", "-l"] + cmd, env=env, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE)
    seconds = time.perf_counter() - start
    if done.returncode not in (0, 1):
        raise RuntimeError(f"{cmd} exited with {done.returncode}")
    report = done.stderr.decode("utf-8", "replace")
    return seconds, int(re.search(r"(\d+)\s+maximum resident set size", report).group(1)) / 2**20


def collect(cmd, env=None):
    """Runs cmd and returns its whole output as bytes."""
    done = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
    if done.returncode not in (0, 1):
        raise RuntimeError(f"{cmd} exited with {done.returncode}")
    return done.stdout


def cs_matches(out):
    """The set of (repo/path, line) in code-search's text output."""
    found = set()
    for line in out.splitlines():
        m = CS_LINE.match(line)
        if m:
            found.add((m.group(1) + b"/" + m.group(2), int(m.group(3))))
    return found


def rg_matches(out, checkouts):
    """The set of (repo/path, line) in rg's output, paths made like
    code-search's: the checkout folder becomes the repo's name."""
    prefixes = [(str(path).encode() + b"/", name.encode() + b"/") for name, path in checkouts]
    found = set()
    for line in out.splitlines():
        m = RG_LINE.match(line)
        if not m:
            continue
        path = m.group(1)
        for prefix, repo in prefixes:
            if path.startswith(prefix):
                found.add((repo + path[len(prefix):], int(m.group(2))))
                break
    return found


def run_until_lines(cmd, lines, env=None):
    """Seconds until cmd has printed `lines` lines (or ended), then stops it."""
    start = time.perf_counter()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
    seen = 0
    while seen < lines:
        chunk = proc.stdout.read1(1 << 16)
        if not chunk:
            break
        seen += chunk.count(b"\n")
    elapsed = time.perf_counter() - start
    proc.kill()
    proc.wait()
    return elapsed


def timings(commands, runs):
    """Runs each (name, function returning seconds) `runs` times, taking
    turns so a slow moment hits every tool alike, after one warm-up run
    each. Returns {name: [seconds, ...]}."""
    for _, fn in commands:
        fn()
    out = {name: [] for name, _ in commands}
    for _ in range(runs):
        for name, fn in commands:
            out[name].append(fn())
    return out


def p50_p90(values):
    return f"{ms(statistics.median(values))} / {ms(percentile(values, 0.9))}"


def times_faster(slow, fast):
    return f"{slow / fast:.0f}x" if slow / fast >= 10 else f"{slow / fast:.1f}x"


def machine_info(cs):
    chip = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip()
    cores = os.cpu_count()
    memory = subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True).stdout.strip()
    rg = subprocess.run(["rg", "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    ours = subprocess.run([cs, "--version"], capture_output=True, text=True).stdout.strip()
    print("### Setup\n")
    print(f"- {chip or platform.processor()}, {cores} cores, "
          f"{int(memory) / 2**30:.0f} GiB, {platform.system()} {platform.release()}")
    print(f"- {ours}, release build; {rg}")
    load = " ".join(f"{x:.1f}" for x in os.getloadavg())
    print(f"- Load average when it started: {load}. Above a few, other programs are using the "
          "cores, and times are higher for both tools.")
    print("- Warm file cache: each command runs once before it's timed.\n")


def bench_same_results(cs, queries, checkouts, env):
    rows = []
    paths = [str(path) for _, path in checkouts]
    for group, query in queries:
        rg = rg_query(query)
        if rg is None:
            continue
        args, _ = rg
        ours = cs_matches(collect([cs, "search", "--color=never", "--max-results", "0", "--", query], env))
        cells = [group, f"`{query}`", f"{len(ours):,}"]
        for extra in ([], MATCHED_FILES):
            theirs = rg_matches(collect(["rg", "-n", "--no-heading", "--with-filename"] + extra + args + paths),
                                checkouts)
            cells += [f"{len(theirs):,}", f"{len(ours - theirs):,}", f"{len(theirs - ours):,}"]
        rows.append(cells)
    print("### Same results?\n")
    print("Matching lines, as (file, line). \"Only cs\" and \"only rg\" are lines one found and "
          "the other didn't; rg matched searches the files code-search indexes.\n")
    table(["Group", "Query", "code-search", "rg", "only cs", "only rg",
           "rg matched", "only cs", "only rg"], rows)


def bench_speed(cs, queries, checkouts, env, runs):
    paths = [str(path) for _, path in checkouts]
    modes = [
        ("All results", [], [], None),
        ("Files only (-l)", ["-l"], ["-l"], None),
        ("First 10 lines", [], [], 10),
    ]
    detail = []
    groups = {}
    for group, query in queries:
        rg = rg_query(query)
        if rg is None:
            continue
        args, _ = rg
        for mode, cs_extra, rg_extra, lines in modes:
            def run(cmd, mode_lines=lines):
                if mode_lines is None:
                    return run_stream(cmd, env)[1]
                return run_until_lines(cmd, mode_lines, env)

            cs_cmd = [cs, "search", "--color=never", "--max-results", "0"] + cs_extra + ["--", query]
            rg_cmd = ["rg", "-n", "--no-heading"] + rg_extra + args + paths
            rg8_cmd = ["rg", "-n", "--no-heading", "-j", "8"] + MATCHED_FILES + rg_extra + args + paths
            got = timings([
                ("cs", lambda c=cs_cmd: run(c)),
                ("rg", lambda c=rg_cmd: run(c)),
                ("rg8", lambda c=rg8_cmd: run(c)),
            ], runs)
            groups.setdefault((mode, group), []).append(got)
            if mode == "All results":
                cs_t, rg_t = statistics.median(got["cs"]), statistics.median(got["rg"])
                detail.append([group, f"`{query}`", p50_p90(got["cs"]), p50_p90(got["rg"]),
                               p50_p90(got["rg8"]), times_faster(rg_t, cs_t)])

    print(f"### Speed, fresh process (ms, p50 / p90 of {runs} runs)\n")
    print("rg is as it comes (all cores, its own file rules); rg matched searches the files "
          "code-search indexes, on 8 threads like code-search.\n")
    print("All results, by query:\n")
    table(["Group", "Query", "code-search", "rg", "rg matched, -j 8", "Faster by"], detail)

    for mode, _, _, _ in modes:
        rows = []
        for (m, group), results in groups.items():
            if m != mode:
                continue
            medians = {tool: [statistics.median(r[tool]) for r in results] for tool in ("cs", "rg", "rg8")}
            cs_mid, rg_mid = statistics.median(medians["cs"]), statistics.median(medians["rg"])
            rows.append([group, str(len(results)), ms(cs_mid), ms(rg_mid),
                         ms(statistics.median(medians["rg8"])), times_faster(rg_mid, cs_mid)])
        print(f"{mode}, by group (median of the queries' medians):\n")
        table(["Group", "Queries", "code-search", "rg", "rg matched, -j 8", "Faster by"], rows)
    return groups


def bench_repo_size(cs, checkouts, env, runs):
    queries = ["MustCompile", "func", r"/\berr != nil/"]
    rows = []
    for name, path in checkouts:
        content = sum(f.stat().st_size for f in path.rglob("*") if f.is_file() and ".git" not in f.parts)
        for query in queries:
            args, _ = rg_query(query)
            got = timings([
                ("cs", lambda q=query, n=name: run_stream(
                    [cs, "search", "--color=never", "--max-results", "0", "--repo", n, "--", q], env)[1]),
                ("rg", lambda a=args, p=path: run_stream(["rg", "-n", "--no-heading"] + a + [str(p)], env)[1]),
            ], runs)
            cs_t, rg_t = statistics.median(got["cs"]), statistics.median(got["rg"])
            rows.append([name, f"{content / 2**20:.0f} MiB", f"`{query}`", ms(cs_t), ms(rg_t),
                         times_faster(rg_t, cs_t)])
    print(f"### Repo size (ms, median of {runs} runs, one repo at a time)\n")
    table(["Repo", "Files on disk", "Query", "code-search", "rg", "Faster by"], rows)


def bench_server(cs, queries, checkouts, env, runs):
    paths = [str(path) for _, path in checkouts]
    port = 7811
    server = subprocess.Popen([cs, "serve", "--port", str(port)], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    groups = {}
    try:
        for group, query in queries:
            rg = rg_query(query)
            if rg is None:
                continue
            args, _ = rg
            url = f"http://127.0.0.1:{port}/api/v1/search?" + urllib.parse.urlencode(
                {"q": query, "max_results": "0"})

            def fetch(u=url):
                start = time.perf_counter()
                with urllib.request.urlopen(u) as answer:
                    answer.read()
                return time.perf_counter() - start

            got = timings([
                ("server", fetch),
                ("rg", lambda a=args: run_stream(["rg", "-n", "--no-heading"] + a + paths, env)[1]),
            ], runs)
            groups.setdefault(group, []).append(got)
    finally:
        server.terminate()
        server.wait()
    rows = []
    for group, results in groups.items():
        server_mid = statistics.median(statistics.median(r["server"]) for r in results)
        rg_mid = statistics.median(statistics.median(r["rg"]) for r in results)
        rows.append([group, str(len(results)), ms(server_mid), ms(rg_mid), times_faster(rg_mid, server_mid)])
    print(f"### A running server (ms, median of the queries' medians, {runs} runs)\n")
    print("code-search serve answers over HTTP with all results as JSON; rg is a new process each time.\n")
    table(["Group", "Queries", "code-search serve", "rg", "Faster by"], rows)


def bench_memory(cs, checkouts, env):
    paths = [str(path) for _, path in checkouts]
    rows = []
    for query in ["MustCompile", "the", r"/\berr != nil/", "a"]:
        args, _ = rg_query(query)
        _, cs_peak = timed_peak([cs, "search", "--color=never", "--max-results", "0", "--", query], env)
        _, rg_peak = timed_peak(["rg", "-n", "--no-heading"] + args + paths, env)
        rows.append([f"`{query}`", f"{cs_peak:.0f} MiB", f"{rg_peak:.0f} MiB"])
    print("### Peak memory\n")
    print("code-search maps its index files, so its count includes the index pages a search "
          "touched; the operating system shares those between processes.\n")
    table(["Query", "code-search", "rg"], rows)


def bench_index_cost(cs, checkouts, env, groups):
    ws = Path(env["CODE_SEARCH_HOME"])
    rows = []
    build_total = 0.0
    for name, path in checkouts:
        content = sum(f.stat().st_size for f in path.rglob("*") if f.is_file() and ".git" not in f.parts)
        rebuild, peak = timed_peak([cs, "update", name, "--rebuild"], env)
        noop, _ = timed_peak([cs, "update", name], env)
        build_total += rebuild
        rows.append([name, f"{content / 2**20:.0f} MiB", f"{rebuild:.2f} s", f"{peak:.0f} MiB",
                     f"{noop * 1000:.0f} ms"])
    index_bytes = sum(f.stat().st_size for f in (ws / "indexes").iterdir())
    print("### What the index costs\n")
    table(["Repo", "Files on disk", "Build", "Peak memory", "No-op update"], rows)
    print(f"All indexes: {index_bytes / 2**20:.0f} MiB on disk.\n")

    # Searches that pay for one build: the build time over the time each
    # search saves, at the median query of each group.
    rows = []
    for (mode, group), results in groups.items():
        if mode != "All results":
            continue
        cs_mid = statistics.median(statistics.median(r["cs"]) for r in results)
        rg_mid = statistics.median(statistics.median(r["rg"]) for r in results)
        saved = rg_mid - cs_mid
        rows.append([group, ms(saved), f"{build_total / saved:.0f}" if saved > 0 else "never"])
    print(f"Building all three indexes took {build_total:.2f} s. Searches it takes to win that back, "
          "at each group's median query:\n")
    table(["Group", "Saved per search (ms)", "Searches to pay for the build"], rows)
    print("An update after new commits rebuilds the whole repo's index, so this is also the cost "
          "of each update that changes something.\n")


# ripgrep's benchsuite clones this fork of the kernel. Pinned so runs can
# be repeated: its HEAD when this was written.
LINUX = ("linux", "https://github.com/BurntSushi/linux", "84e57d292203a45c96dbcb2e6be9dd80961d981a")

# rg's Unicode \w, written out for RE2, whose \w is ASCII only: letters,
# marks, decimal digits and connector punctuation (like '_').
UNICODE_WORD = r"[\p{L}\p{M}\p{Nd}\p{Pc}]"

# The linux_* benchmarks of ripgrep's benchsuite: name, the rg arguments it
# runs (from the kernel's folder), and the same search as a code-search
# query. rg is case-sensitive unless given -i; code-search is the other way
# around, so case:yes. -w becomes \b on both sides of the text.
BENCHSUITE = [
    ("linux_literal_default", ["PM_RESUME"], "case:yes PM_RESUME"),
    ("linux_literal", ["-n", "PM_RESUME"], "case:yes PM_RESUME"),
    ("linux_literal_casei", ["-n", "-i", "PM_RESUME"], "PM_RESUME"),
    ("linux_re_literal_suffix", ["-n", "[A-Z]+_RESUME"], "case:yes /[A-Z]+_RESUME/"),
    ("linux_word", ["-n", "-w", "PM_RESUME"], r"case:yes /\bPM_RESUME\b/"),
    ("linux_unicode_greek", ["-n", r"\p{Greek}"], r"case:yes /\p{Greek}/"),
    ("linux_unicode_greek_casei", ["-n", "-i", r"\p{Greek}"], r"/\p{Greek}/"),
    ("linux_unicode_word", ["-n", r"\wAh"], f"case:yes /{UNICODE_WORD}Ah/"),
    ("linux_no_literal (ASCII)", ["-n", r"(?-u)\w{5}\s+\w{5}\s+\w{5}\s+\w{5}\s+\w{5}"],
     r"case:yes /\w{5}\s+\w{5}\s+\w{5}\s+\w{5}\s+\w{5}/"),
    ("linux_no_literal (rg's Unicode)", ["-n", r"\w{5}\s+\w{5}\s+\w{5}\s+\w{5}\s+\w{5}"],
     r"case:yes /\w{5}\s+\w{5}\s+\w{5}\s+\w{5}\s+\w{5}/"),
    ("linux_alternates", ["-n", "ERR_SYS|PME_TURN_OFF|LINK_REQ_RST|CFG_BME_EVT"],
     "case:yes /ERR_SYS|PME_TURN_OFF|LINK_REQ_RST|CFG_BME_EVT/"),
    ("linux_alternates_casei", ["-n", "-i", "ERR_SYS|PME_TURN_OFF|LINK_REQ_RST|CFG_BME_EVT"],
     "/ERR_SYS|PME_TURN_OFF|LINK_REQ_RST|CFG_BME_EVT/"),
]


def bench_benchsuite(cs, checkout, env, runs):
    ws = Path(env["CODE_SEARCH_HOME"])
    if not (ws / "registry.json").exists():
        build, peak = timed_peak([cs, "add", str(checkout)], env)
        print(f"Indexed the kernel in {build:.1f} s, peak memory {peak:.0f} MiB.\n")
    index_bytes = sum(f.stat().st_size for f in (ws / "indexes").iterdir())
    files = int(subprocess.run(["git", "-C", str(checkout), "ls-files"], capture_output=True,
                               check=True).stdout.count(b"\n"))
    rows = []
    for name, rg_args, query in BENCHSUITE:
        # -n everywhere for counting lines the same way; with a path, so rg's
        # paths can be matched to code-search's.
        theirs = rg_matches(collect(["rg", "-n", "--no-heading", "--with-filename"] +
                                    [a for a in rg_args if a != "-n"] + [str(checkout)]),
                            [("linux", checkout)])
        ours = cs_matches(collect([cs, "search", "--color=never", "--max-results", "0", "--", query], env))
        got = timings([
            ("cs", lambda q=query: run_stream([cs, "search", "--color=never", "--max-results", "0", "--", q],
                                              env)[1]),
            ("rg", lambda a=rg_args: run_stream(["rg"] + a + [str(checkout)], env)[1]),
            ("rg8", lambda a=rg_args: run_stream(["rg", "-j", "8"] + MATCHED_FILES + a + [str(checkout)],
                                                 env)[1]),
        ], runs)
        cs_t, rg_t = statistics.median(got["cs"]), statistics.median(got["rg"])
        rg8_t = statistics.median(got["rg8"])
        rows.append([name, "`rg " + " ".join(rg_args).replace("|", "\\|") + "`",
                     "`" + query.replace("|", "\\|") + "`", p50_p90(got["cs"]), p50_p90(got["rg"]),
                     times_faster(rg_t, cs_t), p50_p90(got["rg8"]), times_faster(rg8_t, cs_t),
                     f"{len(ours):,}", f"{len(theirs):,}",
                     f"{len(ours - theirs):,} / {len(theirs - ours):,}"])
    print(f"### ripgrep's benchsuite: the Linux kernel (ms, p50 / p90 of {runs} runs)\n")
    print(f"BurntSushi/linux at {LINUX[2][:10]}, {files:,} tracked files; code-search's index: "
          f"{index_bytes / 2**20:.0f} MiB. Each tool is a new process printing every matching line "
          "to a pipe. The suite also builds the kernel first, to test skipping ignored build "
          "files; that isn't done here (it needs Linux), so rg has nothing extra to skip.\n")
    print("\"rg\" is the suite's own command. \"rg matched\" adds the file rules code-search "
          "indexes by and -j 8: the kernel has 84 files over 1 MB (generated AMD GPU register "
          "headers of up to 12 MB) that code-search leaves out, and rg as it comes reads. Those "
          "are the lines only rg finds.\n")
    table(["Benchmark", "rg command", "code-search query", "code-search", "rg", "Faster by",
           "rg matched, -j 8", "Faster by", "Lines, cs", "Lines, rg", "Only cs / only rg"], rows)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("work_dir", type=Path)
    parser.add_argument("--code-search", required=True)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--only", choices=["same", "speed", "size", "server", "memory", "cost", "benchsuite"])
    parser.add_argument("--no-linux", action="store_true", help="skip ripgrep's benchsuite on the kernel")
    args = parser.parse_args()

    repos = read_tsv(HERE / "repos.tsv")
    queries = [tuple(q) for q in read_tsv(HERE / "queries.tsv")] + EXTRA_QUERIES
    repo_dir = args.work_dir / "repos"
    for name, url, commit in repos:
        clone(url, commit, repo_dir / name)
    checkouts = [(name, (repo_dir / name).resolve()) for name, _, _ in repos]
    env = dict(os.environ, CODE_SEARCH_HOME=str(args.work_dir / "ws"))
    cs = str(Path(args.code_search).resolve())
    if not (args.work_dir / "ws" / "registry.json").exists():
        for name, path in checkouts:
            subprocess.run([cs, "add", str(path)], env=env, check=True, stdout=subprocess.DEVNULL)

    machine_info(cs)
    groups = {}
    if args.only in (None, "same"):
        bench_same_results(cs, queries, checkouts, env)
    if args.only in (None, "speed", "cost"):
        groups = bench_speed(cs, queries, checkouts, env, args.runs)
    if args.only in (None, "size"):
        bench_repo_size(cs, checkouts, env, args.runs)
    if args.only in (None, "server"):
        bench_server(cs, queries, checkouts, env, args.runs)
    if args.only in (None, "memory"):
        bench_memory(cs, checkouts, env)
    if args.only in (None, "cost"):
        bench_index_cost(cs, checkouts, env, groups)
    if args.only in (None, "benchsuite") and not args.no_linux:
        name, url, commit = LINUX
        clone(url, commit, repo_dir / name)
        # A workspace of its own, so the other parts keep searching only
        # the three repos.
        linux_env = dict(os.environ, CODE_SEARCH_HOME=str(args.work_dir / "ws-linux"))
        bench_benchsuite(cs, (repo_dir / name).resolve(), linux_env, args.runs)


if __name__ == "__main__":
    main()
