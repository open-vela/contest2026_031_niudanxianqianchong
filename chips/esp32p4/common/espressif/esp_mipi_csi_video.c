/****************************************************************************
 * chips/esp32p4/common/espressif/esp_mipi_csi_video.c
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/kmalloc.h>

#include <errno.h>
#include <string.h>
#include <sys/time.h>

#include <arch/chip/esp_mipi_csi_video.h>

#define ESP_MIPI_CSI_VIDEO_WIDTH       1024
#define ESP_MIPI_CSI_VIDEO_HEIGHT       600
#define ESP_MIPI_CSI_VIDEO_FRAME_BYTES \
  (ESP_MIPI_CSI_VIDEO_WIDTH * ESP_MIPI_CSI_VIDEO_HEIGHT * 2)
#define ESP_MIPI_CSI_VIDEO_ALIGNMENT      64

static void esp_mipi_csi_video_done(FAR void *buffer, size_t bytes,
                                    FAR void *arg)
{
  FAR struct esp_mipi_csi_video_s *video = arg;
  struct timeval timestamp;

  (void)buffer;
  gettimeofday(&timestamp, NULL);
  if (video->callback != NULL)
    {
      video->callback(0, bytes, &timestamp, video->callback_arg);
    }
}

static int esp_mipi_csi_video_init(FAR struct imgdata_s *data)
{
  return OK;
}

static int esp_mipi_csi_video_uninit(FAR struct imgdata_s *data)
{
  FAR struct esp_mipi_csi_video_s *video = (FAR void *)data;

  if (video->streaming)
    {
      esp_mipi_csi_stop(video->csi);
      video->streaming = false;
    }

  return OK;
}

static int esp_mipi_csi_video_set_buf(FAR struct imgdata_s *data,
                                      uint8_t nr_datafmts,
                                      FAR imgdata_format_t *datafmts,
                                      FAR uint8_t *addr, uint32_t size)
{
  FAR struct esp_mipi_csi_video_s *video = (FAR void *)data;

  if (nr_datafmts != 1 || datafmts[0].pixelformat != IMGDATA_PIX_FMT_RGB565 ||
      addr == NULL || size != ESP_MIPI_CSI_VIDEO_FRAME_BYTES ||
      ((uintptr_t)addr & (ESP_MIPI_CSI_VIDEO_ALIGNMENT - 1)) != 0)
    {
      return -EINVAL;
    }

  if (video->streaming)
    {
      return esp_mipi_csi_queue_buffer(video->csi, addr, size);
    }

  video->buffer = addr;
  video->bytes = size;
  return OK;
}

static int esp_mipi_csi_video_validate(FAR struct imgdata_s *data,
                                       uint8_t nr_datafmts,
                                       FAR imgdata_format_t *datafmts,
                                       FAR imgdata_interval_t *interval)
{
  return nr_datafmts == 1 &&
         datafmts[0].pixelformat == IMGDATA_PIX_FMT_RGB565 &&
         datafmts[0].width == ESP_MIPI_CSI_VIDEO_WIDTH &&
         datafmts[0].height == ESP_MIPI_CSI_VIDEO_HEIGHT ? OK : -ENOTSUP;
}

static int esp_mipi_csi_video_start(FAR struct imgdata_s *data,
                                    uint8_t nr_datafmts,
                                    FAR imgdata_format_t *datafmts,
                                    FAR imgdata_interval_t *interval,
                                    imgdata_capture_t callback, FAR void *arg)
{
  FAR struct esp_mipi_csi_video_s *video = (FAR void *)data;
  int ret;

  if (video->csi == NULL || video->buffer == NULL || callback == NULL)
    {
      return -EINVAL;
    }

  video->callback = callback;
  video->callback_arg = arg;
  ret = esp_mipi_csi_start_video(video->csi, video->buffer, video->bytes,
                                 esp_mipi_csi_video_done, video);
  if (ret >= 0)
    {
      video->streaming = true;
    }

  return ret;
}

static int esp_mipi_csi_video_stop(FAR struct imgdata_s *data)
{
  FAR struct esp_mipi_csi_video_s *video = (FAR void *)data;
  int ret;

  video->streaming = false;
  ret = esp_mipi_csi_stop(video->csi);
  return ret == -EPIPE ? OK : ret;
}

static FAR void *esp_mipi_csi_video_alloc(FAR struct imgdata_s *data,
                                          uint32_t align_size, uint32_t size)
{
  (void)data;
  (void)align_size;
  return kumm_memalign(ESP_MIPI_CSI_VIDEO_ALIGNMENT, size);
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
