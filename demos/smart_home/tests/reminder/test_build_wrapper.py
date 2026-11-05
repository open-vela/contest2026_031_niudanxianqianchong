#!/usr/bin/env python3
"""Verify source selection and link restoration without building firmware."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[4]
with tempfile.TemporaryDirectory(prefix="reminder-build-wrapper-") as temporary:
    root = Path(temporary)
    task = root / "worktrees/alarms-codex"
    (root / "nuttx").mkdir()
    (root / "build.sh").touch()
    (task / "scripts").mkdir(parents=True)
    wrapper = task / "scripts/build_smart_home_alarms.sh"
    shutil.copyfile(repo / "scripts/build_smart_home_alarms.sh", wrapper)
    original = root / "original"
    original.mkdir()
    links = [root / "packages/demos/smart_home", root / "packages/cAGENT", root / "packages/cagent_addons"]
    targets = [task / "demos/smart_home", task / "packages/cAGENT", task / "packages/cagent_addons"]
    for link, target in zip(links, targets):
        link.parent.mkdir(parents=True, exist_ok=True)
        target.mkdir(parents=True)
        link.symlink_to(original)
    fake = task / "scripts/build_p4x.sh"
    fake.write_text("""#!/usr/bin/env bash
set -euo pipefail
test "$1" = smart_home_alarms
test "$2" = -j8
for link in "$TASK_TEST_ROOT/packages/demos/smart_home" "$TASK_TEST_ROOT/packages/cAGENT" "$TASK_TEST_ROOT/packages/cagent_addons"; do
  [[ $(readlink "$link") == "$TASK_TEST_ROOT/worktrees/alarms-codex/"* ]]
done
exit "${TASK_TEST_EXIT:-0}"
""")
    fake.chmod(0o755)
    for exit_code in [0, 79]:
        env = dict(os.environ, TASK_TEST_ROOT=str(root), TASK_TEST_EXIT=str(exit_code))
        result = subprocess.run(["bash", str(wrapper), "-j8"], env=env, capture_output=True, text=True)
        assert result.returncode == exit_code, result.stderr
        assert all(link.readlink() == original for link in links)
    # An actual directory must never be replaced with a symlink.
    links[0].unlink()
    links[0].mkdir()
    result = subprocess.run(["bash", str(wrapper), "-j8"], capture_output=True)
    assert result.returncode != 0 and links[0].is_dir() and not links[0].is_symlink()
print("PASS: worktree source links, restoration after success/failure, directory protection")
