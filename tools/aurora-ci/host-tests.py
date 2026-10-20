#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run the tree's hermetic host test suites, as listed in host-tests.json.

The suites under tools/ that need no Apple hardware, root or network: Python
and Rust tests of driver logic, their mutation checks, and tool self-tests.
Each one runs from the top of the tree with its own output directory, and the
tree must be as clean afterwards as before.

host-tests.json lists every suite with the toolchain it needs. Suites that
cannot run here are listed under "excluded" with the reason; the unit tests
fail when a file that looks like a suite is in neither list, so a new suite
is never silently left out.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

MANIFEST = Path(__file__).with_name("host-tests.json")


def load_manifest(path=MANIFEST):
    manifest = json.loads(Path(path).read_text())
    names = [suite["name"] for suite in manifest["suites"]]
    if len(names) != len(set(names)):
        raise ValueError("host-tests.json: suite names must be unique")
    return manifest


def rust_toolchains(manifest):
    return sorted({suite["rust"] for suite in manifest["suites"] if suite.get("rust")})


def install_toolchains(manifest):
    packages = manifest.get("apt", [])
    if packages:
        subprocess.run(["sudo", "apt-get", "update", "-q"], check=True)
        subprocess.run(["sudo", "apt-get", "install", "-y", "-q", "--no-install-recommends", *packages], check=True)
    for version in rust_toolchains(manifest):
        subprocess.run(["rustup", "toolchain", "install", version, "--profile", "minimal"], check=True)


def tree_state(tree):
    """What git sees as changed or untracked (ignored build files excluded)."""
    result = subprocess.run(["git", "-C", str(tree), "status", "--porcelain", "--untracked-files=all"],
                            check=True, text=True, stdout=subprocess.PIPE)
    return set(result.stdout.splitlines())


def run_suite(suite, tree, output):
    workdir = output / suite["name"]
    workdir.mkdir(parents=True, exist_ok=True)
    argv = [part.replace("{output}", str(workdir)) for part in suite["argv"]]
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    if suite.get("rust"):
        env["RUSTUP_TOOLCHAIN"] = suite["rust"]
    started = time.monotonic()
    try:
        result = subprocess.run(argv, cwd=tree, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=suite.get("timeout", 900))
        status = "pass" if result.returncode == 0 else f"fail ({result.returncode})"
        log = result.stdout
    except subprocess.TimeoutExpired as expired:
        status, log = "timeout", (expired.stdout or b"").decode(errors="replace") if isinstance(
            expired.stdout, bytes) else (expired.stdout or "")
    seconds = time.monotonic() - started
    (workdir / "log.txt").write_text(log)
    return {"name": suite["name"], "status": status, "seconds": round(seconds, 1), "log": log}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--tree", type=Path, default=Path("."))
    parser.add_argument("--output", type=Path, help="directory for each suite's output and log")
    parser.add_argument("--only", action="append", help="run only this suite (repeatable)")
    parser.add_argument("--list", action="store_true", help="list the suites and exit")
    parser.add_argument("--install-toolchains", action="store_true",
                        help="install what the suites need (apt packages, Rust toolchains) and exit")
    args = parser.parse_args(argv)

    manifest = load_manifest(args.manifest)
    suites = [s for s in manifest["suites"] if not args.only or s["name"] in args.only]
    if args.list:
        for suite in suites:
            print(f"{suite['name']}: {' '.join(suite['argv'])}")
        return 0
    if args.install_toolchains:
        install_toolchains(manifest)
        return 0
    if args.only and len(suites) != len(set(args.only)):
        parser.error(f"unknown suite in {args.only}")
    if not args.output:
        parser.error("--output is required to run suites")
    if args.output.exists():
        shutil.rmtree(args.output)
    args.output.mkdir(parents=True)

    in_ci = bool(os.environ.get("GITHUB_ACTIONS"))
    before = tree_state(args.tree)
    results = []
    for suite in suites:
        if in_ci:
            print(f"::group::{suite['name']}")
        outcome = run_suite(suite, args.tree, args.output.resolve())
        print(outcome["log"], end="" if outcome["log"].endswith("\n") or not outcome["log"] else "\n")
        if in_ci:
            print("::endgroup::")
        print(f"{suite['name']}: {outcome['status']} in {outcome['seconds']} s")
        results.append(outcome)
    dirtied = sorted(tree_state(args.tree) - before)

    failed = [r for r in results if r["status"] != "pass"]
    lines = ["### Host test suites", "", f"{len(results) - len(failed)} of {len(results)} passed "
             f"in {sum(r['seconds'] for r in results):.0f} s.", "", "| Suite | Result | Seconds |", "|---|---|---|"]
    lines += [f"| {r['name']} | {'pass' if r['status'] == 'pass' else '**' + r['status'] + '**'} | {r['seconds']} |"
              for r in results]
    if dirtied:
        lines += ["", "**The suites changed the tree:**", ""] + [f"- `{line}`" for line in dirtied[:40]]
    text = "\n".join(lines) + "\n"
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as out:
            out.write(text)
    (args.output / "results.json").write_text(json.dumps(
        [{k: v for k, v in r.items() if k != "log"} for r in results], indent=1) + "\n")
    for r in failed:
        print(f"::error title=Host test failed::{r['name']}: {r['status']}", file=sys.stderr)
    if dirtied:
        print(f"::error title=Host tests changed the tree::{len(dirtied)} paths, e.g. {dirtied[0]}", file=sys.stderr)
    return 1 if failed or dirtied else 0


if __name__ == "__main__":
    sys.exit(main())
