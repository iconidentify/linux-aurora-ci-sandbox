# SPDX-License-Identifier: GPL-2.0
import json

from support import TempDirTestCase, load, read_outputs

plan_module = load("plan")
plan = plan_module.plan

BASE = "a" * 40
HEAD = "b" * 40


def pull_request(action, labels=(), fork=False, label=None):
    event = {
        "action": action,
        "pull_request": {
            "number": 42,
            "labels": [{"name": name} for name in labels],
            "base": {"sha": BASE, "repo": {"full_name": "omacom/linux-aurora"}},
            "head": {"sha": HEAD, "repo": {"full_name": "someone/linux-aurora" if fork
                                           else "omacom/linux-aurora"}},
        },
    }
    if label:
        event["label"] = {"name": label}
    return event


class PullRequestTest(TempDirTestCase):
    def test_push_without_label_runs_checks_only(self):
        for action in ("opened", "synchronize", "reopened", "ready_for_review"):
            with self.subTest(action=action):
                result = plan("pull_request", pull_request(action))
                self.assertEqual(result["tier"], "light")
                self.assertEqual((result["base_sha"], result["head_sha"]), (BASE, HEAD))
                self.assertEqual(result["pr_number"], "42")

    def test_label_on_branch_of_this_repository_builds_every_push(self):
        result = plan("pull_request", pull_request("synchronize", labels=["ci:build"]))
        self.assertEqual(result["tier"], "full")
        self.assertEqual(result["fork"], "false")

    def test_label_on_fork_does_not_build_a_new_push(self):
        result = plan("pull_request", pull_request("synchronize", labels=["ci:build"], fork=True))
        self.assertEqual(result["tier"], "light")
        self.assertEqual(result["fork"], "true")
        self.assertIn("re-apply", result["reason"])

    def test_applying_the_label_builds_even_a_fork(self):
        result = plan("pull_request", pull_request("labeled", labels=["ci:build"], fork=True,
                                                   label="ci:build"))
        self.assertEqual(result["tier"], "full")

    def test_other_labels_do_not_build(self):
        result = plan("pull_request", pull_request("labeled", labels=["ci:build", "docs"], label="docs"))
        self.assertEqual(result["tier"], "light")

    def test_deleted_fork_counts_as_a_fork(self):
        event = pull_request("synchronize", labels=["ci:build"])
        event["pull_request"]["head"]["repo"] = None
        self.assertEqual(plan("pull_request", event)["tier"], "light")

    def test_custom_label(self):
        event = pull_request("labeled", label="build-approved")
        self.assertEqual(plan("pull_request", event, label="build-approved")["tier"], "full")


class OtherEventsTest(TempDirTestCase):
    def test_merge_group_builds_the_queue_commit(self):
        result = plan("merge_group", {"merge_group": {"base_sha": BASE, "head_sha": HEAD}})
        self.assertEqual(result["tier"], "full")
        self.assertEqual((result["base_sha"], result["head_sha"]), (BASE, HEAD))

    def test_push_compares_with_the_previous_tip(self):
        result = plan("push", {"ref": "refs/heads/integration/aurora", "before": BASE, "after": HEAD})
        self.assertEqual(result["tier"], "full")
        self.assertEqual((result["base_sha"], result["head_sha"]), (BASE, HEAD))

    def test_forced_or_new_branch_push_has_no_baseline(self):
        for before, forced in (("0" * 40, False), (BASE, True)):
            with self.subTest(before=before, forced=forced):
                result = plan("push", {"ref": "refs/heads/x", "before": before, "after": HEAD,
                                       "forced": forced})
                self.assertEqual(result["base_sha"], "")

    def test_schedule_and_dispatch(self):
        self.assertEqual(plan("schedule", {})["tier"], "full")
        self.assertEqual(plan("schedule", {})["force"], "false")
        dispatch = plan("workflow_dispatch", {})
        self.assertEqual((dispatch["tier"], dispatch["force"]), ("full", "true"))

    def test_unknown_event_is_an_error(self):
        with self.assertRaises(ValueError):
            plan("issue_comment", {})


class MainTest(TempDirTestCase):
    def test_writes_github_outputs(self):
        output, summary = self.github_env()
        event = self.tmp / "event.json"
        event.write_text(json.dumps(pull_request("labeled", label="ci:build")))
        self.assertEqual(plan_module.main(["--event-name", "pull_request", "--event-path", str(event)]), 0)
        values = read_outputs(output)
        self.assertEqual(values["tier"], "full")
        self.assertEqual(values["head_sha"], HEAD)
        self.assertIn("full run", summary.read_text())

    def test_malformed_event_fails_cleanly(self):
        self.github_env()
        event = self.tmp / "event.json"
        event.write_text(json.dumps({"action": "opened"}))
        self.assertEqual(plan_module.main(["--event-name", "pull_request", "--event-path", str(event)]), 1)
