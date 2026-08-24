/****************************************************************************
 * app/gt911_probe/gt911_probe_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Raw event validation for the ESP32-P4X GT911 touchscreen.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <nuttx/clock.h>
#include <nuttx/input/touchscreen.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GT911_PROBE_PATH             "/dev/input0"
#define GT911_PROBE_MAX_POINTS       5
#define GT911_PROBE_SECONDS_DEFAULT  30
#define GT911_PROBE_SECONDS_MAX      600

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR const char *gt911_probe_event_name(uint8_t flags)
{
  if ((flags & TOUCH_DOWN) != 0)
    {
      return "DOWN";
    }

  if ((flags & TOUCH_MOVE) != 0)
    {
      return "MOVE";
    }

  if ((flags & TOUCH_UP) != 0)
    {
      return "UP";
    }

  return "UNKNOWN";
}

static int gt911_probe_parse_seconds(int argc, FAR char *argv[],
                                     FAR unsigned int *seconds)
{
  FAR char *end;
  unsigned long value;

  *seconds = GT911_PROBE_SECONDS_DEFAULT;
  if (argc == 1)
    {
      return OK;
    }

  if (argc != 2)
    {
      return -EINVAL;
    }

  value = strtoul(argv[1], &end, 10);
  if (*argv[1] == '\0' || *end != '\0' || value == 0 ||
      value > GT911_PROBE_SECONDS_MAX)
    {
      return -EINVAL;
    }

  *seconds = (unsigned int)value;
  return OK;
}

static void gt911_probe_print_sample(FAR const struct touch_sample_s *sample)
{
  int index;

  printf("gt911_probe: sample points=%ld\n", (long)sample->npoints);
  for (index = 0; index < sample->npoints; index++)
    {
      FAR const struct touch_point_s *point = &sample->point[index];

      printf("  %s id=%u x=%d y=%d size=%dx%d flags=0x%02x\n",
             gt911_probe_event_name(point->flags), point->id,
             point->x, point->y, point->w, point->h, point->flags);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  uint8_t buffer[SIZEOF_TOUCH_SAMPLE_S(GT911_PROBE_MAX_POINTS)];
  struct pollfd pfd;
  clock_t deadline;
  unsigned int seconds;
  int fd;
  int ret;

  ret = gt911_probe_parse_seconds(argc, argv, &seconds);
  if (ret < 0)
    {
      fprintf(stderr, "Usage: gt911_probe [seconds: 1-%u]\n",
              GT911_PROBE_SECONDS_MAX);
      return EXIT_FAILURE;
    }

  fd = open(GT911_PROBE_PATH, O_RDONLY);
  if (fd < 0)
    {
      fprintf(stderr, "gt911_probe: open %s failed: %d\n",
              GT911_PROBE_PATH, errno);
      return EXIT_FAILURE;
    }

  printf("=== ESP32-P4X GT911 Touch Probe ===\n");
  printf("reading %s for %u seconds; touch the panel now\n",
         GT911_PROBE_PATH, seconds);

  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  deadline = clock_systime_ticks() + SEC2TICK(seconds);

  while ((sclock_t)(clock_systime_ticks() - deadline) < 0)
    {
      FAR struct touch_sample_s *sample =
        (FAR struct touch_sample_s *)buffer;
      ssize_t nread;

      ret = poll(&pfd, 1, 250);
      if (ret == 0)
        {
          continue;
        }

      if (ret < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          fprintf(stderr, "gt911_probe: poll failed: %d\n", errno);
          close(fd);
          return EXIT_FAILURE;
        }

      if ((pfd.revents & POLLIN) == 0)
        {
          continue;
        }

      nread = read(fd, buffer, sizeof(buffer));
      if (nread < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          fprintf(stderr, "gt911_probe: read failed: %d\n", errno);
          close(fd);
          return EXIT_FAILURE;
        }

      if ((size_t)nread < SIZEOF_TOUCH_SAMPLE_S(1) ||
          sample->npoints < 1 || sample->npoints > GT911_PROBE_MAX_POINTS)
        {
          fprintf(stderr, "gt911_probe: invalid sample bytes=%ld "
                  "points=%ld\n",
                  (long)nread, (long)sample->npoints);
          continue;
        }

      gt911_probe_print_sample(sample);
    }

  close(fd);
  printf("gt911_probe: capture complete\n");
  return EXIT_SUCCESS;
}
