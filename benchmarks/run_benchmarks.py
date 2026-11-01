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


def clone(url, commit, dest):
    if dest.exists():
        return
    dest.mkdir(parents=True)
    git = ["git", "-C", str(dest)]
    subprocess.run(git + ["init", "-q"], check=True)
    subprocess.run(git + ["fetch", "-q", "--depth", "1", url, commit], check=True)
    subprocess.run(git + ["checkout", "-q", "FETCH_HEAD"], check=True)


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
    """Runs cmd, reading all its output. Returns (s to first byte, s to end)."""
    start = time.perf_counter()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, cwd=cwd)
    first = None
    while True:
        chunk = proc.stdout.read1(1 << 16)
        if not chunk:
            break
        if first is None:
            first = time.perf_counter() - start
    proc.wait()
    total = time.perf_counter() - start
    return (first if first is not None else total), total


def plain_text(query):
    """The literal text of a query that is just one literal, or None."""
    case_sensitive = False
    if query.startswith("case:yes "):
        case_sensitive = True
        query = query[len("case:yes "):]
    if len(query) >= 2 and query.startswith('"') and query.endswith('"'):
        return query[1:-1].replace('\\"', '"').replace("\\\\", "\\"), case_sensitive
    if re.fullmatch(r"[^\s\"\\()\-][^\s\"\\()]*", query) and ":" not in query and query not in ("or", "OR"):
        return query, case_sensitive
    return None


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
            case = "-s" if case_sensitive else "-i"
            _, rg_all = median_of(lambda: run_stream(["rg", "-F", case, "-n", "--no-heading", "--", text] + checkouts))

            def git_grep():
                first = total = 0.0
                for checkout in checkouts:
                    f, t = run_stream(["git", "grep", "-F", case, "-n", "-I", "--", text], cwd=checkout)
                    first = first or f
                    total += t
                return first, total

            _, gg_all = median_of(git_grep)
        by_group.setdefault(group, []).append((cs_first, cs_all, rg_all, gg_all))
        rows.append([group, f"`{query}`", ms(cs_first), ms(cs_all),
                     ms(rg_all) if rg_all else "", ms(gg_all) if gg_all else ""])

    print(f"### Fresh-process searches (ms, median of {runs} runs)\n")
    table(["Group", "Query", "First result", "All", "rg", "git grep"], rows)

    group_rows = []
    for group, values in by_group.items():
        def col(i):
            vs = [v[i] for v in values if v[i] is not None]
            return f"{ms(percentile(vs, 0.5))} / {ms(percentile(vs, 0.95))}" if vs else ""
        group_rows.append([group, str(len(values)), col(0), col(1), col(2), col(3)])
    print("### Fresh-process groups (ms, p50 / p95)\n")
    table(["Group", "Queries", "First result", "All", "rg", "git grep"], group_rows)


def bench_during_update(cs, queries, repos, env, runs):
    """Search times with nothing else running, then while the largest repo is rebuilt over and over."""
    picked = [q for g, q in queries if g in ("rare", "common", "boolean")]

    def search_all():
        return [run_stream([cs, "search", "--color=never", "--", q], env)[1] for q in picked]

    quiet = [statistics.median(ts) for ts in zip(*[search_all() for _ in range(runs)])]
    largest = repos[-1][0]
    stop = Path(env["CODE_SEARCH_HOME"]) / "stop-rebuilding"
    loop = subprocess.Popen(
        ["sh", "-c", f'while [ ! -e "$1" ]; do "$0" update {largest} --rebuild >/dev/null 2>&1; done', cs, str(stop)],
        env=env)
    try:
        time.sleep(0.5)
        busy = [statistics.median(ts) for ts in zip(*[search_all() for _ in range(runs)])]
    finally:
        stop.touch()
        loop.wait()
        stop.unlink()
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
    args = parser.parse_args()

    repos = read_tsv(HERE / "repos.tsv")
    queries = read_tsv(HERE / "queries.tsv")
    repo_dir = args.work_dir / "repos"
    for name, url, commit in repos:
        clone(url, commit, repo_dir / name)
    env = dict(os.environ, CODE_SEARCH_HOME=str(args.work_dir / "ws"))
    cs = str(Path(args.code_search).resolve())

    if not args.skip_build and args.only in (None, "build"):
        bench_build(cs, repos, repo_dir, env)
    if args.only in (None, "search"):
        bench_search(cs, queries, repos, repo_dir, env, args.runs)
    if args.only in (None, "update"):
        bench_during_update(cs, queries, repos, env, args.runs)


if __name__ == "__main__":
    main()
