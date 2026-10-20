.. SPDX-License-Identifier: GPL-2.0

=========
Kernel CI
=========

The GitHub Actions workflows in ``.github/workflows/kernel-ci*.yml`` build,
test and boot this kernel for every change to ``integration/aurora``. The
tools they run live here, so every step can also be run by hand.

What runs when
==============

Builds cost money and time, so pull requests get the free checks on every
push and the kernel is built once, in the merge queue, on exactly the tree
that will land.

======================  ===========================================  ==========
Event                   Jobs                                         Runner
======================  ===========================================  ==========
pull request push       plan, commits, host tests, CI self-test      GitHub
``ci:build`` label      all of the above, plus package, KUnit, boot  see below
merge queue             everything                                   see below
push to the branch      package (reused from the queue), attest      GitHub
nightly                 package, KUnit, boot, only if omarchy-pkgs   see below
                        changed how the kernel is packaged
manual run              everything, never reusing an earlier build   see below
======================  ===========================================  ==========

``result`` is the one check branch protection and the merge queue require.
It knows which jobs each kind of run must pass (``result.py``), so a skipped
build never counts as a pass when a build was due.

The jobs
========

plan (``plan.py``, ``lookup-build.py``)
  Decides between a light and a full run from the event, then names the
  build by its inputs: the kernel tree and the omarchy-pkgs trees that build
  it (``pkgbuilds/linux-aurora``, ``bin``, ``build``, ``helpers``). Results
  are stored as artifacts under that key, so the same inputs are never built
  or tested twice: the push after a merge reuses the queue's packages, and a
  nightly run with nothing new does nothing.

commits (``check-commits.py``)
  Every commit needs a ``Signed-off-by`` trailer (blocking). checkpatch
  findings are reported but do not fail the job. A change under
  ``include/uapi`` gets a warning: Mesa and other userspace built against
  those headers needs a matching update before the kernel ships. Ranges of
  more than 100 commits (rebases onto a new upstream) are reported only.

host tests (``host-tests.py``, ``host-tests.json``)
  The hermetic test suites under ``tools/`` that run on any Linux machine.

CI self-test
  The unit tests in ``tests/``, actionlint and zizmor (pedantic persona, with
  online audits) for the workflows.

package (``prepare-recipe.py``, ``inspect-package.py``, ``compare-packages.py``)
  Builds the kernel with the recipe that ships it: omarchy-pkgs'
  ``pkgbuilds/linux-aurora``, run through omarchy-pkgs' own ``bin/build`` in
  its builder container. ``prepare-recipe.py`` changes three lines (the
  commit, a CI subrelease on ``pkgrel`` so a CI package never has a
  published version, and the kernel source, which is read from the checkout
  instead of cloned again) and refuses to guess if the recipe changed shape.

  ``inspect-package.py`` checks what installs rely on: a ``-ARCH`` kernel
  release, a little-endian arm64 Image with the configured page size, device
  trees, ``modules.dep``, ``pkgbase``, every module symbol resolving, and
  Rust enabled. ``compare-packages.py`` diffs the result against the target
  branch's package: configuration, device trees, modules, image size and
  contents. A device tree that stops being built fails the job unless
  ``retired-dtbs.txt`` lists it with a reason.

KUnit (``run-kunit.py``, ``aurora.kunitconfig``, ``kunit-expected.json``)
  The Apple driver KUnit suites, built into one kernel and run under QEMU.

boot (``boot-test.py``, ``boot-expected.json``)
  Boots the packaged kernel, not a test build, in a QEMU ``virt`` guest with
  a busybox initramfs, loads every module the package ships, and fails on a
  crash or warning in the log, a bad taint, a release mismatch, or a module
  that fails to load for a reason other than missing hardware. The guest has
  none of the Apple hardware, so this proves the kernel boots and every
  module initialises; driver behaviour is tested on Macs.

Who can start a build
=====================

Light runs execute a pull request's code on GitHub-hosted runners with a
read-only token, as every public repository's CI does. Full runs start only
from a maintainer action: queueing a pull request, pushing to the branch, a
manual run, or applying ``ci:build`` (which needs triage access). On a branch
of this repository the label applies to every later push; on a fork it
applies to the head it was applied to, and has to be applied again after a
new push.

On a ``pull_request`` event GitHub runs the workflow files from the pull
request itself, so these rules decide cost, not security: a fork could edit
them. What keeps untrusted code off paid runners is GitHub's setting to
require approval before running workflows from outside collaborators, and,
for a runner like Namespace's, a runner group limited to this repository.

Runners
=======

Every job runs on GitHub-hosted runners, which cost nothing for a public
repository. ``package`` and ``KUnit`` use the repository variable
``KERNEL_CI_RUNNER`` when it is set: a JSON ``runs-on`` value such as
``["namespace-profile-arm64-linux-apple-12x32"]``, the machine omarchy-pkgs
builds kernels on. Without it they use ``ubuntu-24.04-arm``.

Repository setup
================

These need an administrator:

- a ``ci:build`` label;
- branch protection for ``integration/aurora`` requiring ``ci / result``;
- the merge queue, so the tree that merges is the tree that was tested;
- workflow approval for outside collaborators (Settings, Actions, General);
- optionally ``KERNEL_CI_RUNNER`` for faster builds.

Running the tools locally
=========================

All tools are Python 3.10+ standard library scripts; run them with
``python3 -I`` from the top of the tree. ``python3 -I -m unittest discover -s
tools/aurora-ci/tests`` runs the unit tests.

Build a package the way CI does (needs Docker and an omarchy-pkgs checkout)::

  python3 -I tools/aurora-ci/prepare-recipe.py \
      --recipe ../omarchy-pkgs/pkgbuilds/linux-aurora --kernel . \
      --commit "$(git rev-parse HEAD)" --revision 1
  (cd ../omarchy-pkgs && bin/build --mirror edge --arch aarch64 --package linux-aurora)

Check and compare packages (any directory holding ``linux-aurora`` and
``linux-aurora-headers`` ``.pkg.tar.zst`` files)::

  python3 -I tools/aurora-ci/inspect-package.py --packages new/ --output new.json --unpacked root/
  python3 -I tools/aurora-ci/compare-packages.py --base old.json --head new.json \
      --retired tools/aurora-ci/retired-dtbs.txt

Boot it (needs ``qemu-system-aarch64`` and a static aarch64 busybox)::

  python3 -I tools/aurora-ci/boot-test.py --root root/ --busybox /bin/busybox \
      --expected tools/aurora-ci/boot-expected.json --out boot/

Check commits::

  python3 -I tools/aurora-ci/check-commits.py --base origin/integration/aurora --head HEAD

Verifying a package
===================

Packages built for a commit on ``integration/aurora`` carry a build
provenance attestation::

  gh attestation verify linux-aurora-<version>-aarch64.pkg.tar.zst --repo omacom/linux-aurora
