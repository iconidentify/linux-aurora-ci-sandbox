# SPDX-License-Identifier: GPL-2.0
from support import TempDirTestCase, commit_file, git, load

lookup = load("lookup-build")


class FakeApi:
    def __init__(self, artifacts=None):
        self.artifacts, self.asked = artifacts or {}, []

    def newest_artifact(self, name):
        self.asked.append(name)
        return self.artifacts.get(name, "")


class LookupTest(TempDirTestCase):
    def setUp(self):
        super().setUp()
        self.kernel = self.init_repo("kernel")
        self.base = commit_file(self.kernel, "Makefile", "VERSION = 7\n", "kernel")
        commit_file(self.kernel, "tools/lib/x.c", "int x;\n", "a tool")
        self.recipe = self.init_repo("recipe")
        for part in lookup.RECIPE_PARTS:
            path = part if part.startswith(("bin/", "helpers/")) else f"{part}/file"
            commit_file(self.recipe, path, part, f"add {part}")
        commit_file(self.recipe, "bin/omarchy-release", "x", "release tooling")
        commit_file(self.recipe, "pkgbuilds/other/PKGBUILD", "x", "another package")

    def key(self):
        tree = lookup.kernel_id(self.kernel, "HEAD")
        return lookup.build_key(tree, {p: git(self.recipe, "rev-parse", f"HEAD:{p}") for p in lookup.RECIPE_PARTS})

    def test_finds_earlier_build_verification_and_baseline(self):
        key = self.key()
        base_id = lookup.kernel_id(self.kernel, self.base)
        api = FakeApi({f"kernel-package-{key}": "11", f"kernel-verified-{key}": "12",
                       f"kernel-metadata-{base_id}": "13"})
        result = lookup.lookup(api, self.kernel, self.recipe, self.base, force=False)
        self.assertEqual((result["key"], result["package_run"], result["verified_run"]), (key, "11", "12"))
        self.assertEqual((result["base_tree"], result["baseline_run"]), (base_id, "13"))
        self.assertEqual(result["recipe_commit"], git(self.recipe, "rev-parse", "HEAD"))

    def test_force_ignores_earlier_results(self):
        api = FakeApi({f"kernel-package-{self.key()}": "11"})
        result = lookup.lookup(api, self.kernel, self.recipe, "", force=True)
        self.assertEqual((result["package_run"], result["verified_run"], result["baseline_run"]), ("", "", ""))
        self.assertEqual(api.asked, [])

    def test_other_packages_and_tooling_do_not_change_the_key(self):
        before = self.key()
        commit_file(self.recipe, "pkgbuilds/other/PKGBUILD", "y", "bump another package")
        commit_file(self.recipe, "bin/omarchy-release", "y", "release tooling")
        commit_file(self.recipe, "helpers/upstream-watch.py", "y", "an unrelated helper")
        self.assertEqual(self.key(), before)
        commit_file(self.recipe, "helpers/docker-helpers.sh", "changed", "change the builder")
        self.assertNotEqual(self.key(), before)
        before = self.key()
        commit_file(self.recipe, "pkgbuilds/linux-aurora/file", "changed", "change the kernel recipe")
        self.assertNotEqual(self.key(), before)

    def test_kernel_tree_changes_the_key(self):
        for path in ("README", "tools/lib/y.c", "tools/aurora-sep/z.py"):
            with self.subTest(path=path):
                before = self.key()
                commit_file(self.kernel, path, "x\n", "change")
                self.assertNotEqual(self.key(), before)

    def test_ci_changes_do_not_change_the_key(self):
        before = self.key()
        commit_file(self.kernel, ".github/workflows/kernel-ci.yml", "on: push\n", "workflow")
        commit_file(self.kernel, "tools/aurora-ci/plan.py", "print()\n", "ci tool")
        self.assertEqual(self.key(), before)


class ApiTest(TempDirTestCase):
    def test_newest_unexpired_artifact(self):
        api = lookup.Api("https://api.example", "o/r", "token")
        api.get = lambda path, **query: {"artifacts": [
            {"name": query["name"], "expired": True, "created_at": "2026-10-20", "workflow_run": {"id": 1}},
            {"name": query["name"], "expired": False, "created_at": "2026-10-18", "workflow_run": {"id": 2}},
            {"name": query["name"], "expired": False, "created_at": "2026-10-19", "workflow_run": {"id": 3}},
            {"name": "other", "expired": False, "created_at": "2026-10-21", "workflow_run": {"id": 4}},
        ]}
        self.assertEqual(api.newest_artifact("kernel-package-x"), "3")
        api.get = lambda path, **query: {"artifacts": []}
        self.assertEqual(api.newest_artifact("kernel-package-x"), "")
