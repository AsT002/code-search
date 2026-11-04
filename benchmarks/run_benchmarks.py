#!/usr/bin/env python3
"""Fresh-process benchmarks for code-search. See docs/BENCHMARKS.md.

    run_benchmarks.py <work dir> --code-search <binary> [--runs 3] [--skip-build]

Clones the repos in repos.tsv at their pinned commits into <work dir>/repos,
adds them to a workspace in <work dir>/ws, then prints Markdown tables:

- build: time, peak memory and index size for each repo, and a no-op update
- search: each query in queries.tsv as a new process, with rg and git grep
  on the same checkouts for the queries they can run
- during an update: search times while the largest repo is rebuilt

Needs git, rg and /usr/bin/time. Release builds only.
"""

import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def read_tsv(path):
    rows = []
    for line in path.read_text().splitlines():
        if line and not line.startswith("#") and "\t" in line:
            rows.append(line.split("\t"))
    return rows


def head_of(checkout):
    done = subprocess.run(["git", "-C", str(checkout), "rev-parse", "HEAD"], capture_output=True, text=True)
    return done.stdout.strip() if done.returncode == 0 else None


def case_clashes(checkout):
    """Lowercased paths of tracked files whose names differ only in case."""
    names = subprocess.run(["git", "-C", str(checkout), "ls-files"], capture_output=True, text=True,
                           check=True).stdout.splitlines()
    seen = {}
    for name in names:
        seen[name.lower()] = seen.get(name.lower(), 0) + 1
    return {name for name, count in seen.items() if count > 1}


def clone(url, commit, dest):
    """Makes dest a checkout of commit. Checks one that's already there."""
    if dest.exists():
        if head_of(dest) != commit:
            sys.exit(f"{dest} isn't a checkout of {commit}. Remove it to clone it again")
        # code-search indexes the commit, but rg and git grep read the files
        # on disk, so they must be the same: no changed or untracked files.
        # --untracked-files=all: list untracked files even if the repo's
        # status.showUntrackedFiles setting would hide them.
        status = subprocess.run(["git", "-C", str(dest), "status", "--porcelain", "--untracked-files=all"],
                                capture_output=True, text=True, check=True).stdout
        # On a disk that ignores case (macOS's default), of two tracked
        # files whose names differ only in case (the kernel has 13 pairs,
        # like xt_MARK.h and xt_mark.h) only one can exist, so the other
        # shows as changed. Those are the only changes allowed.
        clashing = case_clashes(dest)
        status = "".join(line + "\n" for line in status.splitlines() if line[3:].lower() not in clashing)
        if status:
            sys.exit(f"{dest} has changes, so rg and git grep would search other files than "
                     f"code-search:\n{status}Undo them, or remove it to clone it again")
        return
    # Clone next to it, and rename only once it's complete, so a clone that
    # fails partway never looks finished to the next run.
    partial = dest.with_name(dest.name + ".partial")
    if partial.exists():
        shutil.rmtree(partial)
    partial.mkdir(parents=True)
    git = ["git", "-C", str(partial)]
    subprocess.run(git + ["init", "-q"], check=True)
    subprocess.run(git + ["fetch", "-q", "--depth", "1", url, commit], check=True)
    subprocess.run(git + ["checkout", "-q", "FETCH_HEAD"], check=True)
    partial.rename(dest)


def timed_peak(cmd, env):
    """Runs cmd and returns (seconds, peak memory in MiB)."""
    flag = "-l" if sys.platform == "darwin" else "-v"
    start = time.perf_counter()
    done = subprocess.run(["/usr/bin/time", flag] + cmd, env=env, capture_output=True, text=True)
    seconds = time.perf_counter() - start
    if done.returncode != 0:
        raise RuntimeError(f"{cmd} failed:\n{done.stderr}")
    if sys.platform == "darwin":
        peak = int(re.search(r"(\d+)\s+maximum resident set size", done.stderr).group(1)) / 2**20
    else:
        peak = int(re.search(r"Maximum resident set size \(kbytes\): (\d+)", done.stderr).group(1)) / 2**10
    return seconds, peak


def run_stream(cmd, env=None, cwd=None):
    """Runs cmd, reading all its output. Returns (s to first byte, s to end).

    The programs write to a pipe in blocks (16 KiB for code-search), so the
    first byte arrives when the first block is full or the program ends:
    "first output", not the time to the first result.

    Exit code 1 means nothing was found, for code-search, rg and git grep
    alike. Anything above that is a failure, not a timing, so it raises.
    """
    start = time.perf_counter()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, cwd=cwd)
    first = None
    while True:
        chunk = proc.stdout.read1(1 << 16)
        if not chunk:
            break
        if first is None:
            first = time.perf_counter() - start
    code = proc.wait()
    total = time.perf_counter() - start
    if code not in (0, 1):
        raise RuntimeError(f"{cmd} exited with {code}")
    return (first if first is not None else total), total


