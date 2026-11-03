/****************************************************************************
 * app/graphics_probe/graphics_probe_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/kmalloc.h>
#include <arch/chip/esp_graphics.h>
#include <errno.h>
#include <malloc.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PROBE_ROWS 192
#define PROBE_BYTES (256 * PROBE_ROWS)
#define PROBE_TIMEOUT 1000
#define PROBE_MAX_ROUNDS 1000

/* ESP32-P4 rev3.x address windows from HAL soc/esp32p4/include/soc/soc.h. */

#define PROBE_SRAM_START  0x4ff00000u
#define PROBE_SRAM_END    0x4ffc0000u
#define PROBE_PSRAM_START 0x48000000u
#define PROBE_PSRAM_END   0x4c000000u

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum probe_memory_e
{
  PROBE_AUTO,
  PROBE_SRAM,
  PROBE_PSRAM
};

struct probe_memory_s
{
  enum probe_memory_e source;
  enum probe_memory_e foreground;
  enum probe_memory_e destination;
};

struct probe_s
{
  struct esp_graphics_job_s job;
  unsigned char *source;
  unsigned char *foreground;
  unsigned char *destination;
};

struct probe_test_s
{
  const char *name;
  int (*run)(struct probe_s *probe);
  bool source;
  bool foreground;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void probe_reset(struct probe_s *probe)
{
  struct esp_graphics_job_s *job = &probe->job;

  memset(job, 0, sizeof(*job));
  if (probe->source != NULL)
    {
      memset(probe->source, 0, PROBE_BYTES);
    }

  if (probe->foreground != NULL)
    {
      memset(probe->foreground, 0, PROBE_BYTES);
    }

  memset(probe->destination, 0x5a, PROBE_BYTES);
  job->src.data = probe->source;
  job->src.size = PROBE_BYTES;
  job->src.stride = 128;
  job->src.height = PROBE_ROWS;
  job->src.format = ESP_GRAPHICS_RGB565;
  job->foreground = job->src;
  job->foreground.data = probe->foreground;
  job->dst = job->src;
  job->dst.data = probe->destination;
  job->dst.stride = 192;
  job->width = 32;
  job->height = 32;
  job->src_x = 4;
  job->src_y = 2;
  job->dst_x = 8;
  job->dst_y = 3;
  job->scale_x_q4 = 16;
  job->scale_y_q4 = 16;
  job->bg_alpha = 255;
  job->fg_alpha = 255;
}

static int probe_run(struct probe_s *probe)
{
  uint32_t ticket;
  int ret = esp_graphics_submit(&probe->job, &ticket);

  if (ret == 0)
    {
      ret = esp_graphics_wait(ticket, PROBE_TIMEOUT);
    }

  if (ret < 0)
    {
      printf("graphics: operation %d failed: %d\n",
             probe->job.operation, ret);
    }

  return ret;
}

#ifdef CONFIG_ESP32P4_PPA
static void probe_solid(struct esp_graphics_buffer_s *buffer, uint32_t color)
{
  unsigned int y;
  unsigned int x;

  for (y = 0; y < buffer->height; y++)
    {
      for (x = 0; x < buffer->stride / 2; x++)
        {
          ((uint16_t *)((uint8_t *)buffer->data + y * buffer->stride))[x] =
            color;
        }
    }
}

#endif

static int probe_check(struct probe_s *probe, unsigned int width,
                        unsigned int height, int color)
{
  const struct esp_graphics_job_s *job = &probe->job;
  unsigned int y;
  unsigned int x;
  uint16_t actual;
  uint16_t expected;

  /* Check the full allocation, including row padding and trailing guards. */

  for (y = 0; y < PROBE_BYTES / job->dst.stride; y++)
    {
      for (x = 0; x < job->dst.stride / 2; x++)
        {
          expected = x >= job->dst_x && x < job->dst_x + width &&
                     y >= job->dst_y && y < job->dst_y + height ?
                     color : 0x5a5a;
          if (color < 0 && x >= job->dst_x && x < job->dst_x + width &&
              y >= job->dst_y && y < job->dst_y + height)
            {
              continue;
            }

          actual = ((uint16_t *)(probe->destination +
                                 y * job->dst.stride))[x];
          if (actual != expected)
            {
              printf("pixel %u,%u: %04x expected %04x\n",
                     x, y, actual, expected);
              return -EIO;
            }
        }
    }

  return 0;
}

static int probe_reject(struct probe_s *probe, int expected)
{
  uint32_t ticket;
  int ret = esp_graphics_submit(&probe->job, &ticket);

  if (ret == 0)
    {
      esp_graphics_cancel(ticket);
    }

  return ret == expected ? 0 : -EIO;
}

static int probe_copy(struct probe_s *probe)
{
  struct esp_graphics_job_s *job = &probe->job;
  uint32_t ticket;
  uint32_t other;
  unsigned int y;
  unsigned int x;
  uint16_t actual;
  uint16_t expected;
  int ret;

  probe_reset(probe);
  job->height = 132; /* Cross the 128-row descriptor boundary. */
  for (y = 0; y < PROBE_ROWS; y++)
    {
      for (x = 0; x < 64; x++)
        {
          ((uint16_t *)(probe->source + y * 128))[x] = y * 67 + x;
        }
    }

  ret = esp_graphics_submit(job, &ticket);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_graphics_submit(job, &other);
  if (ret != -EBUSY)
    {
      esp_graphics_cancel(ret == 0 ? other : ticket);
      return -EIO;
    }

  ret = esp_graphics_wait(ticket + 1, PROBE_TIMEOUT);
  if (ret != -ENOENT)
    {
      esp_graphics_cancel(ticket);
      return -EIO;
    }

  ret = esp_graphics_wait(ticket, PROBE_TIMEOUT);
  if (ret < 0)
    {
      return ret;
    }

  for (y = 0; y < PROBE_BYTES / 192; y++)
    {
      for (x = 0; x < 96; x++)
        {
          expected = 0x5a5a;
          if (x >= job->dst_x && x < job->dst_x + job->width &&
              y >= job->dst_y && y < job->dst_y + job->height)
            {
              expected = (y - job->dst_y + job->src_y) * 67 +
                         x - job->dst_x + job->src_x;
            }

          actual = ((uint16_t *)(probe->destination + y * 192))[x];
          if (actual != expected)
            {
              printf("copy mismatch at %u,%u\n", x, y);
              return -EIO;
            }
        }
    }

  job->width = 0;
  if (probe_reject(probe, -EINVAL) < 0)
    {
      return -EIO;
    }

  job->width = 32;
  job->dst = job->src;
  if (probe_reject(probe, -EINVAL) < 0)
    {
      return -EIO;
    }

  probe_reset(probe);
  ret = esp_graphics_submit(job, &ticket);
  if (ret == 0 && esp_graphics_cancel(ticket) != -ECANCELED)
    {
      return -EIO;
    }

  if (ret < 0)
    {
      return ret;
    }

  ret = probe_run(probe);
  if (ret == 0)
    {
      ret = probe_check(probe, 32, 32, 0);
    }

  return ret;
}

#ifdef CONFIG_ESP32P4_PPA
static int probe_fill(struct probe_s *probe)
{
  int ret;

  probe_reset(probe);
  probe->job.operation = ESP_GRAPHICS_FILL;
  probe->job.color = 0xffff0000;
  ret = probe_run(probe);
  return ret < 0 ? ret : probe_check(probe, 32, 32, 0xf800);
}

static int probe_srm(struct probe_s *probe)
{
  struct esp_graphics_job_s *job = &probe->job;
  unsigned int angle;
  unsigned int mirror;
  unsigned int x;
  unsigned int y;
  unsigned int sx;
  unsigned int sy;
  unsigned int temporary;
  uint16_t expected;
  uint16_t actual;
  int ret;

  for (angle = 0; angle < 360; angle += 90)
    {
      for (mirror = 0; mirror < (angle == 0 ? 4 : 1); mirror++)
        {
          probe_reset(probe);
          job->operation = ESP_GRAPHICS_SRM;
          job->rotation = angle;
          job->mirror_x = (mirror & 1) != 0;
          job->mirror_y = (mirror & 2) != 0;
          for (y = 0; y < 32; y++)
            {
              for (x = 0; x < 32; x++)
                {
                  ((uint16_t *)(probe->source +
                    (y + job->src_y) * 128))[x + job->src_x] =
                      ((x & 31) << 11) | ((y & 31) << 6);
                }
            }

          ret = probe_run(probe);
          if (ret < 0)
            {
              return ret;
            }

          /* Check rotations and mirrors independently. */

          for (y = 0; y < 32; y++)
            {
              for (x = 0; x < 32; x++)
                {
                  sx = x;
                  sy = y;
                  if (angle == 90 || angle == 270)
                    {
                      temporary = sx;
                      sx = angle == 90 ? 31 - sy : sy;
                      sy = angle == 90 ? temporary : 31 - temporary;
                    }
                  else if (angle == 180)
                    {
                      sx = 31 - sx;
                      sy = 31 - sy;
                    }

                  sx = job->mirror_x ? 31 - sx : sx;
                  sy = job->mirror_y ? 31 - sy : sy;
                  expected = (sx << 11) | (sy << 6);
                  actual = ((uint16_t *)(probe->destination +
                    (y + job->dst_y) * 192))[x + job->dst_x];
                  if (actual != expected)
                    {
                      printf("SRM mismatch: rotation=%u mirror=%u x=%u "
                             "y=%u actual=%04x expected=%04x\n",
                             angle, mirror, x, y, actual, expected);
                      return -EIO;
                    }
                }
            }

          ret = probe_check(probe, 32, 32, -1);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  for (x = 8; x <= 32; x *= 2)
    {
      probe_reset(probe);
      probe_solid(&job->src, 0xf800);
      job->operation = ESP_GRAPHICS_SRM;
      job->scale_x_q4 = x;
      job->scale_y_q4 = x;
      ret = probe_run(probe);
      if (ret == 0)
        {
          ret = probe_check(probe, 32 * x / 16, 32 * x / 16, 0xf800);
        }

      if (ret < 0)
        {
          return ret;
        }
    }

  return 0;
}

static int probe_blend(struct probe_s *probe)
{
  struct esp_graphics_job_s *job = &probe->job;
  unsigned int alpha;
  int ret;

  for (alpha = 0; alpha <= 255; alpha += 255)
    {
      probe_reset(probe);
      probe_solid(&job->src, 0x001f);
      probe_solid(&job->foreground, 0xf800);
      job->operation = ESP_GRAPHICS_BLEND;
      job->fg_alpha = alpha;
      ret = probe_run(probe);
      if (ret == 0)
        {
          ret = probe_check(probe, 32, 32, alpha == 0 ? 0x001f : 0xf800);
        }

      if (ret < 0)
        {
          return ret;
        }
    }

  probe_reset(probe);
  probe_solid(&job->src, 0x001f);
  probe_solid(&job->foreground, 0xf800);
  job->operation = ESP_GRAPHICS_BLEND;
  job->fg_alpha = 128;
  ret = probe_run(probe);
  if (ret < 0)
    {
      return ret;
    }

  for (unsigned int y = 0; y < 32; y++)
    {
      for (unsigned int x = 0; x < 32; x++)
        {
          uint16_t pixel = ((uint16_t *)(probe->destination +
            (y + job->dst_y) * 192))[x + job->dst_x];
          int red = (pixel >> 11) & 31;
          int green = (pixel >> 5) & 63;
          int blue = pixel & 31;

          if (abs(red - 16) > 1 || green != 0 || abs(blue - 15) > 1)
            {
              printf("blend alpha=128 mismatch: %04x\n", pixel);
              return -EIO;
            }
        }
    }

  return probe_check(probe, 32, 32, -1);
}

static int probe_convert(struct probe_s *probe)
{
  struct esp_graphics_job_s *job = &probe->job;
  unsigned int i;
  unsigned int format;
  int ret;

  for (format = ESP_GRAPHICS_RGB888;
       format <= ESP_GRAPHICS_YUV422; format++)
    {
      probe_reset(probe);
      job->operation = ESP_GRAPHICS_SRM;
      job->src.format = format;
      job->src.stride = format == ESP_GRAPHICS_RGB888 ? 192 : 256;
      for (i = 0; i < PROBE_BYTES; )
        {
          if (format == ESP_GRAPHICS_RGB888)
            {
              probe->source[i++] = 0;
              probe->source[i++] = 0;
              probe->source[i++] = 255;
            }
          else if (format == ESP_GRAPHICS_ARGB8888)
            {
              ((uint32_t *)probe->source)[i / 4] = 0xffff0000;
              i += 4;
            }
          else
            {
              probe->source[i++] = 128;
              probe->source[i++] = 16;
            }
        }

      ret = probe_run(probe);
      if (ret == 0)
        {
          ret = probe_check(probe, 32, 32,
                              format == ESP_GRAPHICS_YUV422 ? 0 : 0xf800);
        }

      if (ret < 0)
        {
          return ret;
        }
    }

  return 0;
}
#endif

static const char *probe_memory_name(enum probe_memory_e memory)
{
  switch (memory)
    {
      case PROBE_SRAM:
        return "sram";
      case PROBE_PSRAM:
        return "psram";
      default:
        return "auto";
    }
}

static int probe_memory_parse(const char *name, enum probe_memory_e *memory)
{
  for (*memory = PROBE_AUTO; *memory <= PROBE_PSRAM; (*memory)++)
    {
      if (strcmp(name, probe_memory_name(*memory)) == 0)
        {
          return 0;
        }
    }

  return -EINVAL;
}

static enum probe_memory_e probe_memory_region(const void *buffer)
{
  uintptr_t address = (uintptr_t)buffer;

  if (address >= PROBE_SRAM_START &&
      address <= PROBE_SRAM_END - PROBE_BYTES)
    {
      return PROBE_SRAM;
    }

  if (address >= PROBE_PSRAM_START &&
      address <= PROBE_PSRAM_END - PROBE_BYTES)
    {
      return PROBE_PSRAM;
    }

  return PROBE_AUTO;
}

static bool probe_memory_supported(enum probe_memory_e memory)
{
  if (memory != PROBE_PSRAM)
    {
      return true;
    }

#if defined(CONFIG_ESPRESSIF_SPIRAM) && \
    defined(CONFIG_ESPRESSIF_SPIRAM_USER_HEAP) && \
    defined(CONFIG_MM_KERNEL_HEAP)
  return true;
#else
  return false;
#endif
}

static void probe_buffer_free(void *buffer, enum probe_memory_e memory)
{
  if (memory == PROBE_SRAM)
    {
      kmm_free(buffer);
    }
  else
    {
      free(buffer);
    }
}

static int probe_buffer_alloc(unsigned char **buffer,
                              enum probe_memory_e memory, const char *role)
{
  int ret;

  if (!probe_memory_supported(memory))
    {
      printf("graphics: %s memory=%s unavailable: need SPIRAM, "
             "SPIRAM_USER_HEAP and MM_KERNEL_HEAP\n",
             role, probe_memory_name(memory));
      return -ENOTSUP;
    }

  if (memory == PROBE_SRAM)
    {
      *buffer = kmm_memalign(ESP_GRAPHICS_ALIGNMENT, PROBE_BYTES);
    }
  else
    {
      *buffer = memalign(ESP_GRAPHICS_ALIGNMENT, PROBE_BYTES);
    }

  ret = -ENOMEM;
  if (*buffer != NULL)
    {
      ret = -EFAULT;
      if (((uintptr_t)*buffer % ESP_GRAPHICS_ALIGNMENT) == 0 &&
          (memory == PROBE_AUTO || probe_memory_region(*buffer) == memory))
        {
          return 0;
        }
    }

  printf("graphics: %s allocation memory=%s addr=%p bytes=%u failed: %d\n",
         role, probe_memory_name(memory), *buffer, PROBE_BYTES, ret);
  probe_buffer_free(*buffer, memory);
  *buffer = NULL;
  return ret;
}

static const char *probe_buffer_name(const void *buffer)
{
  return buffer == NULL ? "unused" :
         probe_memory_name(probe_memory_region(buffer));
}

static bool probe_selected(const char *command,
                           const struct probe_test_s *test)
{
  return strcmp(command, "all") == 0 || strcmp(command, test->name) == 0;
}

static int probe_case(const struct probe_test_s *tests, unsigned int count,
                      const char *command, unsigned long rounds,
                      const struct probe_memory_s *memory)
{
  struct probe_s probe;
  bool source = false;
  bool foreground = false;
  unsigned long round;
  unsigned int i;
  int ret;

  memset(&probe, 0, sizeof(probe));
  for (i = 0; i < count; i++)
    {
      if (probe_selected(command, &tests[i]))
        {
          source |= tests[i].source;
          foreground |= tests[i].foreground;
        }
    }

  printf("graphics case: %s src=%s dst=%s fg=%s\n", command,
         source ? probe_memory_name(memory->source) : "unused",
         probe_memory_name(memory->destination),
         foreground ? probe_memory_name(memory->foreground) : "unused");

  ret = probe_buffer_alloc(&probe.destination, memory->destination, "dst");
  if (ret < 0)
    {
      goto out;
    }

  if (source)
    {
      ret = probe_buffer_alloc(&probe.source, memory->source, "src");
      if (ret < 0)
        {
          goto out;
        }
    }

  if (foreground)
    {
      ret = probe_buffer_alloc(&probe.foreground, memory->foreground, "fg");
      if (ret < 0)
        {
          goto out;
        }
    }

  printf("graphics: cpu=%d rounds=%lu bytes=%u src=%p fg=%p dst=%p\n",
         sched_getcpu(), rounds, PROBE_BYTES,
         probe.source, probe.foreground, probe.destination);
  printf("graphics memory: src=%s fg=%s dst=%s\n",
         probe_buffer_name(probe.source),
         probe_buffer_name(probe.foreground),
         probe_buffer_name(probe.destination));

  for (round = 0; round < rounds; round++)
    {
      if (rounds > 1)
        {
          printf("graphics round %lu/%lu\n", round + 1, rounds);
        }

      for (i = 0; i < count; i++)
        {
          if (probe_selected(command, &tests[i]))
            {
              ret = tests[i].run(&probe);
              printf("graphics %s: %s (%d)\n", tests[i].name,
                     ret == 0 ? "PASS" : "FAIL", ret);
              if (ret < 0)
                {
                  goto out;
                }
            }
        }
    }

out:
  probe_buffer_free(probe.destination, memory->destination);
  probe_buffer_free(probe.foreground, memory->foreground);
  probe_buffer_free(probe.source, memory->source);
  return ret;
}

static int probe_matrix(const struct probe_test_s *tests, unsigned int count,
                        unsigned long rounds)
{
  struct probe_memory_s memory;
  unsigned int cases = 0;
  unsigned int source;
  unsigned int foreground;
  unsigned int destination;
  unsigned int i;
  int ret;

  for (i = 0; i < count; i++)
    {
      for (source = 0; source < (tests[i].source ? 2 : 1); source++)
        {
          for (destination = 0; destination < 2; destination++)
            {
              for (foreground = 0;
                   foreground < (tests[i].foreground ? 2 : 1); foreground++)
                {
                  memory.source = source == 0 ? PROBE_SRAM : PROBE_PSRAM;
                  memory.destination = destination == 0 ?
                                       PROBE_SRAM : PROBE_PSRAM;
                  memory.foreground = foreground == 0 ?
                                      PROBE_SRAM : PROBE_PSRAM;
                  ret = probe_case(tests, count, tests[i].name, rounds,
                                   &memory);
                  if (ret < 0)
                    {
                      printf("graphics matrix: FAIL after %u cases (%d)\n",
                             cases, ret);
                      return ret;
                    }

                  cases++;
                }
            }
        }
    }

  printf("graphics matrix: PASS (%u cases, %lu rounds/case)\n",
         cases, rounds);
  return 0;
}

static void probe_usage(void)
{
  printf("Usage: graphics_probe all | copy"
#ifdef CONFIG_ESP32P4_PPA
         " | fill | srm | blend | convert"
#endif
         " [rounds:1..1000] [src_mem dst_mem [fg_mem]]\n"
         "       graphics_probe matrix [rounds:1..1000]\n"
         "Memory: auto | sram | psram; fg_mem defaults to src_mem\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  static const struct probe_test_s tests[] =
  {
    {"copy", probe_copy, true, false},
#ifdef CONFIG_ESP32P4_PPA
    {"fill", probe_fill, false, false},
    {"srm", probe_srm, true, false},
    {"blend", probe_blend, true, true},
    {"convert", probe_convert, true, false},
#endif
  };

  struct probe_memory_s memory =
  {
    PROBE_AUTO, PROBE_AUTO, PROBE_AUTO
  };

  const unsigned int count = sizeof(tests) / sizeof(tests[0]);
  const char *command = argc > 1 ? argv[1] : "all";
  bool matrix = strcmp(command, "matrix") == 0;
  char *end;
  unsigned long rounds = 1;
  unsigned int i;
  bool matched = matrix;
  int ret;
#ifdef CONFIG_SMP
  cpu_set_t cpu0;
#endif

  if (argc > 6 || argc == 4 || (matrix && argc > 3))
    {
      probe_usage();
      return EXIT_FAILURE;
    }

  if (argc > 2)
    {
      errno = 0;
      rounds = strtoul(argv[2], &end, 10);
      if (errno != 0 || end == argv[2] || *end != '\0' ||
          rounds == 0 || rounds > PROBE_MAX_ROUNDS)
        {
          probe_usage();
          return EXIT_FAILURE;
        }
    }

  if (argc > 3)
    {
      if (probe_memory_parse(argv[3], &memory.source) < 0 ||
          probe_memory_parse(argv[4], &memory.destination) < 0 ||
          (argc == 6 &&
           probe_memory_parse(argv[5], &memory.foreground) < 0))
        {
          probe_usage();
          return EXIT_FAILURE;
        }

      if (argc == 5)
        {
          memory.foreground = memory.source;
        }
    }

  for (i = 0; i < count; i++)
    {
      matched |= probe_selected(command, &tests[i]);
    }

  if (!matched)
    {
      probe_usage();
      return EXIT_FAILURE;
    }

  if (matrix && !probe_memory_supported(PROBE_PSRAM))
    {
      printf("graphics matrix: unavailable: need SPIRAM, "
             "SPIRAM_USER_HEAP and MM_KERNEL_HEAP\n");
      return EXIT_FAILURE;
    }

#ifdef CONFIG_SMP
  CPU_ZERO(&cpu0);
  CPU_SET(0, &cpu0);
  if (sched_setaffinity(0, sizeof(cpu0), &cpu0) < 0)
    {
      perror("graphics: pin CPU0");
      return EXIT_FAILURE;
    }
#endif

  ret = esp_graphics_initialize();
  if (ret < 0)
    {
      printf("graphics: initialize failed: %d\n", ret);
      return EXIT_FAILURE;
    }

  if (matrix)
    {
      ret = probe_matrix(tests, count, rounds);
    }
  else
    {
      ret = probe_case(tests, count, command, rounds, &memory);
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
