#!/usr/bin/env python3
"""Host checks of probe allocation/matrix control; no DMA or cache model."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROBE = ROOT / "app/graphics_probe/graphics_probe_main.c"
HEADER = ROOT / "chips/esp32p4/include/esp_graphics.h"

HARNESS = r'''
#include <assert.h>
#include <sys/mman.h>
#include <arch/chip/esp_graphics.h>
#include <malloc.h>
#include <sched.h>
#include <stdlib.h>
static void *host_alloc(size_t, size_t, int);
static void host_free(void *, int);
static void *host_memalign(size_t a, size_t s) { return host_alloc(a, s, 0); }
static void host_user_free(void *p) { host_free(p, 0); }
static int host_cpu(void) { return 0; }
static int host_pin(pid_t p, size_t n, const cpu_set_t *c)
{
  (void)p;
  assert(n == sizeof(*c) && CPU_ISSET(0, c) && CPU_COUNT(c) == 1);
  return 0;
}
#define main probe_main
#define memalign host_memalign
#define free host_user_free
#define sched_getcpu host_cpu
#define sched_setaffinity host_pin
#include "PROBE_SOURCE"
#undef main
#undef memalign
#undef free
#undef sched_getcpu
#undef sched_setaffinity

static void *g_slots[2][12];
static unsigned int g_calls;
static unsigned int g_live;
static unsigned int g_fail;
static unsigned int g_bad;
static unsigned int g_runs;
static unsigned int g_stop;
static unsigned int g_init;
static unsigned int g_seen[5][8];

static void *host_alloc(size_t alignment, size_t size, int kernel)
{
  uintptr_t base = kernel ? PROBE_SRAM_START : PROBE_PSRAM_START;
  unsigned int i;
  void *p;
  assert(alignment == 64 && size == PROBE_BYTES);
  if (++g_calls == g_fail) return NULL;
  if (g_bad) base = kernel ? PROBE_PSRAM_START : PROBE_SRAM_START;
  for (i = 0; i < 12; i++)
    {
      if (g_slots[kernel][i] != NULL) continue;
      p = mmap((void *)(base + i * 65536), size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      assert(p != MAP_FAILED);
      g_slots[kernel][i] = p;
      g_live++;
      return p;
    }
  abort();
}

static void host_free(void *p, int kernel)
{
  unsigned int i;
  if (p == NULL) return;
  for (i = 0; i < 12; i++)
    {
      if (g_slots[kernel][i] == p)
        {
          assert(munmap(p, PROBE_BYTES) == 0);
          g_slots[kernel][i] = NULL;
          g_live--;
          return;
        }
    }
  abort(); /* Wrong allocator or double free. */
}

void *kmm_memalign(size_t a, size_t s)
{
#ifdef CONFIG_MM_KERNEL_HEAP
  return host_alloc(a, s, 1);
#else
  return host_alloc(a, s, 0);
#endif
}
void kmm_free(void *p)
{
#ifdef CONFIG_MM_KERNEL_HEAP
  host_free(p, 1);
#else
  host_free(p, 0);
#endif
}
int esp_graphics_initialize(void) { g_init++; return 0; }
int esp_graphics_submit(const struct esp_graphics_job_s *j, uint32_t *t)
{
  (void)t;
  assert(j->dst.data != NULL);
  if (j->operation != ESP_GRAPHICS_FILL) assert(j->src.data != NULL);
  if (j->operation == ESP_GRAPHICS_BLEND) assert(j->foreground.data != NULL);
  return -EIO; /* No hardware execution in this harness. */
}
int esp_graphics_wait(uint32_t t, unsigned int ms)
{ (void)t; (void)ms; abort(); }
int esp_graphics_cancel(uint32_t t) { (void)t; abort(); }

static int check(struct probe_s *p, unsigned int op)
{
  unsigned int mask = 0;
  assert((p->source != NULL) == (op != 1));
  assert((p->foreground != NULL) == (op == 3));
  probe_reset(p); /* Also exercise absent source/foreground handling. */
  if (p->source && probe_memory_region(p->source) == PROBE_PSRAM) mask |= 4;
  if (p->foreground && probe_memory_region(p->foreground) == PROBE_PSRAM)
    mask |= 2;
  if (probe_memory_region(p->destination) == PROBE_PSRAM) mask |= 1;
  g_seen[op][mask]++;
  return ++g_runs == g_stop ? -EIO : 0;
}
static int copy(struct probe_s *p) { return check(p, 0); }
static int fill(struct probe_s *p) { return check(p, 1); }
static int srm(struct probe_s *p) { return check(p, 2); }
static int blend(struct probe_s *p) { return check(p, 3); }
static int convert(struct probe_s *p) { return check(p, 4); }
static const struct probe_test_s tests[] =
{
  {"copy", copy, true, false}, {"fill", fill, false, false},
  {"srm", srm, true, false}, {"blend", blend, true, true},
  {"convert", convert, true, false}
};
int main(int argc, char **argv)
{
  unsigned char *p = NULL;
  struct probe_memory_s m = {PROBE_SRAM, PROBE_SRAM, PROBE_SRAM};
  unsigned int op;
  unsigned int mask;
  unsigned int fail;
  int ret;
  if (strcmp(argv[1], "matrix-check") == 0)
    {
      assert(probe_matrix(tests, 5, 2) == 0 && g_runs == 44 && g_live == 0);
      for (op = 0; op < 5; op++)
        for (mask = 0; mask < 8; mask++)
          assert(g_seen[op][mask] ==
                 ((op == 3 || ((mask & 2) == 0 && (op != 1 || mask < 2)))
                  ? 2u : 0u));
    }
  else if (strcmp(argv[1], "fail-check") == 0)
    {
      for (fail = 1; fail <= 3; fail++)
        {
          g_calls = 0; g_fail = fail;
          assert(probe_case(tests, 5, "blend", 1, &m) == -ENOMEM);
          assert(g_live == 0 && g_runs == 0);
        }
      g_fail = 0; g_stop = 3;
      assert(probe_matrix(tests, 5, 2) == -EIO && g_runs == 3 && g_live == 0);
    }
  else if (strcmp(argv[1], "region-check") == 0)
    {
      assert(probe_memory_region((void *)(PROBE_SRAM_END - PROBE_BYTES))
             == PROBE_SRAM);
      assert(probe_memory_region((void *)(PROBE_SRAM_END - PROBE_BYTES + 1))
             == PROBE_AUTO);
      assert(probe_memory_region((void *)(PROBE_PSRAM_END - PROBE_BYTES))
             == PROBE_PSRAM);
      assert(probe_memory_region((void *)(PROBE_PSRAM_END - PROBE_BYTES + 1))
             == PROBE_AUTO);
      g_bad = 1;
      assert(probe_buffer_alloc(&p, PROBE_SRAM, "src") == -EFAULT);
      assert(p == NULL && g_live == 0);
      assert(probe_buffer_alloc(&p, PROBE_PSRAM, "dst") == -EFAULT);
      assert(p == NULL && g_live == 0);
    }
  else
    {
      ret = probe_main(argc - 1, argv + 1);
      assert(g_live == 0);
      printf("host: result=%d allocations=%u init=%u\n", ret, g_calls, g_init);
    }
  return 0;
}
'''


class GraphicsMemory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="p4-graphics-memory-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binaries = {}
        for name, defines in {
            "smart": ["CONFIG_ESP32P4_PPA", "CONFIG_SMP",
                      "CONFIG_ESPRESSIF_SPIRAM", "CONFIG_MM_KERNEL_HEAP",
                      "CONFIG_ESPRESSIF_SPIRAM_USER_HEAP"],
            "ppa": ["CONFIG_ESP32P4_PPA"],
            "dma2d": [],
            "mixed": ["CONFIG_ESP32P4_PPA", "CONFIG_ESPRESSIF_SPIRAM",
                      "CONFIG_ESPRESSIF_SPIRAM_USER_HEAP"],
        }.items():
            path = Path(cls.tmp.name) / name
            (path / "nuttx").mkdir(parents=True)
            (path / "arch/chip").mkdir(parents=True)
            (path / "nuttx/config.h").write_text(
                "".join(f"#define {d} 1\n" for d in defines))
            (path / "nuttx/compiler.h").write_text("#define FAR\n")
            (path / "nuttx/kmalloc.h").write_text(
                "#include <stddef.h>\n"
                "void *kmm_memalign(size_t, size_t);\n"
                "void kmm_free(void *);\n")
            (path / "arch/chip/esp_graphics.h").symlink_to(HEADER)
            source = path / "memory.c"
            source.write_text(HARNESS.replace("PROBE_SOURCE", str(PROBE)))
            binary = path / "memory"
            subprocess.run(
                ["cc", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-fsanitize=undefined", "-I", str(path),
                 str(source), "-o", str(binary)], check=True, capture_output=True)
            cls.binaries[name] = binary

    def run_probe(self, *args, config="smart"):
        result = subprocess.run([str(self.binaries[config]), *args],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_matrix_covers_all_combinations_and_rounds(self):
        self.assertIn("PASS (22 cases, 2 rounds/case)",
                      self.run_probe("matrix-check"))

    def test_partial_allocation_and_operation_failure_release_buffers(self):
        self.run_probe("fail-check")

    def test_whole_buffer_region_checks_and_no_fallback(self):
        self.run_probe("region-check")

    def test_invalid_arguments_do_not_initialize_or_allocate(self):
        for args in [("wat",), ("all", "0"), ("all", "1001"),
                     ("all", "-1"), ("all", "1", "sram"),
                     ("copy", "1", "sram", "typo"),
                     ("matrix", "1", "sram", "psram")]:
            with self.subTest(args=args):
                self.assertIn("result=1 allocations=0 init=0",
                              self.run_probe("graphics_probe", *args))

    def test_unsupported_psram_matrix_never_runs_subset(self):
        for config in ("ppa", "dma2d", "mixed"):
            output = self.run_probe("graphics_probe", "matrix", config=config)
            self.assertIn("matrix: unavailable", output)
            self.assertIn("allocations=0 init=0", output)
            self.assertNotIn("matrix: PASS", output)

    def test_real_command_paths_allocate_only_used_inputs(self):
        for args, expected in [(("copy", "1", "sram", "psram"), 2),
                               (("fill", "1", "sram", "psram"), 1),
                               (("blend", "1", "sram", "psram", "psram"), 3),
                               (("all", "1", "sram", "psram"), 3)]:
            with self.subTest(args=args):
                output = self.run_probe("graphics_probe", *args)
                self.assertIn(f"result=1 allocations={expected} init=1", output)
        for config in ("ppa", "dma2d"):
            output = self.run_probe("graphics_probe", "copy", config=config)
            self.assertIn("allocations=2 init=1", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
