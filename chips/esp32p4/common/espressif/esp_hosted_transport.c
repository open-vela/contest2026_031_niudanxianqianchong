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

#define ESP_HOSTED_TRANSPORT_SLC_INT_RAW          0x050
#define ESP_HOSTED_TRANSPORT_SLC_PACKET_LEN       0x060
#define ESP_HOSTED_TRANSPORT_SLC_INT_CLR          0x0d4
#define ESP_HOSTED_TRANSPORT_SLC_HOST_INTR        0x08c
#define ESP_HOSTED_TRANSPORT_SLC_TOKEN_RDATA      0x044
#define ESP_HOSTED_TRANSPORT_SLC_FIFO_END         0x1f800

#define ESP_HOSTED_TRANSPORT_SLC_NEW_PACKET       UINT32_C(0x00800000)
#define ESP_HOSTED_TRANSPORT_SLC_PACKET_MASK      UINT32_C(0x000fffff)
#define ESP_HOSTED_TRANSPORT_LEN_OFFSET   16
#define ESP_HOSTED_TRANSPORT_SLC_REGISTER_BYTES   20
#define ESP_HOSTED_TRANSPORT_INIT_PACKET_MAX      256
#define ESP_HOSTED_TRANSPORT_INIT_WAIT_RETRIES    100
#define ESP_HOSTED_TRANSPORT_INIT_WAIT_US       10000
#define ESP_HOSTED_TRANSPORT_TX_WAIT_RETRIES       10
#define ESP_HOSTED_TRANSPORT_TX_WAIT_US           400
#define ESP_HOSTED_TRANSPORT_DIAG_SAMPLES           3
#define ESP_HOSTED_TRANSPORT_DIAG_INTERVAL_US  100000

#define ESP_HOSTED_TRANSPORT_IF_PRIVATE           5
#define ESP_HOSTED_TRANSPORT_PACKET_EVENT      0x33
#define ESP_HOSTED_TRANSPORT_EVENT_INIT        0x22
#define ESP_HOSTED_TRANSPORT_HEADER_BYTES        12

#define ESP_HOSTED_TRANSPORT_TAG_CAPABILITY    0x11
#define ESP_HOSTED_TRANSPORT_TAG_CHIP_ID       0x12
#define ESP_HOSTED_TRANSPORT_TAG_RX_QUEUE      0x14
#define ESP_HOSTED_TRANSPORT_TAG_TX_QUEUE      0x15
#define ESP_HOSTED_TRANSPORT_TAG_FIRMWARE      0x17
#define ESP_HOSTED_TRANSPORT_CHIP_ESP32C6      0x0d
#define ESP_HOSTED_TRANSPORT_CAP_WLAN_SDIO     0x01
#define ESP_HOSTED_TRANSPORT_CAP_CHECKSUM       0x80

#define ESP_HOSTED_TRANSPORT_CONFIG_TAG_HOST_CAP 0x44
#define ESP_HOSTED_TRANSPORT_CONFIG_TAG_CHIP_ID  0x45
#define ESP_HOSTED_TRANSPORT_CONFIG_TAG_RAW_TP   0x46
#define ESP_HOSTED_TRANSPORT_CONFIG_TAG_HIGH     0x47
#define ESP_HOSTED_TRANSPORT_CONFIG_TAG_LOW      0x48
#define ESP_HOSTED_TRANSPORT_CONFIG_TLVS            5
#define ESP_HOSTED_TRANSPORT_CONFIG_TLV_BYTES       3
#define ESP_HOSTED_TRANSPORT_CONFIG_EVENT_BYTES \
  (2 + ESP_HOSTED_TRANSPORT_CONFIG_TLVS * \
   ESP_HOSTED_TRANSPORT_CONFIG_TLV_BYTES)
#define ESP_HOSTED_TRANSPORT_CONFIG_PACKET_BYTES \
  (ESP_HOSTED_TRANSPORT_HEADER_BYTES + \
   ESP_HOSTED_TRANSPORT_CONFIG_EVENT_BYTES)

#define ESP_HOSTED_TRANSPORT_TX_TOKEN_MASK      0x0fff
#define ESP_HOSTED_TRANSPORT_TX_TOKEN_MAX       0x1000
#define ESP_HOSTED_TRANSPORT_TX_BUFFER_BYTES     1536

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
  bool data_path_open;
  uint32_t rx_packet_count;
  uint16_t tx_buffer_count;
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

