# SPDX-License-Identifier: GPL-2.0
import json
import sys

from support import TempDirTestCase, commit_file, load

host = load("host-tests")


class RunnerTest(TempDirTestCase):
    def setUp(self):
        super().setUp()
        self.tree = self.init_repo("tree")
        commit_file(self.tree, "tools/x/pass-tests.py", "print('ok')\n", "pass")
        commit_file(self.tree, "tools/x/fail-tests.py", "import sys; sys.exit(3)\n", "fail")
        commit_file(self.tree, "tools/x/slow-tests.py", "import time; time.sleep(30)\n", "slow")
        commit_file(self.tree, "tools/x/dirty-tests.py", "open('tools/x/left-behind', 'w').write('x')\n", "dirty")
        commit_file(self.tree, "tools/x/output-tests.py",
                    "import sys, pathlib; pathlib.Path(sys.argv[1], 'out.txt').write_text('x')\n", "output")
        commit_file(self.tree, "tools/x/env-tests.py",
                    "import os, sys; sys.exit(0 if os.environ.get('RUSTUP_TOOLCHAIN') == '1.93.1' else 4)\n", "env")

    def manifest(self, *suites):
        path = self.tmp / "manifest.json"
        path.write_text(json.dumps({"suites": list(suites), "excluded": {}}))
        return path

    def suite(self, name, **extra):
        return dict({"name": name, "argv": [sys.executable, "-I", f"tools/x/{name}-tests.py"]}, **extra)

    def run_main(self, *suites):
        output = self.tmp / "out"
        status = host.main(["--manifest", str(self.manifest(*suites)), "--tree", str(self.tree),
                            "--output", str(output)])
        return status, json.loads((output / "results.json").read_text())

    def test_passing_suites(self):
        self.github_env()
        status, results = self.run_main(self.suite("pass"),
                                         self.suite("output", argv=[sys.executable, "-I", "tools/x/output-tests.py",
                                                                    "{output}"]),
                                         self.suite("env", rust="1.93.1"))
        self.assertEqual(status, 0)
        self.assertEqual([r["status"] for r in results], ["pass", "pass", "pass"])
        self.assertTrue((self.tmp / "out/output/out.txt").is_file())

    def test_failure_and_timeout(self):
        self.github_env()
        status, results = self.run_main(self.suite("fail"), self.suite("slow", timeout=1))
        self.assertEqual(status, 1)
        self.assertEqual([r["status"] for r in results], ["fail (3)", "timeout"])

    def test_a_suite_that_changes_the_tree_fails(self):
        _, summary = self.github_env()
        status, results = self.run_main(self.suite("dirty"))
        self.assertEqual(status, 1)
        self.assertEqual(results[0]["status"], "pass")
        self.assertIn("left-behind", summary.read_text())

    def test_duplicate_names_are_rejected(self):
        with self.assertRaises(ValueError):
            host.load_manifest(self.manifest(self.suite("pass"), self.suite("pass")))

    def test_only_and_unknown_suite(self):
        self.github_env()
        manifest = self.manifest(self.suite("pass"), self.suite("fail"))
        output = self.tmp / "out"
        self.assertEqual(host.main(["--manifest", str(manifest), "--tree", str(self.tree),
                                    "--output", str(output), "--only", "pass"]), 0)
        with self.assertRaises(SystemExit):
            host.main(["--manifest", str(manifest), "--tree", str(self.tree), "--output", str(output),
                       "--only", "missing"])
