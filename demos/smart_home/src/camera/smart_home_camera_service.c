/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Smart Home SC2336 preview service.
 *
 * This is the sole /dev/video0 consumer.  Its worker owns the V4L2 USERPTR
 * ring; it copies each completed frame into a service-owned double preview
 * buffer before immediately returning the DMA buffer to V4L2.  LVGL only
 * receives a copy through smart_home_camera_copy_latest().
 */

#include <nuttx/config.h>

#include "smart_home_camera_service.h"
#include "../smart_home_memory.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/videoio.h>
#include <time.h>
#include <unistd.h>

#define SMART_HOME_CAMERA_PATH              "/dev/video0"
#define SMART_HOME_CAMERA_SOURCE_WIDTH      1024u
#define SMART_HOME_CAMERA_SOURCE_HEIGHT     600u
#define SMART_HOME_CAMERA_SOURCE_BPP        2u
#define SMART_HOME_CAMERA_SOURCE_BYTES      \
  (SMART_HOME_CAMERA_SOURCE_WIDTH * SMART_HOME_CAMERA_SOURCE_HEIGHT * \
   SMART_HOME_CAMERA_SOURCE_BPP)
#define SMART_HOME_CAMERA_CAPTURE_FPS       30u
#define SMART_HOME_CAMERA_BUFFER_COUNT      3u
#define SMART_HOME_CAMERA_BUFFER_ALIGNMENT  64u
#define SMART_HOME_CAMERA_STACK_SIZE        8192u
#define SMART_HOME_CAMERA_POLL_TIMEOUT_MS   200

struct smart_home_camera_service_s
{
  pthread_mutex_t lock;
  pthread_t thread;
  void *thread_stack_alloc;
  bool thread_active;
  bool join_in_progress;
  bool stop_requested;
  int fd;

  void *capture_alloc[SMART_HOME_CAMERA_BUFFER_COUNT];
  uint8_t *capture[SMART_HOME_CAMERA_BUFFER_COUNT];
  uint8_t *preview[2];
  int published_preview;
  struct smart_home_camera_status_s status;
  uint64_t fps_window_us;
  uint32_t fps_window_frames;
};

static struct smart_home_camera_service_s g_camera =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .fd = -1,
  .published_preview = 0,
};

static uint64_t smart_home_camera_now_us(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return 0;
    }

  return (uint64_t)now.tv_sec * 1000000u + now.tv_nsec / 1000u;
}

static int smart_home_camera_ioctl(int fd, int request, void *arg)
{
  if (ioctl(fd, request, (uintptr_t)arg) < 0)
    {
      return -errno;
    }

  return OK;
}

static void *smart_home_camera_aligned_alloc(size_t bytes, void **raw)
{
  uintptr_t pointer;

  *raw = smart_home_bulk_alloc(bytes + SMART_HOME_CAMERA_BUFFER_ALIGNMENT - 1u);
  if (*raw == NULL)
    {
      return NULL;
    }

  pointer = ((uintptr_t)*raw + SMART_HOME_CAMERA_BUFFER_ALIGNMENT - 1u) &
            ~((uintptr_t)SMART_HOME_CAMERA_BUFFER_ALIGNMENT - 1u);
  return (void *)pointer;
}

static void smart_home_camera_free_buffers(void)
{
  unsigned int index;

  for (index = 0; index < SMART_HOME_CAMERA_BUFFER_COUNT; index++)
    {
      smart_home_bulk_free(g_camera.capture_alloc[index]);
      g_camera.capture_alloc[index] = NULL;
      g_camera.capture[index] = NULL;
    }

  for (index = 0; index < 2; index++)
    {
      smart_home_bulk_free(g_camera.preview[index]);
      g_camera.preview[index] = NULL;
    }
}

static int smart_home_camera_queue_buffer(int fd, unsigned int index)
{
  struct v4l2_buffer buffer;

  memset(&buffer, 0, sizeof(buffer));
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_USERPTR;
  buffer.index = index;
  buffer.m.userptr = (unsigned long)g_camera.capture[index];
  buffer.length = SMART_HOME_CAMERA_SOURCE_BYTES;
  return smart_home_camera_ioctl(fd, VIDIOC_QBUF, &buffer);
}

