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

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct probe_s
{
  struct esp_graphics_job_s job;
  unsigned char *source;
  unsigned char *foreground;
  unsigned char *destination;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

struct probe_test_s
{
  const char *name;
  int (*run)(struct probe_s *probe);
};

static void probe_reset(struct probe_s *probe)
{
  struct esp_graphics_job_s *job = &probe->job;

  memset(job, 0, sizeof(*job));
  memset(probe->source, 0, PROBE_BYTES);
  memset(probe->foreground, 0, PROBE_BYTES);
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

static void probe_usage(void)
{
  printf("Usage: graphics_probe all | copy"
#ifdef CONFIG_ESP32P4_PPA
         " | fill | srm | blend | convert"
#endif
         " [rounds:1..1000]\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  static const struct probe_test_s tests[] =
  {
    {"copy", probe_copy},
#ifdef CONFIG_ESP32P4_PPA
    {"fill", probe_fill},
    {"srm", probe_srm},
    {"blend", probe_blend},
    {"convert", probe_convert},
#endif
  };

  struct probe_s probe;
  const char *command = argc > 1 ? argv[1] : "all";
  char *end;
  unsigned long rounds = 1;
  unsigned long round;
  unsigned int i;
  bool matched = false;
  int ret;
#ifdef CONFIG_SMP
  cpu_set_t cpu0;
#endif

  memset(&probe, 0, sizeof(probe));
  if (argc > 3)
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

  for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    {
      matched |= strcmp(command, "all") == 0 ||
                 strcmp(command, tests[i].name) == 0;
    }

  if (!matched)
    {
      probe_usage();
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

  probe.source = memalign(ESP_GRAPHICS_ALIGNMENT, PROBE_BYTES);
  probe.foreground = memalign(ESP_GRAPHICS_ALIGNMENT, PROBE_BYTES);
  probe.destination = memalign(ESP_GRAPHICS_ALIGNMENT, PROBE_BYTES);
  ret = -ENOMEM;
  if (probe.source == NULL || probe.foreground == NULL ||
      probe.destination == NULL)
    {
      printf("graphics: allocation failed: %d\n", ret);
      goto out;
    }

  ret = esp_graphics_initialize();
  if (ret < 0)
    {
      printf("graphics: initialize failed: %d\n", ret);
      goto out;
    }

  printf("graphics: cpu=%d rounds=%lu bytes=%u src=%p fg=%p dst=%p\n",
         sched_getcpu(), rounds, PROBE_BYTES,
         probe.source, probe.foreground, probe.destination);

  for (round = 0; round < rounds; round++)
    {
      if (rounds > 1)
        {
          printf("graphics round %lu/%lu\n", round + 1, rounds);
        }

      for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        {
          if (strcmp(command, "all") == 0 ||
              strcmp(command, tests[i].name) == 0)
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
  free(probe.destination);
  free(probe.foreground);
  free(probe.source);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
