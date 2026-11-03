#!/usr/bin/env python3
"""Production camera handoff/fallback tests with fake V4L2 and PPA, no DMA model."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NUTTX = next(base / "nuttx" for base in (ROOT.parent, ROOT.parent.parent)
             if (base / "nuttx/include/sys/videoio.h").exists())
SOURCE = ROOT / "demos/smart_home/src/camera/smart_home_camera_service.c"

HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <arch/chip/esp_graphics.h>
static int fake_ioctl(int, unsigned long, ...);
static int fake_poll(struct pollfd *, nfds_t, int);
#define ioctl fake_ioctl
#define poll fake_poll
#include "CAMERA_SOURCE"
#undef ioctl
#undef poll

static void *g_allocs[8];
static unsigned int g_alloc_calls;
static unsigned int g_fail_alloc;
static unsigned int g_live;
static unsigned int g_submits;
static unsigned int g_waits;
static unsigned int g_cancels;
static unsigned int g_queued;
static unsigned int g_sequence;
static int g_mode;
static bool g_streaming;
static bool g_held;
static bool g_owned;
static struct esp_graphics_job_s g_job;

void *smart_home_bulk_alloc(size_t size)
{
  unsigned int i;
  void *raw;
  if (++g_alloc_calls == g_fail_alloc) return NULL;
  raw = malloc(size);
  assert(raw != NULL);
  for (i = 0; i < 8; i++)
    if (!g_allocs[i]) { g_allocs[i] = raw; g_live++; return raw; }
  abort();
}
void smart_home_bulk_free(void *p)
{
  unsigned int i;
  if (!p) return;
  assert(!g_owned && !g_held);
  for (i = 0; i < 8; i++)
    if (g_allocs[i] == p)
      { g_allocs[i] = NULL; g_live--; free(p); return; }
  abort(); /* Must free the original allocation, never an aligned interior. */
}
int esp_graphics_initialize(void) { return g_mode == 1 ? -ENOMEM : 0; }
int esp_graphics_submit(const struct esp_graphics_job_s *job, uint32_t *ticket)
{
  g_submits++;
  assert(g_held && !g_owned);
  assert(job->operation == ESP_GRAPHICS_SRM);
  assert(job->src.data == g_camera.capture[0]);
  assert(job->src.stride == 2048 && job->src.height == 600);
  assert(job->src.size == 1228800 && job->src.format == ESP_GRAPHICS_RGB565);
  assert(job->dst.stride == 1024 && job->dst.height == 300);
  assert(job->dst.size == 307200 && job->dst.format == ESP_GRAPHICS_RGB565);
  assert(job->width == 1024 && job->height == 600);
  assert(job->scale_x_q4 == 8 && job->scale_y_q4 == 8);
  assert(job->rotation == 0 && !job->mirror_x && !job->mirror_y);
  assert((uintptr_t)job->src.data % 64 == 0);
  assert((uintptr_t)job->dst.data % 64 == 0);
  assert(job->dst.data != g_camera.preview[g_camera.published_preview]);
  if (g_mode == 2) return -EBUSY;
  g_owned = true;
  g_job = *job;
  *ticket = 42;
  return 0;
}
int esp_graphics_wait(uint32_t ticket, unsigned int timeout)
{
  unsigned int i;
  uint32_t sequence;
  uint8_t *copy = malloc(SMART_HOME_CAMERA_PREVIEW_BYTES);
  g_waits++;
  assert(ticket == 42 && timeout == 200 && g_held && g_owned);
  assert(g_camera.preview[g_camera.published_preview] != g_job.dst.data);
  if (g_camera.status.preview_sequence == 0)
    assert(smart_home_camera_copy_latest(copy, SMART_HOME_CAMERA_PREVIEW_BYTES,
                                         &sequence) == -EAGAIN);
  else
    {
      assert(smart_home_camera_copy_latest(copy, SMART_HOME_CAMERA_PREVIEW_BYTES,
                                           &sequence) == 0);
      assert(sequence == g_camera.status.preview_sequence);
      for (i = 0; i < SMART_HOME_CAMERA_PREVIEW_BYTES / 2; i++)
        assert(((uint16_t *)copy)[i] == 0x55aa);
    }
  free(copy);
  memset(g_job.dst.data, 0xcc, SMART_HOME_CAMERA_PREVIEW_BYTES);
  if (g_mode == 4) return -EINTR; /* Ownership not yet returned. */
  g_owned = false;
  if (g_mode == 3) return -ETIMEDOUT;
  for (i = 0; i < SMART_HOME_CAMERA_PREVIEW_BYTES / 2; i++)
    ((uint16_t *)g_job.dst.data)[i] = 0x55aa;
  return 0;
}
int esp_graphics_cancel(uint32_t ticket)
{
  assert(ticket == 42 && g_held);
  g_cancels++;
  if (!g_owned) return -ENOENT;
  if (g_cancels == 1) return -EINTR;
  g_owned = false;
  return -ECANCELED;
}
static int fake_poll(struct pollfd *fds, nfds_t count, int timeout)
{
  assert(count == 1 && timeout == 200 && !g_held && !g_owned);
  fds[0].revents = POLLIN;
  return 1;
}
static int fake_ioctl(int fd, unsigned long request, ...)
{
  va_list ap;
  void *arg;
  (void)fd;
  va_start(ap, request);
  arg = (void *)va_arg(ap, uintptr_t);
  va_end(ap);
  if (request == VIDIOC_QUERYCAP)
    ((struct v4l2_capability *)arg)->capabilities =
      V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
  else if (request == VIDIOC_STREAMON) g_streaming = true;
  else if (request == VIDIOC_DQBUF)
    {
      struct v4l2_buffer *b = arg;
      assert(!g_held && !g_owned);
      b->index = 0;
      b->m.userptr = (unsigned long)g_camera.capture[0];
      b->bytesused = SMART_HOME_CAMERA_SOURCE_BYTES;
      b->sequence = g_sequence += 2;
      g_held = true;
    }
  else if (request == VIDIOC_QBUF && g_streaming)
    {
      assert(g_held && !g_owned);
      assert(g_camera.status.preview_sequence == g_queued + 1);
      g_held = false;
      g_queued++;
      g_camera.stop_requested = true;
    }
  return 0;
}
int main(int argc, char **argv)
{
  struct smart_home_camera_status_s status;
  uint8_t *copy;
  uint32_t sequence;
  unsigned int fail;
  unsigned int i;
  unsigned int x;
  unsigned int y;
  bool hardware;
  assert(argc == 2);
  g_mode = atoi(argv[1]);
  if (g_mode == 5)
    {
      for (fail = 1; fail <= 5; fail++)
        {
          g_alloc_calls = 0; g_fail_alloc = fail;
          assert(smart_home_camera_configure(7) == -ENOMEM);
          smart_home_camera_free_buffers();
          assert(g_live == 0);
        }
      return 0;
    }
  assert(smart_home_camera_configure(7) == 0 && g_live == 5);
  for (i = 0; i < SMART_HOME_CAMERA_BUFFER_COUNT; i++)
    assert((uintptr_t)g_camera.capture[i] % 64 == 0);
  for (i = 0; i < 2; i++) assert((uintptr_t)g_camera.preview[i] % 64 == 0);
  for (y = 0; y < 600; y++)
    for (x = 0; x < 1024; x++)
      ((uint16_t *)g_camera.capture[0])[y * 1024 + x] = (y * 1024 + x);
  g_camera.status.state = SMART_HOME_CAMERA_RUNNING;
  smart_home_camera_accel_initialize();
  assert(smart_home_camera_capture_loop(7) == 0 && g_queued == 1);
#ifdef CONFIG_SMART_HOME_CAMERA_PPA
  hardware = g_mode == 0;
  assert(g_submits == (g_mode == 1 ? 0u : 1u));
  assert(g_waits == (g_mode == 1 || g_mode == 2 ? 0u : 1u));
  assert(g_cancels == (g_mode == 3 ? 1u : g_mode == 4 ? 2u : 0u));
#else
  hardware = false;
  assert(!g_submits && !g_waits && !g_cancels);
#endif
  smart_home_camera_get_status(&status);
  assert(status.ppa_frames == (hardware ? 1u : 0u));
  assert(status.cpu_frames == (hardware ? 0u : 1u));
#ifdef CONFIG_SMART_HOME_CAMERA_PPA
  assert(status.ppa_errors == (hardware ? 0u : 1u));
#endif
  copy = malloc(SMART_HOME_CAMERA_PREVIEW_BYTES);
  assert(smart_home_camera_copy_latest(copy, SMART_HOME_CAMERA_PREVIEW_BYTES,
                                       &sequence) == 0 && sequence == 1);
  for (y = 0; y < 300; y++)
    for (x = 0; x < 512; x++)
      assert(((uint16_t *)copy)[y * 512 + x] ==
             (hardware ? 0x55aa : (uint16_t)(y * 2 * 1024 + x * 2)));
  if (hardware)
    {
      g_camera.stop_requested = false;
      assert(smart_home_camera_capture_loop(7) == 0 && g_queued == 2);
      assert(g_camera.status.ppa_frames == 2);
    }
  free(copy);
  smart_home_camera_free_buffers();
  assert(g_live == 0);
  return 0;
}
'''


class CameraPPA(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="p4-camera-ppa-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binaries = {}
        for enabled in (True, False):
            path = Path(cls.tmp.name) / str(enabled)
            for subdir in ("nuttx/fs", "arch/chip", "sys"):
                (path / subdir).mkdir(parents=True)
            (path / "nuttx/config.h").write_text(
                "#define OK 0\n" +
                ("#define CONFIG_SMART_HOME_CAMERA_PPA 1\n" if enabled else ""))
            (path / "nuttx/compiler.h").write_text("#define FAR\n")
            (path / "nuttx/fs/ioctl.h").write_text("#define _VIDIOC(n) (0x5000+(n))\n")
            for name in ("videoio.h", "video_controls.h"):
                (path / "sys" / name).symlink_to(NUTTX / "include/sys" / name)
            (path / "arch/chip/esp_graphics.h").symlink_to(
                ROOT / "chips/esp32p4/include/esp_graphics.h")
            source = path / "camera.c"
            source.write_text(HARNESS.replace('"CAMERA_SOURCE"', '"' + str(SOURCE) + '"'))
            binary = path / "camera"
            result = subprocess.run(
                ["cc", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=undefined", "-pthread", "-I", str(path),
                 str(source), "-o", str(binary)], capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stderr)
            cls.binaries[enabled] = binary

    def run_mode(self, mode, enabled=True):
        result = subprocess.run([str(self.binaries[enabled]), str(mode)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_hardware_completion_before_publish_and_requeue(self):
        self.run_mode(0)

    def test_initialization_failure_uses_cpu(self):
        self.run_mode(1)

    def test_busy_submit_uses_cpu_without_waiting(self):
        self.run_mode(2)

    def test_timeout_overwrites_partial_output_after_release(self):
        self.run_mode(3)

    def test_wait_error_retains_frame_until_cancel_confirms_release(self):
        self.run_mode(4)

    def test_partial_buffer_allocation_frees_original_pointers(self):
        self.run_mode(5)

    def test_software_build_has_no_hardware_calls(self):
        self.run_mode(0, enabled=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
