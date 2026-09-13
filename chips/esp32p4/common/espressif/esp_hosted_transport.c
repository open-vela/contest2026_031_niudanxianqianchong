/****************************************************************************
 * chips/esp32p4/common/espressif/esp_hosted_transport.c
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

#ifdef CONFIG_ESPRESSIF_HOSTED_TRANSPORT

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/signal.h>

#include <arch/chip/esp_hosted_transport.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_HOSTED_TRANSPORT_CMD0                 0
#define ESP_HOSTED_TRANSPORT_CMD3                 3
#define ESP_HOSTED_TRANSPORT_CMD5                 5
#define ESP_HOSTED_TRANSPORT_CMD7                 7
#define ESP_HOSTED_TRANSPORT_CMD52               52

#define ESP_HOSTED_TRANSPORT_CMD5_RETRIES        20
#define ESP_HOSTED_TRANSPORT_CMD5_RETRY_DELAY_MS 20

#define ESP_HOSTED_TRANSPORT_CMD52_FUNCTION_MAX  7
#define ESP_HOSTED_TRANSPORT_CMD52_ADDRESS_MAX   0x1ffff

#define ESP_HOSTED_TRANSPORT_CCCR_IO_ENABLE      0x02
#define ESP_HOSTED_TRANSPORT_CCCR_IO_READY       0x03
#define ESP_HOSTED_TRANSPORT_FUNCTION1_BIT       0x02
#define ESP_HOSTED_TRANSPORT_FUNCTION1_BLOCK_LO  0x110
#define ESP_HOSTED_TRANSPORT_FUNCTION1_BLOCK_HI  0x111

#define ESP_HOSTED_TRANSPORT_R4_READY      UINT32_C(0x80000000)
#define ESP_HOSTED_TRANSPORT_R4_VOLTAGE    UINT32_C(0x00ff8000)

/* SDIO response status: CRC error, illegal command and general error.
 * R5 additionally reports invalid function number and out-of-range address.
 * The upper half of R6 contains the card-assigned relative address (RCA).
 */