# Words that are filters when typed before a ':', as in the query parser.
FILTERS = ("repo", "path", "file", "lang", "case")


def unquote(inner):
    """The text of one quoted part, without its quotes, or None if there's
    another quote in it (like '"a" "b"', which is two words). Inside quotes,
    \\" and \\\\ stand for " and \\; other backslashes stay."""
    out = []
    i = 0
    while i < len(inner):
        c = inner[i]
        if c == '"':
            return None
        if c == "\\" and i + 1 < len(inner) and inner[i + 1] in '"\\':
            out.append(inner[i + 1])
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def plain_text(query):
    """(text, case-sensitive) for a query that is just one literal, or None."""
    case_sensitive = False
    if query.startswith("case:yes "):
        case_sensitive = True
        query = query[len("case:yes "):]
    if len(query) >= 2 and query.startswith('"') and query.endswith('"'):
        text = unquote(query[1:-1])
        return (text, case_sensitive) if text else None
    if not re.fullmatch(r"[^\s\"\\()\-][^\s\"\\()]*", query) or query in ("or", "OR"):
        return None
    if len(query) >= 3 and query.startswith("/") and query.endswith("/"):
        return None  # code-search reads this as a regex, not text
    if ":" in query and query.split(":", 1)[0] in FILTERS:
        return None  # a filter like lang:go, not text
    return query, case_sensitive


def ms(seconds):
    value = seconds * 1000
    return f"{value:.1f}" if value >= 10 else f"{value:.2f}"


def percentile(values, share):
    values = sorted(values)
    rank = max(1, min(len(values), int(share * len(values) + 0.999)))
    return values[rank - 1]


def table(header, rows):
    print("| " + " | ".join(header) + " |")
    print("| " + " | ".join("---" if i == 0 else "---:" for i in range(len(header))) + " |")
    for row in rows:
        print("| " + " | ".join(row) + " |")
    print()


def bench_build(cs, repos, repo_dir, env):
    ws = Path(env["CODE_SEARCH_HOME"])
    if ws.exists():
        shutil.rmtree(ws)
    rows = []
    for name, _, _ in repos:
        seconds, peak = timed_peak([cs, "add", str(repo_dir / name)], env)
        newest = max((ws / "indexes").iterdir(), key=lambda p: p.stat().st_mtime)
        size = newest.stat().st_size  # before update --rebuild replaces the file
        noop, _ = timed_peak([cs, "update", name], env)
        rebuild, rebuild_peak = timed_peak([cs, "update", name, "--rebuild"], env)
        rows.append([name, f"{seconds:.2f} s", f"{rebuild:.2f} s", f"{max(peak, rebuild_peak):.0f} MiB",
                     f"{size / 2**20:.1f} MiB", f"{noop * 1000:.0f} ms"])
    print("### Build\n")
    table(["Repo", "add", "update --rebuild", "Peak memory", "Index size", "No-op update"], rows)


