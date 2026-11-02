#!/usr/bin/env python3
"""Host checks of the production CPU/IRQ guard; no DMA hardware model."""

import ctypes as C
import errno
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CONTEXT = ROOT / "chips/esp32p4/common/espressif"


class GraphicsContext(unittest.TestCase):
    @classmethod
    def build(cls, name, smp, enabled=True):
        path = Path(cls.tmp.name) / name
        (path / "nuttx").mkdir(parents=True)
        config = "#define OK 0\n"
        if smp:
            config += "#define CONFIG_SMP 1\n"
            if enabled:
                config += "#define CONFIG_ESP32P4_SMP_GRAPHICS_CPU0 1\n"
        (path / "nuttx/config.h").write_text(config)
        (path / "nuttx/arch.h").write_text(
            "#include <stdbool.h>\n"
            "bool up_interrupt_context(void);\n"
            "unsigned int up_cpu_index(void);\n"
        )
        (path / "nuttx/sched.h").write_text(
            "#include <stddef.h>\n"
            "typedef unsigned int cpu_set_t;\n"
            "#define CPU_ZERO(p) (*(p) = 0)\n"
            "#define CPU_SET(n,p) (*(p) |= 1u << (n))\n"
            "#define CPU_EQUAL(a,b) (*(a) == *(b))\n"
            "int nxsched_get_affinity(int, size_t, cpu_set_t *);\n"
        )
        (path / "context.c").write_text('''
#include "esp_graphics_context.h"
static int g_isr;
static unsigned int g_cpu;
static cpu_set_t g_mask;
static int g_error;
bool up_interrupt_context(void) { return g_isr != 0; }
unsigned int up_cpu_index(void) { return g_cpu; }
int nxsched_get_affinity(int pid, size_t bytes, cpu_set_t *mask)
{
  (void)pid;
  (void)bytes;
  *mask = g_mask;
  return g_error;
}
int check(int isr, unsigned int cpu, unsigned int mask, int error)
{
  g_isr = isr;
  g_cpu = cpu;
  g_mask = mask;
  g_error = error;
  return esp_graphics_check_context();
}
''')
        result = subprocess.run(
            ["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=undefined", "-I", str(path), "-I", str(CONTEXT),
             str(path / "context.c"), "-o", str(path / "context.so")],
            capture_output=True, text=True,
        )
        return path, result

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="p4-graphics-context-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.checks = {}
        for name, smp in [("smp", True), ("single", False)]:
            path, result = cls.build(name, smp)
            if result.returncode:
                raise RuntimeError(result.stderr)
            library = C.CDLL(str(path / "context.so"))
            check = library.check
            check.argtypes = [C.c_int, C.c_uint, C.c_uint, C.c_int]
            check.restype = C.c_int
            cls.checks[name] = check

    def test_cpu0_and_affinity_must_both_match(self):
        check = self.checks["smp"]
        self.assertEqual(check(0, 0, 1, 0), 0)
        for cpu, mask in [(0, 3), (1, 2), (1, 1), (0, 0)]:
            with self.subTest(cpu=cpu, mask=mask):
                self.assertEqual(check(0, cpu, mask, 0), -errno.EXDEV)

    def test_isr_rejected_before_scheduler_query(self):
        for check in self.checks.values():
            self.assertEqual(check(1, 0, 1, -errno.ESRCH), -errno.EPERM)

    def test_affinity_error_preserved(self):
        self.assertEqual(self.checks["smp"](0, 0, 1, -errno.ESRCH),
                         -errno.ESRCH)

    def test_single_core_needs_no_affinity_query(self):
        self.assertEqual(self.checks["single"](0, 0, 0, -errno.ESRCH), 0)

    def test_smp_requires_explicit_opt_in(self):
        _, result = self.build("missing-opt-in", True, enabled=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SMP graphics requires", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