static int smart_home_camera_configure(int fd)
{
  struct v4l2_capability capability;
  struct v4l2_format format;
  struct v4l2_streamparm parameter;
  struct v4l2_requestbuffers request;
  unsigned int index;
  int ret;

  memset(&capability, 0, sizeof(capability));
  ret = smart_home_camera_ioctl(fd, VIDIOC_QUERYCAP, &capability);
  if (ret < 0 ||
      (capability.capabilities & (V4L2_CAP_VIDEO_CAPTURE |
                                  V4L2_CAP_STREAMING)) !=
      (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING))
    {
      return ret < 0 ? ret : -ENOTSUP;
    }

  memset(&format, 0, sizeof(format));
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = SMART_HOME_CAMERA_SOURCE_WIDTH;
  format.fmt.pix.height = SMART_HOME_CAMERA_SOURCE_HEIGHT;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  format.fmt.pix.bytesperline = SMART_HOME_CAMERA_SOURCE_WIDTH *
                                SMART_HOME_CAMERA_SOURCE_BPP;
  format.fmt.pix.sizeimage = SMART_HOME_CAMERA_SOURCE_BYTES;
  ret = smart_home_camera_ioctl(fd, VIDIOC_S_FMT, &format);
  if (ret < 0)
    {
      return ret;
    }

  if (format.fmt.pix.width != SMART_HOME_CAMERA_SOURCE_WIDTH ||
      format.fmt.pix.height != SMART_HOME_CAMERA_SOURCE_HEIGHT ||
      format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 ||
      format.fmt.pix.sizeimage != SMART_HOME_CAMERA_SOURCE_BYTES)
    {
      return -ENOTSUP;
    }

  memset(&parameter, 0, sizeof(parameter));
  parameter.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parameter.parm.capture.timeperframe.numerator = 1;
  parameter.parm.capture.timeperframe.denominator = SMART_HOME_CAMERA_CAPTURE_FPS;
  ret = smart_home_camera_ioctl(fd, VIDIOC_S_PARM, &parameter);
  if (ret < 0)
    {
      return ret;
    }

  memset(&request, 0, sizeof(request));
  request.count = SMART_HOME_CAMERA_BUFFER_COUNT;
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_USERPTR;
  request.mode = V4L2_BUF_MODE_RING;
  ret = smart_home_camera_ioctl(fd, VIDIOC_REQBUFS, &request);
  if (ret < 0 || request.count != SMART_HOME_CAMERA_BUFFER_COUNT)
    {
      return ret < 0 ? ret : -ENOMEM;
    }

  for (index = 0; index < SMART_HOME_CAMERA_BUFFER_COUNT; index++)
    {
      g_camera.capture[index] =
        smart_home_camera_aligned_alloc(SMART_HOME_CAMERA_SOURCE_BYTES,
                                        &g_camera.capture_alloc[index]);
      if (g_camera.capture[index] == NULL)
        {
          return -ENOMEM;
        }

      ret = smart_home_camera_queue_buffer(fd, index);
      if (ret < 0)
        {
          return ret;
        }
    }

  g_camera.preview[0] = smart_home_bulk_alloc(SMART_HOME_CAMERA_PREVIEW_BYTES);
  g_camera.preview[1] = smart_home_bulk_alloc(SMART_HOME_CAMERA_PREVIEW_BYTES);
  if (g_camera.preview[0] == NULL || g_camera.preview[1] == NULL)
    {
      return -ENOMEM;
    }

  return OK;
}

