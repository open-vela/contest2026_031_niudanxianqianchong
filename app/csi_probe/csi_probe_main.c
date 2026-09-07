/****************************************************************************
 * app/csi_probe/csi_probe_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Staged ESP32-P4X MIPI-CSI camera validation command.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/crc32.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arch/board/board.h>
#include <arch/chip/esp_mipi_csi.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CSI_PROBE_MAX_FRAMES            10
#define CSI_PROBE_FRAME_TIMEOUT_MS      1000
#define CSI_PROBE_BUFFER_ALIGNMENT      64

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void csi_probe_usage(FAR const char *program)
{
  fprintf(stderr,
          "Usage: %s sensor | sccb | pointer | split | write | raw [1-%d]\n",
          program, CSI_PROBE_MAX_FRAMES);
}

static void csi_probe_print_stats(
  FAR const struct esp_mipi_csi_stats_s *stats)
{
  printf("stats: frames=%" PRIu32 " dma=%" PRIu32
         " bridge(overrun=%" PRIu32 " fifo=%" PRIu32
         " discard=%" PRIu32 " size=%" PRIu32 ")\n",
         stats->frame_count, stats->dma_error_count,
         stats->bridge_overrun_count, stats->bridge_fifo_overflow_count,
         stats->bridge_discard_count, stats->bridge_frame_size_error_count);
  printf("stats: csi(ecc=%" PRIu32 " crc=%" PRIu32
         " phy=%" PRIu32 " packet=%" PRIu32 ")"
         " last(dma=0x%08" PRIx32 " bridge=0x%08" PRIx32
         " host=0x%08" PRIx32 ")\n",
         stats->csi_ecc_error_count, stats->csi_crc_error_count,
         stats->csi_phy_error_count, stats->csi_packet_error_count,
         stats->last_dma_status, stats->last_bridge_status,
         stats->last_host_status);
}

static bool csi_probe_stats_clean(
  FAR const struct esp_mipi_csi_stats_s *stats)
{
  return stats->dma_error_count == 0 &&
         stats->bridge_overrun_count == 0 &&
         stats->bridge_fifo_overflow_count == 0 &&
         stats->bridge_discard_count == 0 &&
         stats->bridge_frame_size_error_count == 0 &&
         stats->csi_ecc_error_count == 0 &&
         stats->csi_crc_error_count == 0 &&
         stats->csi_phy_error_count == 0 &&
         stats->csi_packet_error_count == 0;
}

static void csi_probe_check_frame(FAR const uint8_t *buffer, size_t bytes,
                                  FAR bool *nonzero, FAR bool *nonconstant)
{
  size_t i;
  uint8_t first;

  *nonzero = false;
  *nonconstant = false;
  first = buffer[0];

  for (i = 0; i < bytes; i++)
    {
      if (buffer[i] != 0)
        {
          *nonzero = true;
        }

      if (buffer[i] != first)
        {
          *nonconstant = true;
        }
    }
}

static int csi_probe_sensor(void)
{
  struct esp_mipi_csi_config_s config;
  FAR struct esp_mipi_csi_s *csi = NULL;
  uint16_t product_id = 0;
  bool csi_powered = false;
  int cleanup_ret;
  int ret;

  printf("=== ESP32-P4X SC2336 SCCB Probe ===\n");
  printf("control: I2C%d address=0x%02x frequency=%lu; "
         "CSI Host/ISP/DMA: disabled\n",
         BOARD_SC2336_I2C_BUS, BOARD_SC2336_I2C_ADDRESS,
         (unsigned long)BOARD_SC2336_I2C_FREQUENCY);

  ret = board_sc2336_csi_power_acquire(&config, &csi);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=phy_ldo_acquire ret=%d\n", ret);
      goto out;
    }

  csi_powered = true;
  printf("power: D-PHY LDO channel=%d voltage=%dmV ready\n",
         config.phy_ldo.channel_id, config.phy_ldo.voltage_mv);
  ret = board_sc2336_probe(&product_id);
  if (ret < 0)
    {
      fprintf(stderr,
              "csi_probe: FAIL step=sc2336_product_id ret=%d\n", ret);
      goto out;
    }

  printf("csi_probe: PASS sensor=SC2336 product_id=0x%04x\n",
         product_id);
  ret = OK;

out:
  if (csi_powered)
    {
      cleanup_ret = board_sc2336_csi_power_release(csi);
      if (ret >= 0 && cleanup_ret < 0)
        {
          ret = cleanup_ret;
        }
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int csi_probe_sccb(void)
{
  struct esp_mipi_csi_config_s config;
  FAR struct esp_mipi_csi_s *csi = NULL;
  uint16_t first_product_id = 0;
  uint16_t second_product_id = 0;
  bool csi_powered = false;
  int cleanup_ret;
  int ret;

  printf("=== ESP32-P4X SC2336 SCCB Read-Write Diagnostic ===\n");
  printf("sequence: read product ID -> write 0x0100=0x00 -> "
         "read product ID; CSI Host/ISP/DMA: disabled\n");

  ret = board_sc2336_csi_power_acquire(&config, &csi);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=phy_ldo_acquire ret=%d\n", ret);
      goto out;
    }

  csi_powered = true;
  printf("power: D-PHY LDO channel=%d voltage=%dmV ready\n",
         config.phy_ldo.channel_id, config.phy_ldo.voltage_mv);
  ret = board_sc2336_sccb_read_write_test(&first_product_id,
                                           &second_product_id);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=sccb_read_write ret=%d\n", ret);
      goto out;
    }

  printf("csi_probe: PASS sccb first_product_id=0x%04x "
         "second_product_id=0x%04x\n", first_product_id,
         second_product_id);
  ret = OK;

out:
  if (csi_powered)
    {
      cleanup_ret = board_sc2336_csi_power_release(csi);
      if (ret >= 0 && cleanup_ret < 0)
        {
          ret = cleanup_ret;
        }
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int csi_probe_pointer(void)
{
  struct esp_mipi_csi_config_s config;
  FAR struct esp_mipi_csi_s *csi = NULL;
  bool csi_powered = false;
  int cleanup_ret;
  int ret;

  printf("=== ESP32-P4X SC2336 SCCB Pointer Diagnostic ===\n");
  printf("sequence: write register address 0x3107 only; "
         "no read and no control-register write; "
         "CSI Host/ISP/DMA: disabled\n");

  ret = board_sc2336_csi_power_acquire(&config, &csi);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=phy_ldo_acquire ret=%d\n", ret);
      goto out;
    }

  csi_powered = true;
  printf("power: D-PHY LDO channel=%d voltage=%dmV ready\n",
         config.phy_ldo.channel_id, config.phy_ldo.voltage_mv);
  ret = board_sc2336_sccb_pointer_test();
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=sccb_pointer ret=%d\n", ret);
      goto out;
    }

  printf("csi_probe: PASS sccb_pointer register=0x3107\n");
  ret = OK;

out:
  if (csi_powered)
    {
      cleanup_ret = board_sc2336_csi_power_release(csi);
      if (ret >= 0 && cleanup_ret < 0)
        {
          ret = cleanup_ret;
        }
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int csi_probe_split(void)
{
  struct esp_mipi_csi_config_s config;
  FAR struct esp_mipi_csi_s *csi = NULL;
  uint8_t value = 0;
  bool csi_powered = false;
  int cleanup_ret;
  int ret;

  printf("=== ESP32-P4X SC2336 SCCB Split Read Diagnostic ===\n");
  printf("sequence: pointer write 0x3107 -> STOP -> read one byte; "
         "no control-register write; CSI Host/ISP/DMA: disabled\n");

  ret = board_sc2336_csi_power_acquire(&config, &csi);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=phy_ldo_acquire ret=%d\n", ret);
      goto out;
    }

  csi_powered = true;
  printf("power: D-PHY LDO channel=%d voltage=%dmV ready\n",
         config.phy_ldo.channel_id, config.phy_ldo.voltage_mv);
  ret = board_sc2336_sccb_split_test(&value);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=sccb_split ret=%d\n", ret);
      goto out;
    }

  printf("csi_probe: PASS sccb_split register=0x3107 value=0x%02x\n",
         value);
  ret = OK;

out:
  if (csi_powered)
    {
      cleanup_ret = board_sc2336_csi_power_release(csi);
      if (ret >= 0 && cleanup_ret < 0)
        {
          ret = cleanup_ret;
        }
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int csi_probe_write(void)
{
  struct esp_mipi_csi_config_s config;
  FAR struct esp_mipi_csi_s *csi = NULL;
  uint16_t product_id = 0;
  bool csi_powered = false;
  int cleanup_ret;
  int ret;

  printf("=== ESP32-P4X SC2336 SCCB Write-First Diagnostic ===\n");
  printf("sequence: write 0x0103=0x01 -> wait 5ms -> write "
         "0x0100=0x00 -> read product ID; CSI Host/ISP/DMA: disabled\n");

  ret = board_sc2336_csi_power_acquire(&config, &csi);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=phy_ldo_acquire ret=%d\n", ret);
      goto out;
    }

  csi_powered = true;
  printf("power: D-PHY LDO channel=%d voltage=%dmV ready\n",
         config.phy_ldo.channel_id, config.phy_ldo.voltage_mv);
  ret = board_sc2336_sccb_write_test(&product_id);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=sccb_write ret=%d\n", ret);
      goto out;
    }

  printf("csi_probe: PASS sccb_write product_id=0x%04x\n", product_id);
  ret = OK;

out:
  if (csi_powered)
    {
      cleanup_ret = board_sc2336_csi_power_release(csi);
      if (ret >= 0 && cleanup_ret < 0)
        {
          ret = cleanup_ret;
        }
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int csi_probe_raw(unsigned int requested_frames)
{
  struct esp_mipi_csi_config_s config;
  struct esp_mipi_csi_stats_s stats;
  FAR struct esp_mipi_csi_s *csi = NULL;
  FAR uint8_t *buffer = NULL;
  uint16_t product_id = 0;
  size_t frame_bytes;
  uint32_t crc = 0;
  bool nonzero = false;
  bool nonconstant = false;
  bool csi_powered = false;
  bool csi_running = false;
  bool sensor_streaming = false;
  bool csi_initialized = false;
  unsigned int frame;
  int cleanup_ret;
  int ret;

  printf("=== ESP32-P4X SC2336 CSI RAW Capture ===\n");
  printf("request: frames=%u timeout=%ums\n", requested_frames,
         CSI_PROBE_FRAME_TIMEOUT_MS);

  ret = board_sc2336_csi_power_acquire(&config, &csi);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=phy_ldo_acquire ret=%d\n", ret);
      goto out;
    }

  csi_powered = true;
  printf("power: D-PHY LDO channel=%d voltage=%dmV ready\n",
         config.phy_ldo.channel_id, config.phy_ldo.voltage_mv);
  frame_bytes = (size_t)config.width * config.height *
                config.bits_per_pixel / 8;
  printf("profile: sensor=SC2336 lanes=%u dt=0x%02x "
         "RAW%u %ux%u %uMbps/lane frame_bytes=%zu\n",
         config.lane_num, config.data_type,
         config.bits_per_pixel, config.width, config.height,
         config.lane_bit_rate_mbps, frame_bytes);
  ret = board_sc2336_csi_prepare(&product_id);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=sc2336_profile ret=%d\n", ret);
      goto out;
    }

  printf("sensor: SC2336 product_id=0x%04x profile configured\n",
         product_id);
  buffer = memalign(CSI_PROBE_BUFFER_ALIGNMENT, frame_bytes);
  if (buffer == NULL)
    {
      ret = -ENOMEM;
      fprintf(stderr, "csi_probe: FAIL step=frame_buffer ret=%d bytes=%zu\n",
              ret, frame_bytes);
      goto out;
    }

  memset(buffer, 0, frame_bytes);
  ret = board_sc2336_csi_set_stream(true);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=sensor_stream_on ret=%d\n", ret);
      goto out;
    }

  sensor_streaming = true;
  ret = board_sc2336_csi_initialize(csi, &config);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=csi_initialize ret=%d\n", ret);
      goto out;
    }

  csi_initialized = true;
  ret = esp_mipi_csi_start(csi, buffer, frame_bytes);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=csi_start ret=%d\n", ret);
      goto out;
    }

  csi_running = true;
  for (frame = 0; frame < requested_frames; frame++)
    {
      ret = esp_mipi_csi_wait_frame(csi, CSI_PROBE_FRAME_TIMEOUT_MS);
      if (ret < 0)
        {
          fprintf(stderr,
                  "csi_probe: FAIL step=wait_frame frame=%u ret=%d\n",
                  frame + 1, ret);
          goto out;
        }
    }

  ret = esp_mipi_csi_stop(csi);
  csi_running = false;
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=csi_stop ret=%d\n", ret);
      goto out;
    }

  ret = esp_mipi_csi_buffer_sync_for_cpu(buffer, frame_bytes);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=buffer_sync ret=%d\n", ret);
      goto out;
    }

  csi_probe_check_frame(buffer, frame_bytes, &nonzero, &nonconstant);
  crc = crc32(buffer, frame_bytes);
  ret = esp_mipi_csi_get_stats(csi, &stats);
  if (ret < 0)
    {
      fprintf(stderr, "csi_probe: FAIL step=csi_stats ret=%d\n", ret);
      goto out;
    }

  printf("frame: bytes=%zu crc32=0x%08" PRIx32
         " nonzero=%s nonconstant=%s\n",
         frame_bytes, crc, nonzero ? "yes" : "no",
         nonconstant ? "yes" : "no");
  csi_probe_print_stats(&stats);

  if (stats.frame_count < requested_frames || !nonzero || !nonconstant ||
      !csi_probe_stats_clean(&stats))
    {
      fprintf(stderr, "csi_probe: FAIL step=frame_validation\n");
      ret = -EIO;
      goto out;
    }

  printf("csi_probe: PASS raw frames=%" PRIu32 " crc32=0x%08" PRIx32
         "\n", stats.frame_count, crc);
  ret = OK;

out:
  if (csi_running)
    {
      cleanup_ret = esp_mipi_csi_stop(csi);
      if (ret >= 0 && cleanup_ret < 0)
        {
          ret = cleanup_ret;
        }
    }

  if (csi_initialized)
    {
      cleanup_ret = board_sc2336_csi_deinitialize(csi);
      if (ret >= 0 && cleanup_ret < 0)
        {
          ret = cleanup_ret;
        }
    }

  if (sensor_streaming)
    {
      cleanup_ret = board_sc2336_csi_set_stream(false);
      if (ret >= 0 && cleanup_ret < 0)
        {
          ret = cleanup_ret;
        }
    }

  cleanup_ret = board_sc2336_csi_release();
  if (ret >= 0 && cleanup_ret < 0)
    {
      ret = cleanup_ret;
    }

  if (csi_powered)
    {
      cleanup_ret = board_sc2336_csi_power_release(csi);
      if (ret >= 0 && cleanup_ret < 0)
        {
          ret = cleanup_ret;
        }
    }

  free(buffer);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int csi_probe_parse_frame_count(FAR const char *arg,
                                       FAR unsigned int *frame_count)
{
  FAR char *end;
  unsigned long count;

  if (arg == NULL || frame_count == NULL)
    {
      return -EINVAL;
    }

  errno = 0;
  count = strtoul(arg, &end, 10);
  if (errno != 0 || *arg == '\0' || *end != '\0' || count == 0 ||
      count > CSI_PROBE_MAX_FRAMES)
    {
      return -EINVAL;
    }

  *frame_count = (unsigned int)count;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  unsigned int frame_count = 1;

  if (argc == 2 && strcmp(argv[1], "sensor") == 0)
    {
      return csi_probe_sensor();
    }

  if (argc == 2 && strcmp(argv[1], "sccb") == 0)
    {
      return csi_probe_sccb();
    }

  if (argc == 2 && strcmp(argv[1], "pointer") == 0)
    {
      return csi_probe_pointer();
    }

  if (argc == 2 && strcmp(argv[1], "split") == 0)
    {
      return csi_probe_split();
    }

  if (argc == 2 && strcmp(argv[1], "write") == 0)
    {
      return csi_probe_write();
    }

  if (argc == 2 && strcmp(argv[1], "raw") == 0)
    {
      return csi_probe_raw(frame_count);
    }

  if (argc == 3 && strcmp(argv[1], "raw") == 0 &&
      csi_probe_parse_frame_count(argv[2], &frame_count) == OK)
    {
      return csi_probe_raw(frame_count);
    }

  csi_probe_usage(argv[0]);
  return EXIT_FAILURE;
}
