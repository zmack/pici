#!/usr/bin/env python3
"""Run clang-tidy over src/*.cpp with bounded, configurable parallelism.

Invoked by the CMake `tidy` and `tidy-fix` targets.  It drives clang-tidy
directly (one process per translation unit) instead of run-clang-tidy, so
behaviour does not depend on which run-clang-tidy release is installed.

Exit status:
  0  every analyzed file ran clang-tidy successfully (warnings are advisory
     unless --strict is given)
  1  clang-tidy failed on a file (non-zero exit, timeout, could not start),
     coverage was empty/stale, or --strict and warnings were reported
  2  bad invocation (missing tool or compile database)
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import threading

WARNING_RE = re.compile(r"^.+:\d+:\d+: warning: ", re.MULTILINE)
NOISE_RE = re.compile(r"^(\d+ warnings? generated\.|Suppressed \d+ warnings .*|Use -header-filter=.*)\n",
                      re.MULTILINE)
MEM_PER_JOB_KB = 1_500_000


def available_cpus():
    try:
        return len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        return os.cpu_count() or 1


def mem_available_kb():
    try:
        with open("/proc/meminfo", encoding="ascii") as meminfo:
            for line in meminfo:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1])
    except (OSError, ValueError):
        pass
    return None


def resolve_jobs(requested, fix, max_auto):
    """0 means auto: bounded by CPUs, available memory, and max_auto."""
    if requested > 0:
        return requested
    if fix:
        # Concurrent -fix runs race on edits to shared headers.
        return 1
    jobs = min(available_cpus(), max_auto)
    mem = mem_available_kb()
    if mem is not None:
        jobs = min(jobs, mem // MEM_PER_JOB_KB)
    return max(1, jobs)


def discover(source_root, build_dir):
    """Return (analyzable, missing_from_db) absolute paths under src/."""
    db_path = os.path.join(build_dir, "compile_commands.json")
    with open(db_path, encoding="utf-8") as db_file:
        entries = json.load(db_file)
    in_db = set()
    for entry in entries:
        path = entry["file"]
        if not os.path.isabs(path):
            path = os.path.join(entry.get("directory", ""), path)
        in_db.add(os.path.realpath(path))

    analyzable, missing = [], []
    src_dir = os.path.join(source_root, "src")
    for dirpath, dirnames, filenames in os.walk(src_dir):
        dirnames.sort()
        for name in sorted(filenames):
            if name.endswith(".cpp"):
                path = os.path.join(dirpath, name)
                (analyzable if os.path.realpath(path) in in_db else missing).append(path)
    return analyzable, missing


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--clang-tidy", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--jobs", type=int, default=0,
                        help="parallel clang-tidy processes; 0 = auto (default)")
    parser.add_argument("--max-auto-jobs", type=int, default=4,
                        help="upper bound applied only when --jobs is 0")
    parser.add_argument("--timeout", type=int, default=900,
                        help="per-file timeout in seconds")
    parser.add_argument("--fix", action="store_true")
    parser.add_argument("--strict", action="store_true",
                        help="fail when any warning is reported")
    parser.add_argument("--no-analyzer", action="append", default=[],
                        metavar="REL_PATH",
                        help="source-root-relative file analyzed with clang-analyzer-* disabled")
    parser.add_argument("--extra-arg", action="append", default=[])
    parser.add_argument("--list", action="store_true",
                        help="print the files that would be analyzed and exit")
    args = parser.parse_args()

    if args.jobs < 0 or args.max_auto_jobs < 1 or args.timeout < 1:
        parser.error("--jobs must be non-negative; --max-auto-jobs and --timeout must be positive")
    args.source_root = os.path.realpath(args.source_root)
    args.build_dir = os.path.realpath(args.build_dir)
    args.clang_tidy = os.path.abspath(args.clang_tidy)

    if not os.access(args.clang_tidy, os.X_OK):
        print(f"run_clang_tidy: not executable: {args.clang_tidy}", file=sys.stderr)
        return 2
    try:
        files, missing = discover(args.source_root, args.build_dir)
    except (OSError, ValueError, KeyError, TypeError) as err:
        print(f"run_clang_tidy: cannot read compile database in {args.build_dir}: {err}",
              file=sys.stderr)
        return 2

    for path in missing:
        print(f"run_clang_tidy: skipped (not in compile database): "
              f"{os.path.relpath(path, args.source_root)}", file=sys.stderr)
    if not files:
        print("run_clang_tidy: no translation units to analyze", file=sys.stderr)
        return 1

    no_analyzer = {os.path.realpath(os.path.join(args.source_root, rel))
                   for rel in args.no_analyzer}
    stale = [rel for rel in args.no_analyzer
             if os.path.realpath(os.path.join(args.source_root, rel)) not in
             {os.path.realpath(f) for f in files}]
    for rel in stale:
        print(f"run_clang_tidy: --no-analyzer file not analyzed: {rel}", file=sys.stderr)

    jobs = min(resolve_jobs(args.jobs, args.fix, args.max_auto_jobs), len(files))
    if args.list:
        for path in files:
            print(path)
        return 1 if stale else 0
    print(f"run_clang_tidy: {len(files)} files, {jobs} job(s), {args.clang_tidy}",
          flush=True)

    base = [args.clang_tidy, "-p", args.build_dir, "-quiet"]
    base += [f"-extra-arg={arg}" for arg in args.extra_arg]
    if args.fix:
        base.append("-fix")
    if args.strict:
        base.append("-warnings-as-errors=*")

    print_lock = threading.Lock()
    stop = threading.Event()

    def run_one(path):
        if stop.is_set():
            return path, -1, "not run because clang-tidy could not start\n"
        cmd = list(base)
        if os.path.realpath(path) in no_analyzer:
            cmd.append("-checks=-clang-analyzer-*")
        cmd.append(path)
        try:
            proc = subprocess.run(cmd, cwd=args.source_root, text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=args.timeout, check=False)
            code, output = proc.returncode, proc.stdout
        except subprocess.TimeoutExpired as err:
            code, output = -1, f"timed out after {args.timeout}s\n{err.stdout or ''}"
        except OSError as err:
            code, output = -1, f"could not start {args.clang_tidy}: {err}\n"
            stop.set()
        shown = NOISE_RE.sub("", output)
        if shown.strip():
            output_to_print = shown
            with print_lock:
                print(output_to_print, end="" if output_to_print.endswith("\n") else "\n", flush=True)
        return path, code, output

    failed, warned = [], 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for path, code, output in pool.map(run_one, files):
            if code != 0 or "Error parsing " in output:
                failed.append(path)
            warned += len(WARNING_RE.findall(output))

    print(f"run_clang_tidy: {len(files) - len(failed)}/{len(files)} files ok, "
          f"{warned} warning(s), {len(failed)} failure(s)")
    for path in failed:
        print(f"run_clang_tidy: FAILED {os.path.relpath(path, args.source_root)}",
              file=sys.stderr)
    if failed or stale:
        return 1
    if args.strict and warned:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
