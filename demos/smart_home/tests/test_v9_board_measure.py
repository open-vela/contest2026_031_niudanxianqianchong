#!/usr/bin/env python3
"""Exercise --build only against a disposable OpenVela tree, never hardware."""
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest

SCRIPT = Path(__file__).resolve().parents[3] / "scripts/v9_board_measure.sh"
LINKS = (
    "vendor/espressif/boards/esp32p4/esp32p4-function-ev-board",
    "packages/demos/smart_home",
    "packages/cAGENT",
    "packages/cagent_addons",
)


class BoardMeasureTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="v9-measure-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.originals = ("../alternate-board", "/missing/demo", None, "../other-addons")
        for name, original in zip(LINKS, self.originals):
            link = self.root / name
            link.parent.mkdir(parents=True, exist_ok=True)
            if original is not None:
                link.symlink_to(original)
        (self.root / "myenv/bin").mkdir(parents=True)
        (self.root / "myenv/bin/activate").write_text("")
        (self.root / "nuttx").mkdir()
        (self.root / "nuttx/nuttx.bin").write_bytes(b"stale firmware")
        build = self.root / "build.sh"
        build.write_text("""#!/usr/bin/env python3
import json, os, pathlib, sys, time
root = pathlib.Path.cwd()
names = json.loads(os.environ['TEST_LINKS'])
(root / 'config-argument.txt').write_text(sys.argv[1])
(root / 'during.json').write_text(json.dumps([os.readlink(root / p) for p in names]))
mode = os.environ.get('TEST_BUILD_MODE', 'success')
if mode == 'signal': time.sleep(0.2)
if mode == 'fail': raise SystemExit(23)
(root / 'nuttx/nuttx.bin').write_bytes(b'new firmware')
""")
        build.chmod(0o755)
        self.env = dict(os.environ, OPENVELA_ROOT=str(self.root),
                        TEST_LINKS=json.dumps(LINKS))
        self.env.pop("original", None)

    def run_build(self, *extra_args, **env):
        return subprocess.run(["bash", str(SCRIPT), "--build", *extra_args],
                              env=dict(self.env, **env), text=True,
                              capture_output=True, timeout=5)

    def assert_restored(self):
        for name, original in zip(LINKS, self.originals):
            link = self.root / name
            if original is None:
                self.assertFalse(os.path.lexists(link), name)
            else:
                self.assertTrue(link.is_symlink(), name)
                self.assertEqual(os.readlink(link), original)

    def test_success_restores_exact_originals_and_missing_links(self):
        result = self.run_build()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_restored()
        during = json.loads((self.root / "during.json").read_text())
        self.assertTrue(all(Path(target).is_absolute() for target in during))
        self.assertTrue(all(str(SCRIPT.parent.parent) in target for target in during))
        self.assertEqual((self.root / "config-argument.txt").read_text(),
                         str(SCRIPT.parent.parent / "board/esp32p4/esp32p4-function-ev-board/configs/smart_home_service_lab"))
        self.assertEqual((self.root / "nuttx/nuttx.bin").read_bytes(), b"new firmware")

    def test_image_failure_is_not_reported_as_success(self):
        # The sandbox intentionally has no mklittlefs executable. The real
        # image script must fail before staging, and the wrapper must fail too.
        result = self.run_build("--image")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("mklittlefs not found", result.stderr)
        self.assert_restored()

    def test_failed_build_rejects_stale_firmware_and_restores_links(self):
        result = self.run_build(TEST_BUILD_MODE="fail")
        self.assertEqual(result.returncode, 23, result.stderr)
        self.assertIn("拒绝使用", result.stderr)
        self.assert_restored()
        self.assertEqual((self.root / "nuttx/nuttx.bin").read_bytes(), b"stale firmware")

    def test_termination_restores_links(self):
        process = subprocess.Popen(["bash", str(SCRIPT), "--build"],
                                   env=dict(self.env, TEST_BUILD_MODE="signal"),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 3
            while not (self.root / "during.json").exists():
                self.assertIsNone(process.poll())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.005)
            process.send_signal(signal.SIGTERM)
            _, stderr = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 143, stderr)
            self.assert_restored()
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_non_link_is_rejected_before_any_changes(self):
        link = self.root / LINKS[3]
        link.unlink()
        link.mkdir()
        result = self.run_build()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("拒绝替换非软链接", result.stderr)
        self.assertEqual(os.readlink(self.root / LINKS[0]), self.originals[0])
        self.assertFalse((self.root / "during.json").exists())
        self.assertTrue(link.is_dir())

    def test_partial_switch_failure_restores_already_changed_links(self):
        wrapper_dir = self.root / "wrapper"
        wrapper_dir.mkdir()
        wrapper = wrapper_dir / "ln"
        wrapper.write_text("""#!/usr/bin/env bash
if [[ "${!#}" == */packages/demos/smart_home ]]; then exit 17; fi
exec /usr/bin/ln "$@"
""")
        wrapper.chmod(0o755)
        result = self.run_build(PATH=str(wrapper_dir) + os.pathsep + os.environ["PATH"])
        self.assertNotEqual(result.returncode, 0)
        self.assert_restored()
        self.assertFalse((self.root / "during.json").exists())


if __name__ == "__main__":
    unittest.main()