static uint16_t esp_hosted_transport_get_le16(FAR const uint8_t *value)
{
  return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static void esp_hosted_transport_put_le16(FAR uint8_t *value,
                                          uint16_t number)
{
  value[0] = (uint8_t)number;
  value[1] = (uint8_t)(number >> 8);
}

static uint32_t esp_hosted_transport_get_le32(FAR const uint8_t *value)
{
  return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
         ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static void esp_hosted_transport_put_le32(FAR uint8_t *value,
                                          uint32_t number)
{
  value[0] = (uint8_t)number;
  value[1] = (uint8_t)(number >> 8);
  value[2] = (uint8_t)(number >> 16);
  value[3] = (uint8_t)(number >> 24);
}

static int esp_hosted_transport_clear_interrupts(
  FAR struct esp_hosted_transport_s *transport, uint32_t interrupts)
{
  uint8_t value[sizeof(interrupts)];

  esp_hosted_transport_put_le32(value, interrupts);
  return esp_hosted_transport_transfer(transport, true,
                                       ESP_HOSTED_TRANSPORT_SLC_INT_CLR,
                                       value, sizeof(value), false);
}

static int esp_hosted_transport_read_registers(
  FAR struct esp_hosted_transport_s *transport, FAR uint8_t *registers)
{
  int ret;

  /* Although the ESP-Hosted reference implementation can read this whole
   * window with one auto-increment CMD53, our multiword PIO capture has
   * not passed validation.  The three values used by the protocol have
   * each been verified through a four-byte CMD53.
   * Keep their original offsets so callers retain the reference layout.
   */

  memset(registers, 0, ESP_HOSTED_TRANSPORT_SLC_REGISTER_BYTES);
  ret = esp_hosted_transport_transfer(
    transport, false, ESP_HOSTED_TRANSPORT_SLC_INT_RAW, registers,
    sizeof(uint32_t), false);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_transport_transfer(
    transport, false, ESP_HOSTED_TRANSPORT_SLC_INT_RAW + 8, registers + 8,
    sizeof(uint32_t), false);
  if (ret < 0)
    {
      return ret;
    }

  return esp_hosted_transport_transfer(
    transport, false, ESP_HOSTED_TRANSPORT_SLC_PACKET_LEN, registers +
    ESP_HOSTED_TRANSPORT_LEN_OFFSET, sizeof(uint32_t), false);
}

static int esp_hosted_transport_read_fifo(
  FAR struct esp_hosted_transport_s *transport, FAR uint8_t *buffer,
  size_t length)
{
  bool blocks = length >= 512 && length % 512 == 0;

  return esp_hosted_transport_transfer(transport, false,
                                       ESP_HOSTED_TRANSPORT_SLC_FIFO_END -
                                       length, buffer, length, blocks);
}

static uint16_t esp_hosted_transport_checksum(FAR const uint8_t *packet,
                                              size_t length)
{
  uint16_t checksum = 0;
  size_t index;

  for (index = 0; index < length; index++)
    {
      checksum += packet[index];
    }

  return checksum;
}

static int esp_hosted_transport_wait_tx_buffers(
  FAR struct esp_hosted_transport_s *transport, unsigned int needed)
{
  uint8_t token_bytes[sizeof(uint32_t)];
  uint32_t token;
  unsigned int available;
  int attempt;
  int ret;

  for (attempt = 0; attempt < ESP_HOSTED_TRANSPORT_TX_WAIT_RETRIES;
       attempt++)
    {
      ret = esp_hosted_transport_transfer(
        transport, false, ESP_HOSTED_TRANSPORT_SLC_TOKEN_RDATA, token_bytes,
        sizeof(token_bytes), false);
      if (ret < 0)
        {
          return ret;
        }

      token = esp_hosted_transport_get_le32(token_bytes);
      available = ((token >> 16) & ESP_HOSTED_TRANSPORT_TX_TOKEN_MASK) +
                  ESP_HOSTED_TRANSPORT_TX_TOKEN_MAX -
                  transport->tx_buffer_count;
      available %= ESP_HOSTED_TRANSPORT_TX_TOKEN_MAX;
      if (available >= needed)
        {
          syslog(LOG_INFO,
                 "INFO: ESP-Hosted C6 TX buffers: token=0x%08" PRIx32
                 " available=%u needed=%u\n", token, available, needed);
          return OK;
        }

      ret = nxsig_usleep(ESP_HOSTED_TRANSPORT_TX_WAIT_US);
      if (ret < 0)
        {
          return ret;
        }
    }

  return -EAGAIN;
}

static int esp_hosted_transport_send_init_config(
  FAR struct esp_hosted_transport_s *transport,
  FAR const struct esp_hosted_init_info_s *info)
{
  uint8_t packet[ESP_HOSTED_TRANSPORT_CONFIG_PACKET_BYTES];
  uint8_t *event;
  uint8_t *tlv;
  unsigned int buffers;
  int ret;

  buffers = (sizeof(packet) + ESP_HOSTED_TRANSPORT_TX_BUFFER_BYTES - 1) /
            ESP_HOSTED_TRANSPORT_TX_BUFFER_BYTES;
  ret = esp_hosted_transport_wait_tx_buffers(transport, buffers);
  if (ret < 0)
    {
      return ret;
    }

  memset(packet, 0, sizeof(packet));
  packet[0] = ESP_HOSTED_TRANSPORT_IF_PRIVATE;
  esp_hosted_transport_put_le16(packet + 2,
                                ESP_HOSTED_TRANSPORT_CONFIG_EVENT_BYTES);
  esp_hosted_transport_put_le16(packet + 4,
                                ESP_HOSTED_TRANSPORT_HEADER_BYTES);
  packet[11] = ESP_HOSTED_TRANSPORT_PACKET_EVENT;

  event = packet + ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  event[0] = ESP_HOSTED_TRANSPORT_EVENT_INIT;
  event[1] = ESP_HOSTED_TRANSPORT_CONFIG_EVENT_BYTES - 2;
  tlv = event + 2;

  tlv[0] = ESP_HOSTED_TRANSPORT_CONFIG_TAG_HOST_CAP;
  tlv[1] = 1;
  tlv[2] = 0;
  tlv += ESP_HOSTED_TRANSPORT_CONFIG_TLV_BYTES;
  tlv[0] = ESP_HOSTED_TRANSPORT_CONFIG_TAG_CHIP_ID;
  tlv[1] = 1;
  tlv[2] = info->chip_id;
  tlv += ESP_HOSTED_TRANSPORT_CONFIG_TLV_BYTES;
  tlv[0] = ESP_HOSTED_TRANSPORT_CONFIG_TAG_RAW_TP;
  tlv[1] = 1;
  tlv[2] = 0;
  tlv += ESP_HOSTED_TRANSPORT_CONFIG_TLV_BYTES;
  tlv[0] = ESP_HOSTED_TRANSPORT_CONFIG_TAG_HIGH;
  tlv[1] = 1;
  tlv[2] = 0;
  tlv += ESP_HOSTED_TRANSPORT_CONFIG_TLV_BYTES;
  tlv[0] = ESP_HOSTED_TRANSPORT_CONFIG_TAG_LOW;
  tlv[1] = 1;
  tlv[2] = 0;

  if ((info->capabilities & ESP_HOSTED_TRANSPORT_CAP_CHECKSUM) != 0)
    {
      esp_hosted_transport_put_le16(packet + 6,
                                    esp_hosted_transport_checksum(
                                      packet, sizeof(packet)));
    }

  ret = esp_hosted_transport_transfer(transport, true,
                                      ESP_HOSTED_TRANSPORT_SLC_FIFO_END -
                                      sizeof(packet), packet, sizeof(packet),
                                      false);
  if (ret < 0)
    {
      return ret;
    }

  transport->tx_buffer_count =
    (transport->tx_buffer_count + buffers) %
    ESP_HOSTED_TRANSPORT_TX_TOKEN_MAX;
  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 config sent: bytes=%u checksum=%s\n",
         (unsigned int)sizeof(packet),
         (info->capabilities & ESP_HOSTED_TRANSPORT_CAP_CHECKSUM) != 0 ?
         "enabled" : "disabled");
  return OK;
}

static int esp_hosted_transport_parse_init_event(
  FAR const uint8_t *packet, size_t packet_length,
  FAR struct esp_hosted_init_info_s *info)
{
  FAR const uint8_t *event;
  FAR const uint8_t *value;
  uint16_t payload_length;
  uint16_t payload_offset;
  uint8_t event_length;
  uint8_t tag_length;
  size_t remaining;

  if (packet_length < ESP_HOSTED_TRANSPORT_HEADER_BYTES ||
      (packet[0] & 0x0f) != ESP_HOSTED_TRANSPORT_IF_PRIVATE ||
      packet[11] != ESP_HOSTED_TRANSPORT_PACKET_EVENT)
    {
      return -EPROTO;
    }

  payload_length = esp_hosted_transport_get_le16(packet + 2);
  payload_offset = esp_hosted_transport_get_le16(packet + 4);
  if (payload_offset != ESP_HOSTED_TRANSPORT_HEADER_BYTES ||
      payload_length > packet_length - payload_offset ||
      payload_length < 2)
    {
      return -EMSGSIZE;
    }

  event = packet + payload_offset;
  if (event[0] != ESP_HOSTED_TRANSPORT_EVENT_INIT)
    {
      return -EPROTO;
    }

  event_length = event[1];
  if ((size_t)event_length + 2 > payload_length)
    {
      return -EMSGSIZE;
    }

  memset(info, 0, sizeof(*info));
  value = event + 2;
  remaining = event_length;
  while (remaining != 0)
    {
      if (remaining < 2)
        {
          return -EPROTO;
        }

      tag_length = value[1];
      if ((size_t)tag_length + 2 > remaining)
        {
          return -EPROTO;
        }

      switch (value[0])
        {
          case ESP_HOSTED_TRANSPORT_TAG_CAPABILITY:
            if (tag_length == 1)
              {
                info->capabilities = value[2];
              }
            break;

          case ESP_HOSTED_TRANSPORT_TAG_CHIP_ID:
            if (tag_length == 1)
              {
                info->chip_id = value[2];
              }
            break;

          case ESP_HOSTED_TRANSPORT_TAG_RX_QUEUE:
            if (tag_length == 1)
              {
                info->rx_queue_size = value[2];
              }
            break;

          case ESP_HOSTED_TRANSPORT_TAG_TX_QUEUE:
            if (tag_length == 1)
              {
                info->tx_queue_size = value[2];
              }
            break;

          case ESP_HOSTED_TRANSPORT_TAG_FIRMWARE:
            if (tag_length == sizeof(info->firmware_version))
              {
                info->firmware_version =
                  esp_hosted_transport_get_le32(value + 2);
              }
            break;

          default:
            break;
        }

      value += tag_length + 2;
      remaining -= tag_length + 2;
    }

  if (info->chip_id != ESP_HOSTED_TRANSPORT_CHIP_ESP32C6 ||
      (info->capabilities & ESP_HOSTED_TRANSPORT_CAP_WLAN_SDIO) == 0)
    {
      return -EPROTONOSUPPORT;
    }

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

int esp_hosted_transport_start(FAR struct esp_hosted_transport_s *transport,
                               FAR struct esp_hosted_init_info_s *info)
{
  uint8_t registers[ESP_HOSTED_TRANSPORT_SLC_REGISTER_BYTES];
  uint8_t packet[ESP_HOSTED_TRANSPORT_INIT_PACKET_MAX];
  uint32_t interrupts = 0;
  uint32_t packet_count = 0;
  size_t packet_length;
  int attempt;
  int ret;

  if (transport != &g_transport || !transport->function_ready ||
      info == NULL)
    {
      return -EINVAL;
    }

  if (transport->data_path_open)
    {
      return -EALREADY;
    }

  ret = esp_hosted_transport_read_registers(transport, registers);
  if (ret < 0)
    {
      return ret;
    }

  interrupts = esp_hosted_transport_get_le32(registers);
  transport->rx_packet_count =
    esp_hosted_transport_get_le32(registers +
                                  ESP_HOSTED_TRANSPORT_LEN_OFFSET) &
    ESP_HOSTED_TRANSPORT_SLC_PACKET_MASK;
  if (interrupts != 0)
    {
      ret = esp_hosted_transport_clear_interrupts(transport, interrupts);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* ESP_OPEN_DATA_PATH is host interrupt zero.  The C6 sends its private
   * initialization event only after this bit is written to scratch register
   * seven.  Polling is intentional at this stage; DAT1 IRQ handling is a
   * later transport optimization.
   */

  ret = esp_hosted_transport_write_reg(
    transport, ESP_HOSTED_TRANSPORT_SLC_HOST_INTR, 1);
  if (ret < 0)
    {
      return ret;
    }

  for (attempt = 0; attempt < ESP_HOSTED_TRANSPORT_INIT_WAIT_RETRIES;
       attempt++)
    {
      ret = nxsig_usleep(ESP_HOSTED_TRANSPORT_INIT_WAIT_US);
      if (ret < 0)
        {
          return ret;
        }

      ret = esp_hosted_transport_read_registers(transport, registers);
      if (ret < 0)
        {
          return ret;
        }

      interrupts = esp_hosted_transport_get_le32(registers);
      packet_count =
        esp_hosted_transport_get_le32(registers +
                                      ESP_HOSTED_TRANSPORT_LEN_OFFSET) &
        ESP_HOSTED_TRANSPORT_SLC_PACKET_MASK;
      if (interrupts != 0)
        {
          ret = esp_hosted_transport_clear_interrupts(transport, interrupts);
          if (ret < 0)
            {
              return ret;
            }
        }

      if ((interrupts & ESP_HOSTED_TRANSPORT_SLC_NEW_PACKET) == 0)
        {
          continue;
        }

      packet_length = (packet_count - transport->rx_packet_count) &
                      ESP_HOSTED_TRANSPORT_SLC_PACKET_MASK;
      if (packet_length == 0 ||
          packet_length > ESP_HOSTED_TRANSPORT_INIT_PACKET_MAX)
        {
          syslog(LOG_ERR,
                 "ERROR: ESP-Hosted C6 init: invalid packet length=%zu"
                 " counter=0x%05" PRIx32 " previous=0x%05" PRIx32
                 " int_raw=0x%08" PRIx32 "\n", packet_length,
                 packet_count, transport->rx_packet_count, interrupts);
          return -EMSGSIZE;
        }

      ret = esp_hosted_transport_read_fifo(transport, packet, packet_length);
      if (ret < 0)
        {
          return ret;
        }

      transport->rx_packet_count = packet_count;
      ret = esp_hosted_transport_parse_init_event(packet, packet_length,
                                                   info);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: ESP-Hosted C6 init: invalid event result=%d"
                 " bytes=%zu header=%02x %02x %02x %02x\n", ret,
                 packet_length, (unsigned int)packet[0],
                 (unsigned int)packet[2], (unsigned int)packet[4],
                 (unsigned int)packet[11]);
          return ret;
        }

      ret = esp_hosted_transport_send_init_config(transport, info);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: ESP-Hosted C6 init: host config result=%d\n", ret);
          return ret;
        }

      transport->data_path_open = true;
      syslog(LOG_INFO,
             "INFO: ESP-Hosted C6 init: chip=0x%02x capabilities=0x%02x"
             " firmware=0x%08" PRIx32 " rxq=%u txq=%u\n",
             (unsigned int)info->chip_id, (unsigned int)info->capabilities,
             info->firmware_version, (unsigned int)info->rx_queue_size,
             (unsigned int)info->tx_queue_size);
      return OK;
    }

  syslog(LOG_ERR,
         "ERROR: ESP-Hosted C6 init: event timeout int_raw=0x%08"
         PRIx32 " packet_count=0x%05" PRIx32 "\n", interrupts,
         packet_count);
  return -ETIMEDOUT;
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

int esp_hosted_transport_diagnose(
  FAR struct esp_hosted_transport_s *transport)
{
  uint8_t registers[ESP_HOSTED_TRANSPORT_SLC_REGISTER_BYTES];
  uint8_t packet_length_bytes[sizeof(uint32_t)];
  uint8_t direct_bytes[sizeof(uint32_t)];
  uint32_t int_raw_window;
  uint32_t int_status_window;
  uint32_t packet_length_window;
  uint32_t int_raw_direct;
  uint32_t int_status_direct;
  uint32_t packet_length_direct;
  uint32_t packet_length_cmd52;
  int sample;
  unsigned int index;
  int ret;

  if (transport != &g_transport || !transport->function_ready ||
      transport->data_path_open)
    {
      return -EINVAL;
    }

  for (sample = 0; sample < ESP_HOSTED_TRANSPORT_DIAG_SAMPLES; sample++)
    {
      ret = esp_hosted_transport_read_registers(transport, registers);
      if (ret < 0)
        {
          return ret;
        }

      for (index = 0; index < sizeof(packet_length_bytes); index++)
        {
          ret = esp_hosted_transport_read_reg(
            transport, ESP_HOSTED_TRANSPORT_SLC_PACKET_LEN + index,
            &packet_length_bytes[index]);
          if (ret < 0)
            {
              return ret;
            }
        }

      ret = esp_hosted_transport_transfer(
        transport, false, ESP_HOSTED_TRANSPORT_SLC_INT_RAW, direct_bytes,
        sizeof(direct_bytes), false);
      if (ret < 0)
        {
          return ret;
        }

      int_raw_direct = esp_hosted_transport_get_le32(direct_bytes);
      ret = esp_hosted_transport_transfer(
        transport, false, ESP_HOSTED_TRANSPORT_SLC_INT_RAW + 8,
        direct_bytes, sizeof(direct_bytes), false);
      if (ret < 0)
        {
          return ret;
        }

      int_status_direct = esp_hosted_transport_get_le32(direct_bytes);
      ret = esp_hosted_transport_transfer(
        transport, false, ESP_HOSTED_TRANSPORT_SLC_PACKET_LEN, direct_bytes,
        sizeof(direct_bytes), false);
      if (ret < 0)
        {
          return ret;
        }

      packet_length_direct = esp_hosted_transport_get_le32(direct_bytes);
      int_raw_window = esp_hosted_transport_get_le32(registers);
      int_status_window = esp_hosted_transport_get_le32(registers + 8);
      packet_length_window =
        esp_hosted_transport_get_le32(registers +
                                      ESP_HOSTED_TRANSPORT_LEN_OFFSET);
      packet_length_cmd52 =
        esp_hosted_transport_get_le32(packet_length_bytes);
      syslog(LOG_INFO,
             "INFO: ESP-Hosted C6 diag: sample=%d window(raw=0x%08"
             PRIx32 " st=0x%08" PRIx32 " len=0x%08" PRIx32 ")"
             " cmd53(raw=0x%08" PRIx32 " st=0x%08" PRIx32
             " len=0x%08" PRIx32 ") cmd52_len=0x%08" PRIx32 "\n",
             sample, int_raw_window, int_status_window,
             packet_length_window, int_raw_direct, int_status_direct,
             packet_length_direct, packet_length_cmd52);

      if (sample + 1 < ESP_HOSTED_TRANSPORT_DIAG_SAMPLES)
        {
          ret = nxsig_usleep(ESP_HOSTED_TRANSPORT_DIAG_INTERVAL_US);
          if (ret < 0)
            {
              return ret;
            }
        }
    }

  return OK;
}

static int esp_hosted_transport_transfer_once(
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

int esp_hosted_transport_transfer(
  FAR struct esp_hosted_transport_s *transport, bool write,
  uint32_t address, FAR void *buffer, size_t length, bool blocks)
{
  FAR uint8_t *bytes = buffer;
  size_t aligned_length;
  int ret;

  /* ESP32-P4 SDMMC transfers in byte mode must end on a word boundary when
   * using IDMAC.  Mirror ESP-IDF's SDIO convention: issue the aligned part
   * first, then one PIO transaction for the remaining one to three bytes.
   * CMD53 increments the address for both transfers.
   */

  if (blocks || (length & 3) == 0)
    {
      return esp_hosted_transport_transfer_once(transport, write, address,
                                                buffer, length, blocks);
    }

  aligned_length = length & ~((size_t)3);
  if (aligned_length != 0)
    {
      ret = esp_hosted_transport_transfer_once(transport, write,
                                                address, bytes,
                                                aligned_length, false);
      if (ret < 0)
        {
          return ret;
        }

      address += aligned_length;
      bytes += aligned_length;
      length -= aligned_length;
    }

  return esp_hosted_transport_transfer_once(transport, write, address,
                                            bytes, length, false);
}

#endif /* CONFIG_ESPRESSIF_HOSTED_TRANSPORT */