#define ESP_HOSTED_TRANSPORT_R1_ERRORS    UINT32_C(0x00c80000)
#define ESP_HOSTED_TRANSPORT_R5_ERRORS    UINT32_C(0x0000cb00)
#define ESP_HOSTED_TRANSPORT_R6_ERRORS    UINT32_C(0x0000e000)
#define ESP_HOSTED_TRANSPORT_RCA_MASK     UINT32_C(0xffff0000)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_hosted_transport_s
{
  FAR struct esp_hosted_sdio_s *sdio;
  bool initialized;
  bool function_ready;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp_hosted_transport_s g_transport;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int esp_hosted_transport_probe_cmd5(
  FAR struct esp_hosted_sdio_s *sdio, FAR uint32_t *ocr)
{
  uint32_t response;
  uint32_t request;
  int attempt;
  int ret;

  ret = esp_hosted_sdio_command(sdio, ESP_HOSTED_TRANSPORT_CMD5, 0, true,
                                false, false, &response);
  if (ret < 0)
    {
      return ret;
    }

  request = response & ESP_HOSTED_TRANSPORT_R4_VOLTAGE;
  for (attempt = 0; attempt < ESP_HOSTED_TRANSPORT_CMD5_RETRIES; attempt++)
    {
      ret = esp_hosted_sdio_command(sdio, ESP_HOSTED_TRANSPORT_CMD5,
                                    request, true, false, false, &response);
      if (ret == OK && (response & ESP_HOSTED_TRANSPORT_R4_READY) != 0)
        {
          *ocr = response;
          return OK;
        }

      up_mdelay(ESP_HOSTED_TRANSPORT_CMD5_RETRY_DELAY_MS);
    }

  return ret < 0 ? ret : -ETIMEDOUT;
}

static int esp_hosted_transport_cmd52(
  FAR struct esp_hosted_sdio_s *sdio, bool write, uint8_t function,
  uint32_t address, uint8_t write_value, FAR uint8_t *response_value)
{
  uint32_t command_argument;
  uint32_t command_response;
  int ret;

  if (function > ESP_HOSTED_TRANSPORT_CMD52_FUNCTION_MAX ||
      address > ESP_HOSTED_TRANSPORT_CMD52_ADDRESS_MAX ||
      response_value == NULL)
    {
      return -EINVAL;
    }

  command_argument = ((uint32_t)function << 28) | (address << 9) |
                     write_value;
  if (write)
    {
      command_argument |= UINT32_C(1) << 31;
    }

  ret = esp_hosted_sdio_command(sdio, ESP_HOSTED_TRANSPORT_CMD52,
                                command_argument, true, true, false,
                                &command_response);
  if (ret < 0)
    {
      return ret;
    }

  if ((command_response & ESP_HOSTED_TRANSPORT_R5_ERRORS) != 0)
    {
      syslog(LOG_ERR, "ERROR: ESP-Hosted C6 CMD52: R5=0x%08" PRIx32 "\n",
             command_response);
      return -EIO;
    }

  *response_value = (uint8_t)command_response;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_hosted_transport_initialize(
  FAR const struct esp_hosted_transport_config_s *config,
  FAR struct esp_hosted_transport_s **transport)
{
  struct esp_hosted_sdio_status_s status;
  FAR struct esp_hosted_sdio_s *sdio;
  FAR const char *stage;
  uint32_t ocr;
  uint32_t response;
  uint32_t rca_argument;
  uint8_t cccr_revision;
  int ret;

  if (config == NULL || transport == NULL)
    {
      return -EINVAL;
    }

  *transport = NULL;
  if (g_transport.initialized)
    {
      return -EBUSY;
    }

  stage = "controller_initialize";
  ret = esp_hosted_sdio_initialize(&config->sdio, &sdio);
  if (ret < 0)
    {
      goto fail;
    }

  stage = "cmd0";
  ret = esp_hosted_sdio_command(sdio, ESP_HOSTED_TRANSPORT_CMD0, 0, false,
                                false, true, NULL);
  if (ret < 0)
    {
      goto deinitialize;
    }

  stage = "cmd5";
  ret = esp_hosted_transport_probe_cmd5(sdio, &ocr);
  if (ret < 0)
    {
      goto deinitialize;
    }

  syslog(LOG_INFO, "INFO: ESP-Hosted C6 probe: stage=cmd5 R4=0x%08"
         PRIx32 "\n", ocr);

  /* CMD5 only makes the I/O card ready.  Obtain its RCA with CMD3, then
   * select it with CMD7 before accessing CCCR using CMD52.
   */

  stage = "cmd3_rca";
  ret = esp_hosted_sdio_command(sdio, ESP_HOSTED_TRANSPORT_CMD3, 0, true,
                                true, false, &response);
  if (ret < 0)
    {
      goto deinitialize;
    }

  syslog(LOG_INFO, "INFO: ESP-Hosted C6 probe: stage=cmd3_rca R6=0x%08"
         PRIx32 "\n", response);
  rca_argument = response & ESP_HOSTED_TRANSPORT_RCA_MASK;
  if ((response & ESP_HOSTED_TRANSPORT_R6_ERRORS) != 0 || rca_argument == 0)
    {
      ret = -EIO;
      goto deinitialize;
    }

  stage = "cmd7_select";
  ret = esp_hosted_sdio_command(sdio, ESP_HOSTED_TRANSPORT_CMD7,
                                rca_argument, true, true, false, &response);
  if (ret < 0)
    {
      goto deinitialize;
    }

  syslog(LOG_INFO, "INFO: ESP-Hosted C6 probe: stage=cmd7_select R1=0x%08"
         PRIx32 "\n", response);
  if ((response & ESP_HOSTED_TRANSPORT_R1_ERRORS) != 0)
    {
      ret = -EIO;
      goto deinitialize;
    }

  stage = "cmd52_cccr";
  ret = esp_hosted_transport_cmd52(sdio, false, 0, 0, 0, &cccr_revision);
  if (ret < 0)
    {
      goto deinitialize;
    }

  stage = "controller_status";
  ret = esp_hosted_sdio_get_status(sdio, &status);
  if (ret < 0)
    {
      goto deinitialize;
    }

  g_transport.sdio = sdio;
  g_transport.initialized = true;
  *transport = &g_transport;
  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 SDIO: slot=%u width=%u clock=%" PRIu32
         "kHz revision=0x%08" PRIx32 " hardware=0x%08" PRIx32 "\n",
         (unsigned int)status.slot, (unsigned int)status.bus_width,
         status.clock_khz, status.revision, status.hardware_config);
  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 probe: CMD5 R4=0x%08" PRIx32
         " RCA=0x%04" PRIx32 " CCCR=0x%02x\n", ocr,
         rca_argument >> 16, (unsigned int)cccr_revision);
  return OK;

deinitialize:
  esp_hosted_sdio_deinitialize(sdio);
fail:
  syslog(LOG_ERR, "ERROR: ESP-Hosted C6 probe: stage=%s result=%d\n",
         stage, ret);
  return ret;
}

FAR struct esp_hosted_sdio_s *esp_hosted_transport_get_sdio(
  FAR struct esp_hosted_transport_s *transport)
{
  if (transport == NULL || !transport->initialized)
    {
      return NULL;
    }

  return transport->sdio;
}

