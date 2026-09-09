/****************************************************************************
 * chips/esp32p4/common/espressif/esp_mipi_csi_video.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/kmalloc.h>

#include <errno.h>
#include <string.h>
#include <sys/time.h>

#include <arch/chip/esp_mipi_csi_video.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_MIPI_CSI_VIDEO_WIDTH       1024
#define ESP_MIPI_CSI_VIDEO_HEIGHT       600
#define ESP_MIPI_CSI_VIDEO_FRAME_BYTES \
  (ESP_MIPI_CSI_VIDEO_WIDTH * ESP_MIPI_CSI_VIDEO_HEIGHT * 2)
#define ESP_MIPI_CSI_VIDEO_ALIGNMENT      64

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void esp_mipi_csi_video_free_dma_buffers(
  FAR struct esp_mipi_csi_video_s *video)
{
  unsigned int i;

  for (i = 0; i < ESP_MIPI_CSI_VIDEO_DMA_BUFFERS; i++)
    {
      kumm_free(video->dma_buffers[i]);
      video->dma_buffers[i] = NULL;
    }
}

static int esp_mipi_csi_video_allocate_dma_buffers(
  FAR struct esp_mipi_csi_video_s *video)
{
  unsigned int i;

  if (video->dma_buffers[0] != NULL)
    {
      return OK;
    }

  for (i = 0; i < ESP_MIPI_CSI_VIDEO_DMA_BUFFERS; i++)
    {
      video->dma_buffers[i] = kumm_memalign(ESP_MIPI_CSI_VIDEO_ALIGNMENT,
                                             video->bytes);
      if (video->dma_buffers[i] == NULL)
        {
          esp_mipi_csi_video_free_dma_buffers(video);
          return -ENOMEM;
        }
    }

  return OK;
}

static void esp_mipi_csi_video_done(FAR void *buffer, size_t bytes,
                                    FAR void *arg)
{
  FAR struct esp_mipi_csi_video_s *video = arg;
  imgdata_capture_t callback;
  FAR void *callback_arg;
  FAR uint8_t *v4l2_buffer;
  struct timeval timestamp;
  irqstate_t flags;
  bool requeue;

  flags = spin_lock_irqsave(&video->lock);
  callback = video->callback;
  callback_arg = video->callback_arg;
  v4l2_buffer = video->v4l2_buffer;
  video->v4l2_buffer = NULL;
  requeue = video->streaming;
  spin_unlock_irqrestore(&video->lock, flags);

  if (v4l2_buffer != NULL && callback != NULL)
    {
      memcpy(v4l2_buffer, buffer, bytes);
      gettimeofday(&timestamp, NULL);
      callback(0, bytes, &timestamp, callback_arg);
    }

  flags = spin_lock_irqsave(&video->lock);
  requeue = requeue && video->streaming;
  spin_unlock_irqrestore(&video->lock, flags);
  if (requeue)
    {
      esp_mipi_csi_queue_buffer(video->csi, buffer, bytes);
    }
}

static int esp_mipi_csi_video_init(FAR struct imgdata_s *data)
{
  return OK;
}

static int esp_mipi_csi_video_uninit(FAR struct imgdata_s *data)
{
  FAR struct esp_mipi_csi_video_s *video = (FAR void *)data;
  irqstate_t flags;
  bool streaming;

  flags = spin_lock_irqsave(&video->lock);
  streaming = video->streaming;
  video->streaming = false;
  video->v4l2_buffer = NULL;
  spin_unlock_irqrestore(&video->lock, flags);
  if (streaming)
    {
      esp_mipi_csi_stop(video->csi);
    }

  if (video->csi != NULL)
    {
      esp_mipi_csi_wait_video_idle(video->csi);
    }

  esp_mipi_csi_video_free_dma_buffers(video);
  return OK;
}

static int esp_mipi_csi_video_set_buf(FAR struct imgdata_s *data,
                                      uint8_t nr_datafmts,
                                      FAR imgdata_format_t *datafmts,
                                      FAR uint8_t *addr, uint32_t size)
{
  FAR struct esp_mipi_csi_video_s *video = (FAR void *)data;
  irqstate_t flags;
  int ret;

  if (nr_datafmts != 1 || datafmts == NULL ||
      datafmts[0].pixelformat != IMGDATA_PIX_FMT_RGB565 ||
      addr == NULL || size != ESP_MIPI_CSI_VIDEO_FRAME_BYTES ||
      ((uintptr_t)addr & (ESP_MIPI_CSI_VIDEO_ALIGNMENT - 1)) != 0)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&video->lock);
  if (video->v4l2_buffer != NULL)
    {
      ret = -EBUSY;
    }
  else
    {
      video->v4l2_buffer = addr;
      video->bytes = size;
      ret = OK;
    }

  spin_unlock_irqrestore(&video->lock, flags);
  return ret;
}

static int esp_mipi_csi_video_validate(FAR struct imgdata_s *data,
                                       uint8_t nr_datafmts,
                                       FAR imgdata_format_t *datafmts,
                                       FAR imgdata_interval_t *interval)
{
  if (nr_datafmts != 1 || datafmts == NULL || interval == NULL ||
      datafmts[0].pixelformat != IMGDATA_PIX_FMT_RGB565 ||
      datafmts[0].width != ESP_MIPI_CSI_VIDEO_WIDTH ||
      datafmts[0].height != ESP_MIPI_CSI_VIDEO_HEIGHT ||
      interval->numerator != 1 || interval->denominator != 30)
    {
      return -ENOTSUP;
    }

  return OK;
}

static int esp_mipi_csi_video_start(FAR struct imgdata_s *data,
                                    uint8_t nr_datafmts,
                                    FAR imgdata_format_t *datafmts,
                                    FAR imgdata_interval_t *interval,
                                    imgdata_capture_t callback, FAR void *arg)
{
  FAR struct esp_mipi_csi_video_s *video = (FAR void *)data;
  irqstate_t flags;
  int ret;

  ret = esp_mipi_csi_video_validate(data, nr_datafmts, datafmts, interval);
  if (ret < 0 || video->csi == NULL || callback == NULL)
    {
      return ret < 0 ? ret : -EINVAL;
    }

  flags = spin_lock_irqsave(&video->lock);
  if (video->streaming || video->v4l2_buffer == NULL)
    {
      ret = -EBUSY;
    }
  else
    {
      video->callback = callback;
      video->callback_arg = arg;
      video->streaming = true;
      ret = OK;
    }

  spin_unlock_irqrestore(&video->lock, flags);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_mipi_csi_video_allocate_dma_buffers(video);
  if (ret >= 0)
    {
      ret = esp_mipi_csi_queue_buffer(video->csi, video->dma_buffers[1],
                                      video->bytes);
    }

  if (ret >= 0)
    {
      ret = esp_mipi_csi_queue_buffer(video->csi, video->dma_buffers[2],
                                      video->bytes);
    }

  if (ret >= 0)
    {
      ret = esp_mipi_csi_start_video(video->csi, video->dma_buffers[0],
                                     video->bytes,
                                     esp_mipi_csi_video_done, video);
    }

  if (ret < 0)
    {
      flags = spin_lock_irqsave(&video->lock);
      video->streaming = false;
      video->callback = NULL;
      video->callback_arg = NULL;
      spin_unlock_irqrestore(&video->lock, flags);
    }

  return ret;
}

static int esp_mipi_csi_video_stop(FAR struct imgdata_s *data)
{
  FAR struct esp_mipi_csi_video_s *video = (FAR void *)data;
  irqstate_t flags;
  int ret;

  flags = spin_lock_irqsave(&video->lock);
  video->streaming = false;
  video->v4l2_buffer = NULL;
  spin_unlock_irqrestore(&video->lock, flags);

  ret = esp_mipi_csi_stop(video->csi);
  return ret == -EPIPE ? OK : ret;
}

static FAR void *esp_mipi_csi_video_alloc(FAR struct imgdata_s *data,
                                          uint32_t align_size, uint32_t size)
{
  uint32_t alignment = ESP_MIPI_CSI_VIDEO_ALIGNMENT;

  (void)data;
  if (align_size > alignment)
    {
      alignment = align_size;
    }

  return kumm_memalign(alignment, size);
}

static void esp_mipi_csi_video_free(FAR struct imgdata_s *data, void *addr)
{
  (void)data;
  kumm_free(addr);
}

static const struct imgdata_ops_s g_esp_mipi_csi_video_ops =
{
  .init = esp_mipi_csi_video_init,
  .uninit = esp_mipi_csi_video_uninit,
  .set_buf = esp_mipi_csi_video_set_buf,
  .validate_frame_setting = esp_mipi_csi_video_validate,
  .start_capture = esp_mipi_csi_video_start,
  .stop_capture = esp_mipi_csi_video_stop,
  .alloc = esp_mipi_csi_video_alloc,
  .free = esp_mipi_csi_video_free,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_mipi_csi_video_initialize(FAR struct esp_mipi_csi_video_s *video,
                                  FAR struct esp_mipi_csi_s *csi)
{
  if (video == NULL)
    {
      return -EINVAL;
    }

  memset(video, 0, sizeof(*video));
  video->data.ops = &g_esp_mipi_csi_video_ops;
  video->csi = csi;
  video->lock = SP_UNLOCKED;
  return OK;
}

void esp_mipi_csi_video_set_csi(FAR struct esp_mipi_csi_video_s *video,
                                FAR struct esp_mipi_csi_s *csi)
{
  if (video != NULL)
    {
      video->csi = csi;
    }
}
