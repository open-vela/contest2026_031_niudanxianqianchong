/****************************************************************************
 * demos/smart_home/src/voice/smart_home_voice_capture.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "smart_home_voice_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>
#include <nuttx/irq.h>

#include "../smart_home_memory.h"

#ifdef CONFIG_SMART_HOME_VAD
#  include "smart_home_vad.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VOICE_CAPTURE_DEVICE   "/dev/audio/pcm_in0"
#define VOICE_CAPTURE_RATE     16000
#define VOICE_CAPTURE_MQ_MAX   32

/* NuttX 数字越大优先级越高：采集分发须高于 UI 主任务(100)，否则
 * 界面渲染挤占音频线程导致缓冲欠载丢帧；worker 只做 memcpy 分发。 */
#define VOICE_CAPTURE_PRIORITY 110

#ifndef CONFIG_SMART_HOME_VOICE_CAPTURE_STACKSIZE
#define CONFIG_SMART_HOME_VOICE_CAPTURE_STACKSIZE 8192
#endif

#ifndef CONFIG_AUDIO_BUFFER_NUMBYTES
#define CONFIG_AUDIO_BUFFER_NUMBYTES 2048
#endif

#ifndef CONFIG_AUDIO_NUM_BUFFERS
#define CONFIG_AUDIO_NUM_BUFFERS 8
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct voice_capture_state_s
{
  pthread_mutex_t mutex;
  pthread_t worker;
  void *worker_stack_alloc;   /* bulk 原始指针（释放用） */
  void *worker_stack;         /* 16B 对齐后交给 pthread */
  bool started;
  bool stop_requested;

  /* 单活跃消费者（mutex 保护）。 */
  voice_capture_frame_cb_t cb;
  void *user_data;

  /* 音频设备状态（仅 worker 线程访问）。 */
  int audio_fd;
  mqd_t mq;
  char mq_name[VOICE_CAPTURE_MQ_MAX];
  FAR struct ap_buffer_s **buffers;
  struct ap_buffer_info_s buffer_info;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct voice_capture_state_s g_capture =
{
  .mutex = PTHREAD_MUTEX_INITIALIZER,
  .audio_fd = -1,
  .mq = (mqd_t)-1,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void capture_dispatch(const FAR struct ap_buffer_s *apb)
{
  voice_capture_frame_cb_t cb;
  void *user_data;

  if (apb->nbytes < 2)
    {
      return;
    }

#ifdef CONFIG_SMART_HOME_VAD
  /* VAD 先于消费者执行：ASR 与 KWS 争用消费者槽位期间，
   * 采集分发路径仍然连续，端点判断不会因让位而中断。
   */
  smart_home_vad_process((const int16_t *)apb->samp, apb->nbytes / 2);
#endif

  pthread_mutex_lock(&g_capture.mutex);
  cb = g_capture.cb;
  user_data = g_capture.user_data;
  pthread_mutex_unlock(&g_capture.mutex);

  if (cb != NULL)
    {
      cb((const int16_t *)apb->samp, apb->nbytes / 2, user_data);
    }
}

static void capture_cleanup(FAR struct voice_capture_state_s *state)
{
  unsigned int index;
  struct audio_buf_desc_s desc;

  if (state->audio_fd >= 0)
    {
      ioctl(state->audio_fd, AUDIOIOC_STOP, 0);

      if (state->buffers != NULL)
        {
          for (index = 0; index < state->buffer_info.nbuffers; index++)
            {
              if (state->buffers[index] != NULL)
                {
                  memset(&desc, 0, sizeof(desc));
                  desc.u.buffer = state->buffers[index];
                  ioctl(state->audio_fd, AUDIOIOC_FREEBUFFER,
                        (unsigned long)(uintptr_t)&desc);
                }
            }

          free(state->buffers);
          state->buffers = NULL;
        }

      ioctl(state->audio_fd, AUDIOIOC_UNREGISTERMQ,
            (unsigned long)(uintptr_t)state->mq);
      ioctl(state->audio_fd, AUDIOIOC_RELEASE, 0);
      close(state->audio_fd);
      state->audio_fd = -1;
    }

  if (state->mq != (mqd_t)-1)
    {
      mq_close(state->mq);
      mq_unlink(state->mq_name);
      state->mq = (mqd_t)-1;
    }
}

static int capture_prepare(FAR struct voice_capture_state_s *state)
{
  struct audio_caps_desc_s caps;
  struct mq_attr attr;
  unsigned int index;
  int ret;

  state->audio_fd = open(VOICE_CAPTURE_DEVICE, O_RDWR | O_CLOEXEC);
  if (state->audio_fd < 0)
    {
      fprintf(stderr, "voice_capture: open %s failed: %d (%s)\n",
              VOICE_CAPTURE_DEVICE, -errno, strerror(errno));
      return -errno;
    }

  ret = ioctl(state->audio_fd, AUDIOIOC_RESERVE, 0);
  if (ret < 0)
    {
      fprintf(stderr, "voice_capture: reserve failed: %d\n", -errno);
      return -errno;
    }

  memset(&caps, 0, sizeof(caps));
  caps.caps.ac_len = sizeof(caps.caps);
  caps.caps.ac_type = AUDIO_TYPE_INPUT;
  caps.caps.ac_subtype = AUDIO_FMT_PCM;
  caps.caps.ac_channels = 1;
  caps.caps.ac_controls.hw[0] = VOICE_CAPTURE_RATE;
  caps.caps.ac_controls.b[2] = 16;  /* bits per sample */
  ret = ioctl(state->audio_fd, AUDIOIOC_CONFIGURE,
              (unsigned long)(uintptr_t)&caps);
  if (ret < 0)
    {
      fprintf(stderr, "voice_capture: configure failed: %d\n", -errno);
      return -errno;
    }

  if (ioctl(state->audio_fd, AUDIOIOC_GETBUFFERINFO,
            (unsigned long)(uintptr_t)&state->buffer_info) < 0)
    {
      state->buffer_info.buffer_size = CONFIG_AUDIO_BUFFER_NUMBYTES;
      state->buffer_info.nbuffers = CONFIG_AUDIO_NUM_BUFFERS;
    }

  if (state->buffer_info.nbuffers == 0 ||
      state->buffer_info.buffer_size == 0)
    {
      return -EINVAL;
    }

  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg = state->buffer_info.nbuffers + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  snprintf(state->mq_name, sizeof(state->mq_name), "/voice_cap_%ld",
           (long)getpid());
  state->mq = mq_open(state->mq_name, O_RDWR | O_CREAT, 0644, &attr);
  if (state->mq == (mqd_t)-1)
    {
      return -errno;
    }

  ret = ioctl(state->audio_fd, AUDIOIOC_REGISTERMQ,
              (unsigned long)(uintptr_t)state->mq);
  if (ret < 0)
    {
      return -errno;
    }

  state->buffers = calloc(state->buffer_info.nbuffers,
                          sizeof(FAR struct ap_buffer_s *));
  if (state->buffers == NULL)
    {
      return -ENOMEM;
    }

  for (index = 0; index < state->buffer_info.nbuffers; index++)
    {
      struct audio_buf_desc_s desc;

      memset(&desc, 0, sizeof(desc));
      desc.numbytes = state->buffer_info.buffer_size;
      desc.u.pbuffer = &state->buffers[index];
      ret = ioctl(state->audio_fd, AUDIOIOC_ALLOCBUFFER,
                  (unsigned long)(uintptr_t)&desc);
      if (ret < 0)
        {
          return -errno;
        }
    }

  return 0;
}

static void *capture_worker_main(void *arg)
{
  FAR struct voice_capture_state_s *state = &g_capture;
  struct audio_msg_s message;
  struct audio_buf_desc_s desc;
  unsigned int priority;
  unsigned int index;
  ssize_t size;
  int ret;

  (void)arg;

  ret = capture_prepare(state);
  if (ret < 0)
    {
      capture_cleanup(state);
      pthread_mutex_lock(&state->mutex);
      state->started = false;
      state->stop_requested = false;
      pthread_mutex_unlock(&state->mutex);
      return NULL;
    }

  /* 预填空缓冲并启动采集。 */
  for (index = 0; index < state->buffer_info.nbuffers; index++)
    {
      memset(&desc, 0, sizeof(desc));
      desc.numbytes = state->buffers[index]->nmaxbytes;
      desc.u.buffer = state->buffers[index];
      if (ioctl(state->audio_fd, AUDIOIOC_ENQUEUEBUFFER,
                (unsigned long)(uintptr_t)&desc) < 0)
        {
          capture_cleanup(state);
          pthread_mutex_lock(&state->mutex);
          state->started = false;
          state->stop_requested = false;
          pthread_mutex_unlock(&state->mutex);
          return NULL;
        }
    }

  if (ioctl(state->audio_fd, AUDIOIOC_START, 0) < 0)
    {
      capture_cleanup(state);
      pthread_mutex_lock(&state->mutex);
      state->started = false;
      state->stop_requested = false;
      pthread_mutex_unlock(&state->mutex);
      return NULL;
    }

  while (true)
    {
      struct timespec deadline;

      pthread_mutex_lock(&state->mutex);
      bool stop = state->stop_requested;
      pthread_mutex_unlock(&state->mutex);
      if (stop)
        {
          break;
        }

      /* 带超时接收：无音频消息时也能周期性检查 stop 标志。 */
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_nsec += 200 * 1000000L;
      if (deadline.tv_nsec >= 1000000000L)
        {
          deadline.tv_sec++;
          deadline.tv_nsec -= 1000000000L;
        }

      size = mq_timedreceive(state->mq, (FAR char *)&message,
                             sizeof(message), &priority, &deadline);
      if (size != sizeof(message))
        {
          if (errno == EINTR || errno == ETIMEDOUT)
            {
              continue;
            }

          break;
        }

      if (message.msg_id == AUDIO_MSG_DEQUEUE)
        {
          FAR struct ap_buffer_s *apb =
              (FAR struct ap_buffer_s *)message.u.ptr;

          capture_dispatch(apb);

          memset(&desc, 0, sizeof(desc));
          desc.numbytes = apb->nmaxbytes;
          desc.u.buffer = apb;
          if (ioctl(state->audio_fd, AUDIOIOC_ENQUEUEBUFFER,
                    (unsigned long)(uintptr_t)&desc) < 0)
            {
              break;
            }
        }
      else if (message.msg_id == AUDIO_MSG_IOERR)
        {
          fprintf(stderr, "voice_capture: ioerr\n");
          break;
        }
    }

  capture_cleanup(state);
  return NULL;
}

/* voice_capture_record 的收集器状态（运行于调用线程栈）。 */
struct capture_collector_s
{
  uint8_t *buffer;
  size_t used;
  size_t capacity;
};

static void capture_collect_cb(const int16_t *samples, size_t sample_count,
                               void *user_data)
{
  struct capture_collector_s *collector = user_data;
  size_t bytes = sample_count * 2;

  if (bytes > collector->capacity - collector->used)
    {
      bytes = collector->capacity - collector->used;
    }

  if (bytes > 0)
    {
      memcpy(collector->buffer + collector->used, samples, bytes);
      collector->used += bytes;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int voice_capture_start(void)
{
  pthread_attr_t attr;
  int attr_ready = 0;
  int ret;

  pthread_mutex_lock(&g_capture.mutex);
  if (g_capture.started)
    {
      pthread_mutex_unlock(&g_capture.mutex);
      return 0;
    }

  g_capture.stop_requested = false;
  g_capture.audio_fd = -1;
  g_capture.mq = (mqd_t)-1;
  g_capture.buffers = NULL;

  /* RISC-V pthread 栈须 16B 对齐，bulk 堆不保证（同 voice_play）。 */
  g_capture.worker_stack_alloc = smart_home_bulk_alloc(
      CONFIG_SMART_HOME_VOICE_CAPTURE_STACKSIZE + STACK_ALIGNMENT - 1u);
  if (g_capture.worker_stack_alloc == NULL)
    {
      pthread_mutex_unlock(&g_capture.mutex);
      return -ENOMEM;
    }

  g_capture.worker_stack = (void *)STACK_ALIGN_UP(
      (uintptr_t)g_capture.worker_stack_alloc);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      attr_ready = 1;
      ret = pthread_attr_setstack(&attr, g_capture.worker_stack,
                                  CONFIG_SMART_HOME_VOICE_CAPTURE_STACKSIZE);
    }

  if (ret == 0)
    {
      ret = pthread_attr_setschedparam(&attr,
          &(struct sched_param)
          {
            .sched_priority = VOICE_CAPTURE_PRIORITY
          });
    }

  if (ret == 0)
    {
      /* worker 在 prepare 失败路径自行复位 started 标志前先置位，
       * 使失败对调用方表现为"短暂启动后仍不可用"。 */
      g_capture.started = true;
      ret = pthread_create(&g_capture.worker, &attr, capture_worker_main,
                           NULL);
    }

  if (attr_ready)
    {
      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      g_capture.started = false;
      smart_home_bulk_free(g_capture.worker_stack_alloc);
      g_capture.worker_stack_alloc = NULL;
      g_capture.worker_stack = NULL;
      pthread_mutex_unlock(&g_capture.mutex);
      return -ret;
    }

  pthread_mutex_unlock(&g_capture.mutex);
  return 0;
}

void voice_capture_deinit(void)
{
  pthread_mutex_lock(&g_capture.mutex);
  if (!g_capture.started)
    {
      pthread_mutex_unlock(&g_capture.mutex);
      return;
    }

  g_capture.stop_requested = true;
  g_capture.cb = NULL;
  g_capture.user_data = NULL;
  pthread_mutex_unlock(&g_capture.mutex);

  /* 唤醒 worker（mq 非阻塞轮询 + stop 标志，worker 至多一个缓冲周期
   * 后退出；pthread_join 由持锁外的当前线程执行）。 */
  pthread_join(g_capture.worker, NULL);

  pthread_mutex_lock(&g_capture.mutex);
  g_capture.started = false;
  g_capture.stop_requested = false;
  pthread_mutex_unlock(&g_capture.mutex);

  smart_home_bulk_free(g_capture.worker_stack_alloc);
  g_capture.worker_stack_alloc = NULL;
  g_capture.worker_stack = NULL;
}

int voice_capture_listen(voice_capture_frame_cb_t cb, void *user_data)
{
  pthread_mutex_lock(&g_capture.mutex);
  g_capture.cb = cb;
  g_capture.user_data = user_data;
  pthread_mutex_unlock(&g_capture.mutex);
  return 0;
}

int voice_capture_stop_listening(voice_capture_frame_cb_t cb)
{
  pthread_mutex_lock(&g_capture.mutex);
  if (g_capture.cb == cb)
    {
      g_capture.cb = NULL;
      g_capture.user_data = NULL;
    }

  pthread_mutex_unlock(&g_capture.mutex);
  return 0;
}

/* 录音主体：auto_stop_ms > 0 时按 VAD 端点提前结束。
 *
 * 只有在"曾经检测到人声"之后，持续静音 auto_stop_ms 才提前结束；
 * 全程无人声时仍录满 seconds，避免把噪声场景裁成空录音。
 */
static int capture_record_impl(int seconds, const volatile int *abort,
                               int auto_stop_ms,
                               uint8_t **out_pcm, size_t *out_bytes)
{
  struct capture_collector_s collector;
  voice_capture_frame_cb_t previous_cb;
  void *previous_user;
  struct timespec start;
  struct timespec now;
  long elapsed_ms;
  long budget_ms;
  uint64_t now_ms;
  uint64_t last_speech_ms = 0;
  int saw_speech = 0;
  int ret;

  if (seconds <= 0 || out_pcm == NULL || out_bytes == NULL)
    {
      return -EINVAL;
    }

  *out_pcm = NULL;
  *out_bytes = 0;

  ret = voice_capture_start();
  if (ret != 0)
    {
      return ret;
    }

  collector.capacity = (size_t)seconds * VOICE_CAPTURE_RATE * 2u;
  collector.buffer = smart_home_bulk_alloc(collector.capacity);
  if (collector.buffer == NULL)
    {
      return -ENOMEM;
    }

  collector.used = 0;

  /* 暂存并替换当前消费者（KWS 让位），录制结束后恢复。
   * VAD 运行在采集分发路径上，不受消费者替换影响。
   */
  pthread_mutex_lock(&g_capture.mutex);
  previous_cb = g_capture.cb;
  previous_user = g_capture.user_data;
  g_capture.cb = capture_collect_cb;
  g_capture.user_data = &collector;
  pthread_mutex_unlock(&g_capture.mutex);

  clock_gettime(CLOCK_MONOTONIC, &start);
  budget_ms = (long)seconds * 1000L;

  while (true)
    {
      bool running;

      pthread_mutex_lock(&g_capture.mutex);
      running = g_capture.started;
      pthread_mutex_unlock(&g_capture.mutex);

      clock_gettime(CLOCK_MONOTONIC, &now);
      elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
                   (now.tv_nsec - start.tv_nsec) / 1000000L;
      now_ms = (uint64_t)now.tv_sec * 1000u +
               (uint64_t)now.tv_nsec / 1000000u;

      if (elapsed_ms >= budget_ms || !running ||
          (abort != NULL && *abort != 0) ||
          collector.used >= collector.capacity)
        {
          break;
        }

      if (auto_stop_ms > 0)
        {
          if (voice_capture_speech_active())
            {
              saw_speech = 1;
              last_speech_ms = now_ms;
            }
          else if (saw_speech && elapsed_ms >= 300 &&
                   now_ms - last_speech_ms >= (uint64_t)auto_stop_ms)
            {
              break;
            }
        }

      usleep(20000);
    }

  /* 恢复原消费者。 */
  pthread_mutex_lock(&g_capture.mutex);
  if (g_capture.cb == capture_collect_cb)
    {
      g_capture.cb = previous_cb;
      g_capture.user_data = previous_user;
    }

  pthread_mutex_unlock(&g_capture.mutex);

  if (collector.used == 0)
    {
      smart_home_bulk_free(collector.buffer);
      return -ENODATA;
    }

  *out_pcm = collector.buffer;
  *out_bytes = collector.used;
  return 0;
}

int voice_capture_record(int seconds, const volatile int *abort,
                         uint8_t **out_pcm, size_t *out_bytes)
{
  return capture_record_impl(seconds, abort, 0, out_pcm, out_bytes);
}

int voice_capture_record_vad(int seconds, const volatile int *abort,
                             int silence_ms,
                             uint8_t **out_pcm, size_t *out_bytes)
{
#ifdef CONFIG_SMART_HOME_VAD
  return capture_record_impl(seconds, abort, silence_ms,
                             out_pcm, out_bytes);
#else
  (void)silence_ms;
  return capture_record_impl(seconds, abort, 0, out_pcm, out_bytes);
#endif
}

int voice_capture_speech_active(void)
{
#ifdef CONFIG_SMART_HOME_VAD
  return smart_home_vad_active();
#else
  return 0;
#endif
}