int esp_hosted_transport_enable_function(
  FAR struct esp_hosted_transport_s *transport)
{
  uint8_t value;
  uint8_t low;
  uint8_t high;
  int attempt;
  int ret;

  if (transport != &g_transport || !transport->initialized)
    {
      return -EINVAL;
    }

  if (transport->function_ready)
    {
      return OK;
    }

  ret = esp_hosted_transport_cmd52(transport->sdio, false, 0,
                                   ESP_HOSTED_TRANSPORT_CCCR_IO_ENABLE,
                                   0, &value);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_transport_cmd52(
    transport->sdio, true, 0, ESP_HOSTED_TRANSPORT_CCCR_IO_ENABLE,
    value | ESP_HOSTED_TRANSPORT_FUNCTION1_BIT, &value);
  if (ret < 0)
    {
      return ret;
    }

  for (attempt = 0; attempt < 100; attempt++)
    {
      ret = esp_hosted_transport_cmd52(transport->sdio, false, 0,
                                       ESP_HOSTED_TRANSPORT_CCCR_IO_READY,
                                       0, &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((value & ESP_HOSTED_TRANSPORT_FUNCTION1_BIT) != 0)
        {
          break;
        }

      ret = nxsig_usleep(10000);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (attempt == 100)
    {
      return -ETIMEDOUT;
    }

  /* Function 1 FBR block size is 512 bytes.  Polling does not require
   * enabling the DAT1 interrupt; no bus-width change is made here.
   */

  ret = esp_hosted_transport_cmd52(
    transport->sdio, true, 0, ESP_HOSTED_TRANSPORT_FUNCTION1_BLOCK_LO,
    0, &value);
  if (ret == OK)
    {
      ret = esp_hosted_transport_cmd52(
        transport->sdio, true, 0, ESP_HOSTED_TRANSPORT_FUNCTION1_BLOCK_HI,
        2, &value);
    }

  if (ret == OK)
    {
      ret = esp_hosted_transport_cmd52(
        transport->sdio, false, 0, ESP_HOSTED_TRANSPORT_FUNCTION1_BLOCK_LO,
        0, &low);
    }

  if (ret == OK)
    {
      ret = esp_hosted_transport_cmd52(
        transport->sdio, false, 0, ESP_HOSTED_TRANSPORT_FUNCTION1_BLOCK_HI,
        0, &high);
    }

  if (ret < 0)
    {
      return ret;
    }

  if (low != 0 || high != 2)
    {
      return -EIO;
    }

  transport->function_ready = true;
  syslog(LOG_INFO,
         "INFO: ESP-Hosted Function 1 ready: io_ready=0x%02x"
         " block_size=%u\n", (unsigned int)value, 512);
  return OK;
}

int esp_hosted_transport_deinitialize(
  FAR struct esp_hosted_transport_s *transport)
{
  int ret;

  if (transport != &g_transport || !transport->initialized)
    {
      return -EINVAL;
    }

  ret = esp_hosted_sdio_deinitialize(transport->sdio);
  if (ret == OK)
    {
      memset(&g_transport, 0, sizeof(g_transport));
    }

  return ret;
}

int esp_hosted_transport_read_reg(
  FAR struct esp_hosted_transport_s *transport, uint32_t address,
  FAR uint8_t *value)
{
  if (transport != &g_transport || !transport->function_ready)
    {
      return -EPIPE;
    }

  return esp_hosted_transport_cmd52(transport->sdio, false, 1,
                                    address, 0, value);
}

int esp_hosted_transport_write_reg(
  FAR struct esp_hosted_transport_s *transport, uint32_t address,
  uint8_t value)
{
  uint8_t response;

  if (transport != &g_transport || !transport->function_ready)
    {
      return -EPIPE;
    }

  return esp_hosted_transport_cmd52(transport->sdio, true, 1,
                                    address, value, &response);
}

int esp_hosted_transport_transfer(
  FAR struct esp_hosted_transport_s *transport, bool write,
  uint32_t address, FAR void *buffer, size_t length, bool blocks)
{
  uint32_t count;
  uint32_t argument;

  if (transport != &g_transport || !transport->function_ready)
    {
      return -EPIPE;
    }

  if (address > 0x1ffff || length == 0 || length > 4096 ||
      (blocks && length % 512 != 0) || (!blocks && length > 512))
    {
      return -EINVAL;
    }

  count = blocks ? length / 512 : length % 512;
  argument = (UINT32_C(1) << 28) | (UINT32_C(1) << 26) |
             (address << 9) | count;
  if (write)
    {
      argument |= UINT32_C(1) << 31;
    }

  if (blocks)
    {
      argument |= UINT32_C(1) << 27;
    }

  return esp_hosted_sdio_transfer(transport->sdio, argument, buffer,
                                  length, 512);
}

#endif /* CONFIG_ESPRESSIF_HOSTED_TRANSPORT */