static void smart_home_camera_downscale_rgb565(const uint8_t *source,
                                                uint8_t *destination)
{
  const uint16_t *src = (const uint16_t *)source;
  uint16_t *dst = (uint16_t *)destination;
  unsigned int y;
  unsigned int x;

  for (y = 0; y < SMART_HOME_CAMERA_PREVIEW_HEIGHT; y++)
    {
      unsigned int source_y = y * SMART_HOME_CAMERA_SOURCE_HEIGHT /
                              SMART_HOME_CAMERA_PREVIEW_HEIGHT;
      const uint16_t *source_row = src + source_y * SMART_HOME_CAMERA_SOURCE_WIDTH;
      uint16_t *destination_row = dst + y * SMART_HOME_CAMERA_PREVIEW_WIDTH;

      for (x = 0; x < SMART_HOME_CAMERA_PREVIEW_WIDTH; x++)
        {
          destination_row[x] = source_row[x * SMART_HOME_CAMERA_SOURCE_WIDTH /
                                          SMART_HOME_CAMERA_PREVIEW_WIDTH];
        }
    }
}

static bool smart_home_camera_stop_requested(void)
{
  bool stop;

  pthread_mutex_lock(&g_camera.lock);
  stop = g_camera.stop_requested;
  pthread_mutex_unlock(&g_camera.lock);
  return stop;
}

static void smart_home_camera_record_capture(uint32_t capture_sequence)
{
  uint64_t now = smart_home_camera_now_us();

  pthread_mutex_lock(&g_camera.lock);
  g_camera.status.capture_sequence = capture_sequence;
  g_camera.fps_window_frames++;
  if (g_camera.fps_window_us == 0)
    {
      g_camera.fps_window_us = now;
    }
  else if (now > g_camera.fps_window_us + 1000000u)
    {
      uint64_t elapsed = now - g_camera.fps_window_us;
      uint32_t fps_x100 = (uint32_t)((uint64_t)g_camera.fps_window_frames *
                                     100000000u / elapsed);
      g_camera.status.capture_fps_x100 = fps_x100;
      g_camera.fps_window_frames = 0;
      g_camera.fps_window_us = now;
    }
  pthread_mutex_unlock(&g_camera.lock);
}

static void smart_home_camera_publish_preview(const uint8_t *source)
{
  int write_index;

  pthread_mutex_lock(&g_camera.lock);
  write_index = 1 - g_camera.published_preview;
  pthread_mutex_unlock(&g_camera.lock);

  smart_home_camera_downscale_rgb565(source, g_camera.preview[write_index]);

  pthread_mutex_lock(&g_camera.lock);
  g_camera.published_preview = write_index;
  g_camera.status.preview_sequence++;
  g_camera.status.preview_fps_x100 = 1500;
  pthread_mutex_unlock(&g_camera.lock);
}

