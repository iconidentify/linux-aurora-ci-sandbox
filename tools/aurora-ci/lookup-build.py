#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Name a kernel build by its inputs and find earlier results for it.

A build is identified by what goes into it: the kernel tree, and the trees of
the omarchy-pkgs parts that build it (pkgbuilds/linux-aurora, bin, build,
helpers). The same key means the same package, so CI never builds or tests
one twice:

  kernel-package-<key>     the packages and their metadata (reused instead of
                           building again)
  kernel-verified-<key>    written when every full-run job passed; a later
                           run with the same key skips KUnit and the boot test
  kernel-metadata-<tree>   package metadata for a kernel tree, the baseline
                           that a later pull request is compared with

This script computes the key, pins the omarchy-pkgs commit it was computed
from, and looks those artifacts up through the Actions API.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path

RECIPE_PARTS = ("pkgbuilds/linux-aurora", "bin", "build", "helpers")


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, text=True,
                          stdout=subprocess.PIPE).stdout.strip()


def build_key(kernel_tree, recipe_trees):
    text = f"kernel {kernel_tree}\n" + "".join(f"{part} {recipe_trees[part]}\n" for part in RECIPE_PARTS)
    return hashlib.sha256(text.encode()).hexdigest()[:32]


class Api:
    def __init__(self, url, repository, token):
        self.url, self.repository, self.token = url.rstrip("/"), repository, token

    def get(self, path, **query):
        address = f"{self.url}/repos/{self.repository}/{path}"
        if query:
            address += "?" + urllib.parse.urlencode(query)
        request = urllib.request.Request(address, headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self.token}",
            "X-GitHub-Api-Version": "2022-11-28",
        })
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.load(response)

    def newest_artifact(self, name):
        """Run ID of the newest unexpired artifact with this exact name, or ''."""
        data = self.get("actions/artifacts", name=name, per_page=20)
        live = [a for a in data.get("artifacts", []) if not a.get("expired") and a.get("name") == name]
        live.sort(key=lambda a: a.get("created_at", ""), reverse=True)
        return str(live[0]["workflow_run"]["id"]) if live else ""

    def commit_tree(self, sha):
        return self.get(f"git/commits/{sha}")["tree"]["sha"]


def lookup(api, kernel, recipe_repo, base_sha, force):
    kernel_tree = git(kernel, "rev-parse", "HEAD^{tree}")
    recipe_commit = git(recipe_repo, "rev-parse", "HEAD")
    recipe_trees = {part: git(recipe_repo, "rev-parse", f"HEAD:{part}") for part in RECIPE_PARTS}
    key = build_key(kernel_tree, recipe_trees)
    out = {"key": key, "kernel_tree": kernel_tree, "recipe_commit": recipe_commit,
           "package_run": "", "verified_run": "", "base_tree": "", "baseline_run": ""}
    if not force:
        out["package_run"] = api.newest_artifact(f"kernel-package-{key}")
        out["verified_run"] = api.newest_artifact(f"kernel-verified-{key}")
    if base_sha:
        out["base_tree"] = api.commit_tree(base_sha)
        out["baseline_run"] = api.newest_artifact(f"kernel-metadata-{out['base_tree']}")
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--kernel", type=Path, required=True, help="checkout of the commit to build")
    parser.add_argument("--recipe-repo", type=Path, required=True, help="omarchy-pkgs checkout")
    parser.add_argument("--base-sha", default="", help="commit whose build is the comparison baseline")
    parser.add_argument("--force", default="false", help="'true' ignores earlier builds and results")
    args = parser.parse_args(argv)

    api = Api(os.environ.get("GITHUB_API_URL", "https://api.github.com"),
              os.environ["GITHUB_REPOSITORY"], os.environ["GITHUB_TOKEN"])
    try:
        result = lookup(api, args.kernel, args.recipe_repo, args.base_sha, args.force == "true")
    except (subprocess.CalledProcessError, OSError, KeyError, ValueError) as error:
        print(f"::error title=Build lookup::{error}", file=sys.stderr)
        return 1

    print(json.dumps(result, indent=1))
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as out:
            for name, value in result.items():
                out.write(f"{name}={value}\n")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        reuse = []
        if result["package_run"]:
            reuse.append(f"packages from run {result['package_run']}")
        if result["verified_run"]:
            reuse.append(f"verified by run {result['verified_run']}")
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as out:
            out.write(f"Build key `{result['key']}` (kernel tree `{result['kernel_tree'][:12]}`, "
                      f"omarchy-pkgs `{result['recipe_commit'][:12]}`)"
                      + (f"; reusing {', '.join(reuse)}" if reuse else "") + ".\n\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
