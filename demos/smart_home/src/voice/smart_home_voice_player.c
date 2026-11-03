/****************************************************************************
 * demos/smart_home/src/voice/smart_home_voice_player.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "smart_home_voice_player.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define VOICE_PLAYER_DEVICE      "/dev/audio/pcm0"
#define VOICE_PLAYER_MQ_NAME_MAX 32

/* ES8311 只在 CONFIG_AUDIO_DRIVER_SPECIFIC_BUFFERS 下回报推荐缓冲参数，
 * 否则使用通用默认值。与 audio_smoke 的降级路径保持一致。 */
#define VOICE_PLAYER_FALLBACK_BUFFER_SIZE CONFIG_AUDIO_BUFFER_NUMBYTES
#define VOICE_PLAYER_FALLBACK_NBUFFERS    CONFIG_AUDIO_NUM_BUFFERS

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct voice_player_state_s
{
  FAR struct ap_buffer_s **buffers;
  struct audio_buf_desc_s desc;
  struct ap_buffer_info_s buffer_info;
  mqd_t mq;
  char mq_name[VOICE_PLAYER_MQ_NAME_MAX];
  int audio_fd;
  unsigned int outstanding;
  bool started;
  bool stopping;

  /* 数据来源（二选一） */
  const uint8_t *mem;          /* 内存缓冲 */
  int file_fd;                 /* 原始 PCM 文件 */
  size_t remaining;            /* 尚未送入 codec 的字节数 */

  smart_home_voice_abort_t *abort;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool voice_player_aborted(const struct voice_player_state_s *state)
{
  return state->abort != NULL && state->abort->abort_flag != 0;
}

static void voice_player_cleanup(FAR struct voice_player_state_s *state)
{
  unsigned int index;

  if (state->audio_fd >= 0)
    {
      if (state->started && !state->stopping)
        {
          ioctl(state->audio_fd, AUDIOIOC_STOP, 0);
        }

      if (state->buffers != NULL)
        {
          for (index = 0; index < state->buffer_info.nbuffers; index++)
            {
              if (state->buffers[index] != NULL)
                {
                  memset(&state->desc, 0, sizeof(state->desc));
                  state->desc.u.buffer = state->buffers[index];
                  ioctl(state->audio_fd, AUDIOIOC_FREEBUFFER,
                        (unsigned long)(uintptr_t)&state->desc);
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

  if (state->file_fd >= 0)
    {
      close(state->file_fd);
      state->file_fd = -1;
    }

  if (state->mq != (mqd_t)-1)
    {
      mq_close(state->mq);
      mq_unlink(state->mq_name);
      state->mq = (mqd_t)-1;
    }
}

static int voice_player_prepare(FAR struct voice_player_state_s *state,
                                uint32_t rate)
{
  struct audio_caps_desc_s caps;
  struct mq_attr attr;
  unsigned int index;

  state->audio_fd = open(VOICE_PLAYER_DEVICE, O_RDWR | O_CLOEXEC);
  if (state->audio_fd < 0)
    {
      printf("[player] open %s failed: %d\n", VOICE_PLAYER_DEVICE, errno);
      return -errno;
    }

  if (ioctl(state->audio_fd, AUDIOIOC_RESERVE, 0) < 0)
    {
      printf("[player] reserve failed: %d\n", errno);
      return -errno;
    }

  memset(&caps, 0, sizeof(caps));
  caps.caps.ac_len = sizeof(caps.caps);
  caps.caps.ac_type = AUDIO_TYPE_OUTPUT;
  caps.caps.ac_subtype = AUDIO_FMT_PCM;
  caps.caps.ac_channels = 1;
  caps.caps.ac_controls.hw[0] = rate;
  caps.caps.ac_controls.b[2] = 16;  /* bits per sample */
  if (ioctl(state->audio_fd, AUDIOIOC_CONFIGURE,
            (unsigned long)(uintptr_t)&caps) < 0)
    {
      /* 采样率不被 codec 支持时这里返回 -EINVAL：ES8311 的
       * (MCLK, rate) 系数表在开机默认 4096000 下没有 24 kHz，
       * 而 MiMo TTS 固定返回 24 kHz。 */
      printf("[player] configure rate=%lu ch=%u bits=%u failed: %d\n",
             (unsigned long)rate, (unsigned)caps.caps.ac_channels,
             (unsigned)caps.caps.ac_controls.b[2], errno);
      return -errno;
    }

  printf("[player] configure ok rate=%lu ch=%u bits=%u\n",
         (unsigned long)rate, (unsigned)caps.caps.ac_channels,
         (unsigned)caps.caps.ac_controls.b[2]);

  if (ioctl(state->audio_fd, AUDIOIOC_GETBUFFERINFO,
            (unsigned long)(uintptr_t)&state->buffer_info) < 0)
    {
      state->buffer_info.buffer_size = VOICE_PLAYER_FALLBACK_BUFFER_SIZE;
      state->buffer_info.nbuffers = VOICE_PLAYER_FALLBACK_NBUFFERS;
    }

  if (state->buffer_info.nbuffers == 0 || state->buffer_info.buffer_size == 0)
    {
      return -EINVAL;
    }

  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg = state->buffer_info.nbuffers + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  snprintf(state->mq_name, sizeof(state->mq_name), "/voice_play_%ld",
           (long)getpid());
  state->mq = mq_open(state->mq_name, O_RDWR | O_CREAT, 0644, &attr);
  if (state->mq == (mqd_t)-1)
    {
      return -errno;
    }

  if (ioctl(state->audio_fd, AUDIOIOC_REGISTERMQ,
            (unsigned long)(uintptr_t)state->mq) < 0)
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
      memset(&state->desc, 0, sizeof(state->desc));
      state->desc.numbytes = state->buffer_info.buffer_size;
      state->desc.u.pbuffer = &state->buffers[index];
      if (ioctl(state->audio_fd, AUDIOIOC_ALLOCBUFFER,
                (unsigned long)(uintptr_t)&state->desc) < 0)
        {
          return -errno;
        }
    }

  return 0;
}

/* 把一个 ap_buffer 填满（或填到数据末尾）。返回填充字节数。 */
static size_t voice_player_fill(FAR struct voice_player_state_s *state,
                                FAR struct ap_buffer_s *apb)
{
  size_t bytes = apb->nmaxbytes;
  ssize_t read_ret;

  if (bytes > state->remaining)
    {
      bytes = state->remaining;
    }

  if (state->mem != NULL)
    {
      memcpy(apb->samp, state->mem, bytes);
      state->mem += bytes;
    }
  else
    {
      read_ret = read(state->file_fd, apb->samp, bytes);
      if (read_ret < 0)
        {
          return 0;
        }

      bytes = (size_t)read_ret;
    }

  apb->nbytes = bytes;
  apb->curbyte = 0;
  apb->flags = 0;
  state->remaining -= bytes;
  return bytes;
}

static int voice_player_enqueue(FAR struct voice_player_state_s *state,
                                FAR struct ap_buffer_s *apb)
{
  if (voice_player_fill(state, apb) == 0)
    {
      return -ENODATA;
    }

  memset(&state->desc, 0, sizeof(state->desc));
  state->desc.numbytes = apb->nbytes;
  state->desc.u.buffer = apb;
  if (ioctl(state->audio_fd, AUDIOIOC_ENQUEUEBUFFER,
            (unsigned long)(uintptr_t)&state->desc) < 0)
    {
      return -errno;
    }

  state->outstanding++;
  return 0;
}

static int voice_player_run(FAR struct voice_player_state_s *state)
{
  struct audio_msg_s message;
  unsigned int priority;
  unsigned int index;
  ssize_t size;
  FAR struct ap_buffer_s *apb;
  int ret = 0;

  if (ioctl(state->audio_fd, AUDIOIOC_START, 0) < 0)
    {
      printf("[player] start failed: %d\n", errno);
      return -errno;
    }

  printf("[player] start ok: bufs=%u bytes_each=%u total=%zu\n",
         (unsigned)state->buffer_info.nbuffers,
         (unsigned)state->buffer_info.buffer_size, state->remaining);

  state->started = true;

  for (index = 0; index < state->buffer_info.nbuffers &&
                  state->remaining > 0; index++)
    {
      ret = voice_player_enqueue(state, state->buffers[index]);
      if (ret < 0)
        {
          return ret < 0 && ret != -ENODATA ? ret : 0;
        }
    }

  if (state->outstanding == 0)
    {
      return -ENODATA;
    }

  while (true)
    {
      if (voice_player_aborted(state))
        {
          ret = -EINTR;
          ioctl(state->audio_fd, AUDIOIOC_STOP, 0);
          state->stopping = true;
          return ret;
        }

      size = mq_receive(state->mq, (FAR char *)&message, sizeof(message),
                        &priority);
      if (size != sizeof(message))
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (message.msg_id == AUDIO_MSG_COMPLETE)
        {
          return ret;
        }

      if (message.msg_id == AUDIO_MSG_IOERR ||
          message.msg_id == AUDIO_MSG_UNDERRUN)
        {
          ret = message.msg_id == AUDIO_MSG_IOERR && message.u.data != 0 ?
                -(int)message.u.data : -EIO;
          ioctl(state->audio_fd, AUDIOIOC_STOP, 0);
          state->stopping = true;
          return ret;
        }

      if (message.msg_id != AUDIO_MSG_DEQUEUE)
        {
          continue;
        }

      if (state->outstanding > 0)
        {
          state->outstanding--;
        }

      apb = (FAR struct ap_buffer_s *)message.u.ptr;
      if (ret == 0 && state->remaining > 0)
        {
          ret = voice_player_enqueue(state, apb);
          if (ret == -ENODATA)
            {
              ret = 0;
            }
        }

      if ((ret != 0 || state->remaining == 0) && state->outstanding == 0)
        {
          if (ioctl(state->audio_fd, AUDIOIOC_STOP, 0) < 0 && ret == 0)
            {
              ret = -errno;
            }

          state->stopping = true;
          return ret;
        }
    }
}

static int voice_player_play(FAR struct voice_player_state_s *state,
                             uint32_t rate, size_t total_bytes)
{
  int ret;

  state->remaining = total_bytes;
  ret = voice_player_prepare(state, rate);
  if (ret < 0)
    {
      goto out;
    }

  ret = voice_player_run(state);

out:
  voice_player_cleanup(state);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int smart_home_voice_player_play_mem(const uint8_t *samples, size_t bytes,
                                     uint32_t rate,
                                     smart_home_voice_abort_t *abort)
{
  struct voice_player_state_s state;
  int ret;

  if (samples == NULL || bytes == 0 || rate == 0)
    {
      return -EINVAL;
    }

  memset(&state, 0, sizeof(state));
  state.audio_fd = -1;
  state.file_fd = -1;
  state.mq = (mqd_t)-1;
  state.mem = samples;
  state.abort = abort;

  ret = voice_player_play(&state, rate, bytes);
  if (ret < 0)
    {
      fprintf(stderr, "voice_player: play_mem failed: %d (%s)\n",
              ret, strerror(-ret));
    }

  return ret;
}

int smart_home_voice_player_play_file(const char *path, uint32_t rate,
                                      smart_home_voice_abort_t *abort)
{
  struct voice_player_state_s state;
  off_t size;
  int ret;

  if (path == NULL || rate == 0)
    {
      return -EINVAL;
    }

  memset(&state, 0, sizeof(state));
  state.audio_fd = -1;
  state.mq = (mqd_t)-1;
  state.abort = abort;

  state.file_fd = open(path, O_RDONLY | O_CLOEXEC);
  if (state.file_fd < 0)
    {
      fprintf(stderr, "voice_player: open %s failed: %d (%s)\n",
              path, -errno, strerror(errno));
      return -errno;
    }

  size = lseek(state.file_fd, 0, SEEK_END);
  if (size <= 0)
    {
      close(state.file_fd);
      return -EINVAL;
    }

  lseek(state.file_fd, 0, SEEK_SET);

  ret = voice_player_play(&state, rate, (size_t)size);
  if (ret < 0)
    {
      fprintf(stderr, "voice_player: play_file %s failed: %d (%s)\n",
              path, ret, strerror(-ret));
    }

  return ret;
}