def bench_search(cs, queries, repos, repo_dir, env, runs):
    by_group = {}
    rows = []
    checkouts = [str(repo_dir / name) for name, _, _ in repos]
    for group, query in queries:
        def median_of(cmd_fn):
            firsts, totals = [], []
            for _ in range(runs):
                first, total = cmd_fn()
                firsts.append(first)
                totals.append(total)
            return statistics.median(firsts), statistics.median(totals)

        cs_first, cs_all = median_of(lambda: run_stream([cs, "search", "--color=never", "--", query], env))
        rg_all = gg_all = None
        plain = plain_text(query)
        if plain is not None:
            text, case_sensitive = plain
            # rg's -s means case-sensitive. git grep is case-sensitive unless
            # given -i; its -s means something else.
            rg_case = ["-s"] if case_sensitive else ["-i"]
            git_case = [] if case_sensitive else ["-i"]
            _, rg_all = median_of(lambda: run_stream(["rg", "-F"] + rg_case + ["-n", "--no-heading", "--", text] + checkouts))

            def git_grep():
                first = total = 0.0
                for checkout in checkouts:
                    f, t = run_stream(["git", "grep", "-F"] + git_case + ["-n", "-I", "--", text], cwd=checkout)
                    first = first or f
                    total += t
                return first, total

            _, gg_all = median_of(git_grep)
        elif re.fullmatch(r"/[^\s].*/", query) and "/ " not in query:
            # A regex on its own. rg reads the same syntax for these, and its
            # ^ and $ also work per line. git grep's regex syntax doesn't
            # have \d, \b and the like, so it's left out.
            pattern = query[1:-1].replace("\\/", "/")
            _, rg_all = median_of(lambda: run_stream(["rg", "-i", "-n", "--no-heading", "-e", pattern] + checkouts))
        by_group.setdefault(group, []).append((cs_first, cs_all, rg_all, gg_all))
        rows.append([group, f"`{query}`", ms(cs_first), ms(cs_all),
                     ms(rg_all) if rg_all else "", ms(gg_all) if gg_all else ""])

    print(f"### Fresh-process searches (ms, median of {runs} runs)\n")
    table(["Group", "Query", "First output", "All", "rg", "git grep"], rows)

    group_rows = []
    for group, values in by_group.items():
        def col(i):
            vs = [v[i] for v in values if v[i] is not None]
            return f"{ms(percentile(vs, 0.5))} / {ms(percentile(vs, 0.95))}" if vs else ""
        group_rows.append([group, str(len(values)), col(0), col(1), col(2), col(3)])
    print("### Fresh-process groups (ms, p50 / p95)\n")
    table(["Group", "Queries", "First output", "All", "rg", "git grep"], group_rows)


def bench_during_update(cs, queries, repos, env, runs):
    """Search times with nothing else running, then while the largest repo is rebuilt over and over."""
    picked = [q for g, q in queries if g in ("rare", "common", "boolean")]

    def search_all():
        return [run_stream([cs, "search", "--color=never", "--", q], env)[1] for q in picked]

    quiet = [statistics.median(ts) for ts in zip(*[search_all() for _ in range(runs)])]
    largest = repos[-1][0]
    stop = Path(env["CODE_SEARCH_HOME"]) / "stop-rebuilding"
    stop.unlink(missing_ok=True)  # left over from a run that was stopped
    # The loop stops with exit code 1 as soon as an update fails: searches
    # timed next to a failing update would say nothing about a real one.
    loop = subprocess.Popen(
        ["sh", "-c", f'while [ ! -e "$1" ]; do "$0" update {largest} --rebuild >/dev/null 2>&1 || exit 1; done',
         cs, str(stop)],
        env=env)
    try:
        time.sleep(0.5)
        busy = [statistics.median(ts) for ts in zip(*[search_all() for _ in range(runs)])]
        stopped_early = loop.poll() is not None
    finally:
        stop.touch()
        code = loop.wait()
        stop.unlink()
    if stopped_early or code != 0:
        raise RuntimeError(f"update {largest} --rebuild failed while searches were timed")
    print("### Searches during an update (ms, median of runs)\n")
    rows = [[f"`{q}`", ms(a), ms(b)] for q, a, b in zip(picked, quiet, busy)]
    rows.append(["**p50**", ms(percentile(quiet, 0.5)), ms(percentile(busy, 0.5))])
    rows.append(["**p95**", ms(percentile(quiet, 0.95)), ms(percentile(busy, 0.95))])
    table(["Query", "Nothing else running", f"While `update {largest} --rebuild` loops"], rows)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("work_dir", type=Path)
    parser.add_argument("--code-search", required=True)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--skip-build", action="store_true", help="reuse the workspace from an earlier run")
    parser.add_argument("--only", choices=["build", "search", "update"], help="run just one part")
    parser.add_argument("--group", help="time only this group's queries in the search part")
    args = parser.parse_args()

    repos = read_tsv(HERE / "repos.tsv")
    queries = read_tsv(HERE / "queries.tsv")
    # --group only picks what the search part runs. The update part uses its
    # own fixed set of groups.
    searched = [q for q in queries if args.group in (None, q[0])]
    if not searched:
        sys.exit(f"no queries in group {args.group}")
    repo_dir = args.work_dir / "repos"
    for name, url, commit in repos:
        clone(url, commit, repo_dir / name)
    env = dict(os.environ, CODE_SEARCH_HOME=str(args.work_dir / "ws"))
    cs = str(Path(args.code_search).resolve())

    if not args.skip_build and args.only in (None, "build"):
        bench_build(cs, repos, repo_dir, env)
    if args.only in (None, "search"):
        bench_search(cs, searched, repos, repo_dir, env, args.runs)
    if args.only in (None, "update"):
        bench_during_update(cs, queries, repos, env, args.runs)


if __name__ == "__main__":
    main()