static int smart_home_camera_capture_loop(int fd)
{
  struct pollfd pollfd;
  struct v4l2_buffer buffer;
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  uint32_t previous_sequence = 0;
  bool have_sequence = false;
  int ret;

  ret = smart_home_camera_ioctl(fd, VIDIOC_STREAMON, &type);
  if (ret < 0)
    {
      return ret;
    }

  pollfd.fd = fd;
  pollfd.events = POLLIN;
  while (!smart_home_camera_stop_requested())
    {
      pollfd.revents = 0;
      ret = poll(&pollfd, 1, SMART_HOME_CAMERA_POLL_TIMEOUT_MS);
      if (ret == 0)
        {
          continue;
        }

      if (ret < 0)
        {
          return -errno;
        }

      if ((pollfd.revents & POLLIN) == 0)
        {
          return -EIO;
        }

      memset(&buffer, 0, sizeof(buffer));
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_USERPTR;
      ret = smart_home_camera_ioctl(fd, VIDIOC_DQBUF, &buffer);
      if (ret < 0)
        {
          return ret;
        }

      if (buffer.index >= SMART_HOME_CAMERA_BUFFER_COUNT ||
          buffer.m.userptr != (unsigned long)g_camera.capture[buffer.index] ||
          buffer.bytesused != SMART_HOME_CAMERA_SOURCE_BYTES ||
          (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0)
        {
          return -EIO;
        }

      if (have_sequence && (int32_t)(buffer.sequence - previous_sequence) <= 0)
        {
          return -EIO;
        }

      if (have_sequence && buffer.sequence > previous_sequence + 1u)
        {
          pthread_mutex_lock(&g_camera.lock);
          g_camera.status.dropped_frames += buffer.sequence - previous_sequence - 1u;
          pthread_mutex_unlock(&g_camera.lock);
        }

      smart_home_camera_record_capture(buffer.sequence);

      /* Keep CSI/ISP at its validated 30 FPS while doing the expensive
       * RGB565 downscale only once per two source frames. */
      if ((buffer.sequence & 1u) == 0u)
        {
          smart_home_camera_publish_preview(g_camera.capture[buffer.index]);
        }
      previous_sequence = buffer.sequence;
      have_sequence = true;

      ret = smart_home_camera_queue_buffer(fd, buffer.index);
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

static void *smart_home_camera_worker(void *arg)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int fd;
  int ret;

  (void)arg;
  fd = open(SMART_HOME_CAMERA_PATH, O_RDWR);
  if (fd < 0)
    {
      ret = -errno;
      goto done;
    }

  pthread_mutex_lock(&g_camera.lock);
  g_camera.fd = fd;
  pthread_mutex_unlock(&g_camera.lock);

  ret = smart_home_camera_configure(fd);
  if (ret < 0)
    {
      goto close_fd;
    }

  pthread_mutex_lock(&g_camera.lock);
  g_camera.status.state = SMART_HOME_CAMERA_RUNNING;
  pthread_mutex_unlock(&g_camera.lock);
  printf("[smart_home_camera] start path=%s source=1024x600 rgb565 preview=%ux%u target=15fps\n",
         SMART_HOME_CAMERA_PATH, SMART_HOME_CAMERA_PREVIEW_WIDTH,
         SMART_HOME_CAMERA_PREVIEW_HEIGHT);

  ret = smart_home_camera_capture_loop(fd);
  (void)smart_home_camera_ioctl(fd, VIDIOC_STREAMOFF, &type);

close_fd:
  close(fd);
  pthread_mutex_lock(&g_camera.lock);
  g_camera.fd = -1;
  pthread_mutex_unlock(&g_camera.lock);

done:
  pthread_mutex_lock(&g_camera.lock);
  if (ret < 0 && !g_camera.stop_requested)
    {
      g_camera.status.state = SMART_HOME_CAMERA_ERROR;
      g_camera.status.last_error = ret;
      printf("[smart_home_camera] failed ret=%d\n", ret);
    }
  else
    {
      g_camera.status.state = SMART_HOME_CAMERA_OFF;
      g_camera.status.last_error = 0;
    }
  pthread_mutex_unlock(&g_camera.lock);
  return NULL;
}

int smart_home_camera_start(void)
{
  pthread_attr_t attr;
  void *stack_alloc;
  void *stack;
  bool attr_initialized = false;
  int ret;

  pthread_mutex_lock(&g_camera.lock);
  if (g_camera.thread_active)
    {
      ret = g_camera.status.state == SMART_HOME_CAMERA_RUNNING ? OK : -EBUSY;
      pthread_mutex_unlock(&g_camera.lock);
      return ret;
    }

  stack = smart_home_camera_aligned_alloc(SMART_HOME_CAMERA_STACK_SIZE,
                                          &stack_alloc);
  if (stack == NULL)
    {
      pthread_mutex_unlock(&g_camera.lock);
      return -ENOMEM;
    }

  g_camera.thread_stack_alloc = stack_alloc;

  memset(&g_camera.status, 0, sizeof(g_camera.status));
  g_camera.status.state = SMART_HOME_CAMERA_STARTING;
  g_camera.stop_requested = false;
  g_camera.join_in_progress = false;
  g_camera.published_preview = 0;
  g_camera.fps_window_us = 0;
  g_camera.fps_window_frames = 0;
  pthread_mutex_unlock(&g_camera.lock);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      attr_initialized = true;
#ifdef __NuttX__
      ret = pthread_attr_setstack(&attr, stack, SMART_HOME_CAMERA_STACK_SIZE);
      if (ret == 0)
        {
          cpu_set_t cpuset;

          /* V4L2 capture and the preview copy run on CPU0, next to the DSI
           * scanout, so frame buffers never cross cores during cache sync.
           */

          CPU_ZERO(&cpuset);
          CPU_SET(0, &cpuset);
          ret = pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
        }
#else
      (void)stack;
#endif
    }
  if (ret == 0)
    {
      ret = pthread_create(&g_camera.thread, &attr, smart_home_camera_worker,
                           NULL);
    }
  if (attr_initialized)
    {
      pthread_attr_destroy(&attr);
    }
  if (ret != 0)
    {
      pthread_mutex_lock(&g_camera.lock);
      smart_home_bulk_free(g_camera.thread_stack_alloc);
      g_camera.thread_stack_alloc = NULL;
      g_camera.status.state = SMART_HOME_CAMERA_ERROR;
      g_camera.status.last_error = -ret;
      pthread_mutex_unlock(&g_camera.lock);
      return -ret;
    }

  pthread_mutex_lock(&g_camera.lock);
  g_camera.thread_active = true;
  pthread_mutex_unlock(&g_camera.lock);
  return OK;
}

int smart_home_camera_stop(void)
{
  pthread_t thread;
  void *stack;
  int ret;

  pthread_mutex_lock(&g_camera.lock);
  if (!g_camera.thread_active)
    {
      pthread_mutex_unlock(&g_camera.lock);
      return OK;
    }

  if (g_camera.join_in_progress)
    {
      pthread_mutex_unlock(&g_camera.lock);
      return -EBUSY;
    }

  g_camera.stop_requested = true;
  g_camera.join_in_progress = true;
  thread = g_camera.thread;
  pthread_mutex_unlock(&g_camera.lock);

  ret = pthread_join(thread, NULL);
  if (ret != 0)
    {
      return -ret;
    }

  pthread_mutex_lock(&g_camera.lock);
  stack = g_camera.thread_stack_alloc;
  g_camera.thread_stack_alloc = NULL;
  g_camera.thread_active = false;
  g_camera.join_in_progress = false;
  g_camera.stop_requested = false;
  if (g_camera.status.state == SMART_HOME_CAMERA_OFF)
    {
      g_camera.status.capture_sequence = 0;
      g_camera.status.preview_sequence = 0;
      g_camera.status.capture_fps_x100 = 0;
      g_camera.status.preview_fps_x100 = 0;
      g_camera.status.dropped_frames = 0;
    }
  smart_home_camera_free_buffers();
  pthread_mutex_unlock(&g_camera.lock);
  smart_home_bulk_free(stack);
  printf("[smart_home_camera] stop\n");
  return OK;
}

int smart_home_camera_get_status(struct smart_home_camera_status_s *status)
{
  if (status == NULL)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_camera.lock);
  *status = g_camera.status;
  pthread_mutex_unlock(&g_camera.lock);
  return OK;
}

int smart_home_camera_copy_latest(uint8_t *dst, size_t dst_bytes,
                                  uint32_t *sequence)
{
  int index;

  if (dst == NULL || dst_bytes < SMART_HOME_CAMERA_PREVIEW_BYTES)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_camera.lock);
  if (g_camera.status.state != SMART_HOME_CAMERA_RUNNING ||
      g_camera.status.preview_sequence == 0 ||
      g_camera.preview[g_camera.published_preview] == NULL)
    {
      pthread_mutex_unlock(&g_camera.lock);
      return -EAGAIN;
    }

  index = g_camera.published_preview;
  memcpy(dst, g_camera.preview[index], SMART_HOME_CAMERA_PREVIEW_BYTES);
  if (sequence != NULL)
    {
      *sequence = g_camera.status.preview_sequence;
    }
  pthread_mutex_unlock(&g_camera.lock);
  return OK;
}
