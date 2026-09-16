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
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/signal.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include <arch/chip/esp_hosted_transport.h>

#ifdef CONFIG_ESPRESSIF_HOSTED_WLAN
#  include <arch/chip/esp_hosted_wlan.h>
#endif

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
#define ESP_HOSTED_TRANSPORT_RX_PACKET_MAX       1600
#define ESP_HOSTED_TRANSPORT_INIT_WAIT_RETRIES    100
#define ESP_HOSTED_TRANSPORT_INIT_WAIT_US       10000
#define ESP_HOSTED_TRANSPORT_TX_WAIT_RETRIES       10
#define ESP_HOSTED_TRANSPORT_TX_WAIT_US           400
#define ESP_HOSTED_TRANSPORT_DIAG_SAMPLES           3
#define ESP_HOSTED_TRANSPORT_DIAG_INTERVAL_US  100000
#define ESP_HOSTED_TRANSPORT_RX_WORK_DELAY_MS      10
#define ESP_HOSTED_TRANSPORT_RX_PACKETS_PER_WORK    4
#define ESP_HOSTED_TRANSPORT_RPC_TIMEOUT_MS      1000

#define ESP_HOSTED_TRANSPORT_IF_PRIVATE           5
#define ESP_HOSTED_TRANSPORT_IF_STA               1
#define ESP_HOSTED_TRANSPORT_IF_SERIAL            3
#define ESP_HOSTED_TRANSPORT_PACKET_EVENT      0x33
#define ESP_HOSTED_TRANSPORT_EVENT_INIT        0x22
#define ESP_HOSTED_TRANSPORT_HEADER_BYTES        12

#define ESP_HOSTED_TRANSPORT_TLV_EPNAME           1
#define ESP_HOSTED_TRANSPORT_TLV_DATA             2
#define ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES   6

#define ESP_HOSTED_TRANSPORT_RPC_TYPE_REQ         1
#define ESP_HOSTED_TRANSPORT_RPC_TYPE_RESP        2
#define ESP_HOSTED_TRANSPORT_RPC_TYPE_EVENT       3
#define ESP_HOSTED_TRANSPORT_RPC_REQ_GET_MODE   259
#define ESP_HOSTED_TRANSPORT_RPC_RESP_GET_MODE  515
#define ESP_HOSTED_TRANSPORT_RPC_REQ_GET_MAC    257
#define ESP_HOSTED_TRANSPORT_RPC_RESP_GET_MAC   513
#define ESP_HOSTED_TRANSPORT_RPC_REQ_SET_MODE   260
#define ESP_HOSTED_TRANSPORT_RPC_RESP_SET_MODE  516
#define ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_INIT  278
#define ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_INIT 534
#define ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_START 280
#define ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_START 536
#define ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_CONNECT 282
#define ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_CONNECT 538
#define ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_SET_CONFIG 284
#define ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_SET_CONFIG 540
#define ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_SET_STORAGE 313
#define ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_SET_STORAGE 569
#define ESP_HOSTED_TRANSPORT_RPC_EVENT_ESPINIT  769
#define ESP_HOSTED_TRANSPORT_RPC_EVENT_WIFI_NO_ARGS 773
#define ESP_HOSTED_TRANSPORT_RPC_EVENT_STA_CONNECTED 775
#define ESP_HOSTED_TRANSPORT_RPC_EVENT_STA_DISCONNECTED 776

#define ESP_HOSTED_TRANSPORT_WIFI_EVENT_STA_START 2

#define ESP_HOSTED_TRANSPORT_RPC_PACKET_MAX      256
#define ESP_HOSTED_TRANSPORT_WIFI_CONFIG_MAX     104
#define ESP_HOSTED_TRANSPORT_STA_SSID_MAX          32
#define ESP_HOSTED_TRANSPORT_STA_PASSWORD_MAX      64

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

struct esp_hosted_wifi_config_field_s
{
  uint8_t field;
  uint64_t value;
};

struct esp_hosted_transport_s
{
  FAR struct esp_hosted_sdio_s *sdio;
  struct work_s rx_work;
  mutex_t bus_lock;
  mutex_t rpc_lock;
  spinlock_t tx_lock;
  sem_t rpc_sem;
  sem_t sta_start_sem;
  bool initialized;
  bool function_ready;
  bool data_path_open;
  bool rx_active;
  bool checksum_enabled;
  bool rpc_pending;
  bool sta_started;
  uint32_t rx_packet_count;
  uint16_t tx_buffer_count;
  uint16_t tx_sequence;
  uint32_t rpc_uid;
  uint32_t rpc_response_id;
  uint32_t rpc_wifi_mode;
  uint8_t rpc_mac[6];
  int rpc_result;
  esp_hosted_transport_wlan_rx_t wlan_rx;
  FAR void *wlan_rx_arg;
  uint8_t rx_packet[ESP_HOSTED_TRANSPORT_RX_PACKET_MAX];
  uint8_t tx_packet[ESP_HOSTED_TRANSPORT_TX_BUFFER_BYTES];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp_hosted_transport_s g_transport;

static void esp_hosted_transport_rx_worker(FAR void *arg);

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
      syslog(LOG_ERR, "ERROR: ESP-Hosted RX: read INT_RAW failed: %d\n",
             ret);
      return ret;
    }

  ret = esp_hosted_transport_transfer(
    transport, false, ESP_HOSTED_TRANSPORT_SLC_INT_RAW + 8, registers + 8,
    sizeof(uint32_t), false);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: ESP-Hosted RX: read INT_ST failed: %d\n",
             ret);
      return ret;
    }

  ret = esp_hosted_transport_transfer(
    transport, false, ESP_HOSTED_TRANSPORT_SLC_PACKET_LEN, registers +
    ESP_HOSTED_TRANSPORT_LEN_OFFSET, sizeof(uint32_t), false);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: ESP-Hosted RX: read PACKET_LEN failed: %d\n", ret);
    }

  return ret;
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

static uint16_t esp_hosted_transport_next_tx_sequence(
  FAR struct esp_hosted_transport_s *transport)
{
  irqstate_t flags;
  uint16_t sequence;

  flags = spin_lock_irqsave(&transport->tx_lock);
  sequence = transport->tx_sequence++;
  spin_unlock_irqrestore(&transport->tx_lock, flags);
  return sequence;
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

static int esp_hosted_transport_send_packet_locked(
  FAR struct esp_hosted_transport_s *transport, FAR uint8_t *packet,
  size_t packet_length)
{
  unsigned int buffers;
  int ret;

  if (packet_length == 0 ||
      packet_length > ESP_HOSTED_TRANSPORT_TX_BUFFER_BYTES)
    {
      return -EMSGSIZE;
    }

  buffers = (packet_length + ESP_HOSTED_TRANSPORT_TX_BUFFER_BYTES - 1) /
            ESP_HOSTED_TRANSPORT_TX_BUFFER_BYTES;
  ret = esp_hosted_transport_wait_tx_buffers(transport, buffers);
  if (ret == OK)
    {
      ret = esp_hosted_transport_transfer(
        transport, true, ESP_HOSTED_TRANSPORT_SLC_FIFO_END - packet_length,
        packet, packet_length, false);
      if (ret == OK)
        {
          transport->tx_buffer_count =
            (transport->tx_buffer_count + buffers) %
            ESP_HOSTED_TRANSPORT_TX_TOKEN_MAX;
        }
    }

  return ret;
}

static int esp_hosted_transport_send_packet(
  FAR struct esp_hosted_transport_s *transport, FAR uint8_t *packet,
  size_t packet_length)
{
  int ret;

  ret = nxmutex_lock(&transport->bus_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_transport_send_packet_locked(transport, packet,
                                                 packet_length);
  nxmutex_unlock(&transport->bus_lock);
  return ret;
}

static int esp_hosted_transport_send_init_config(
  FAR struct esp_hosted_transport_s *transport,
  FAR const struct esp_hosted_init_info_s *info)
{
  uint8_t packet[ESP_HOSTED_TRANSPORT_CONFIG_PACKET_BYTES];
  uint8_t *event;
  uint8_t *tlv;
  int ret;

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

  if (transport->checksum_enabled)
    {
      esp_hosted_transport_put_le16(packet + 6,
                                    esp_hosted_transport_checksum(
                                      packet, sizeof(packet)));
    }

  ret = esp_hosted_transport_send_packet(transport, packet, sizeof(packet));
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 config sent: bytes=%u checksum=%s\n",
         (unsigned int)sizeof(packet),
         transport->checksum_enabled ?
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

static int esp_hosted_transport_get_varint(FAR const uint8_t *data,
                                           size_t length,
                                           FAR size_t *offset,
                                           FAR uint64_t *value)
{
  uint64_t result = 0;
  uint8_t byte;
  unsigned int shift;

  for (shift = 0; shift < 64; shift += 7)
    {
      if (*offset >= length)
        {
          return -EMSGSIZE;
        }

      byte = data[(*offset)++];
      result |= (uint64_t)(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0)
        {
          *value = result;
          return OK;
        }
    }

  return -EPROTO;
}

static int esp_hosted_transport_skip_field(FAR const uint8_t *data,
                                           size_t length,
                                           FAR size_t *offset,
                                           uint32_t wire_type)
{
  uint64_t field_length;

  switch (wire_type)
    {
      case 0:
        return esp_hosted_transport_get_varint(data, length, offset,
                                               &field_length);

      case 1:
        field_length = 8;
        break;

      case 2:
        if (esp_hosted_transport_get_varint(data, length, offset,
                                            &field_length) < 0)
          {
            return -EMSGSIZE;
          }
        break;

      case 5:
        field_length = 4;
        break;

      default:
        return -EPROTO;
    }

  if (field_length > length - *offset)
    {
      return -EMSGSIZE;
    }

  *offset += field_length;
  return OK;
}

static int esp_hosted_transport_parse_serial_tlv(
  FAR const uint8_t *payload, size_t payload_length,
  FAR const uint8_t **rpc, FAR size_t *rpc_length)
{
  static const uint8_t response_endpoint[] = "RPCRsp";
  static const uint8_t event_endpoint[] = "RPCEvt";
  uint16_t endpoint_length;
  uint16_t data_length;
  size_t offset;

  if (payload_length < 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES + 3 ||
      payload[0] != ESP_HOSTED_TRANSPORT_TLV_EPNAME)
    {
      return -EPROTO;
    }

  endpoint_length = esp_hosted_transport_get_le16(payload + 1);
  offset = 3;
  if (endpoint_length != ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES ||
      endpoint_length > payload_length - offset ||
      (memcmp(payload + offset, response_endpoint, endpoint_length) != 0 &&
       memcmp(payload + offset, event_endpoint, endpoint_length) != 0))
    {
      return -EPROTO;
    }

  offset += endpoint_length;
  if (offset + 3 > payload_length ||
      payload[offset] != ESP_HOSTED_TRANSPORT_TLV_DATA)
    {
      return -EPROTO;
    }

  data_length = esp_hosted_transport_get_le16(payload + offset + 1);
  offset += 3;
  if (data_length == 0 || data_length != payload_length - offset)
    {
      return -EMSGSIZE;
    }

  *rpc = payload + offset;
  *rpc_length = data_length;
  return OK;
}

static int esp_hosted_transport_handle_get_mode_response(
  FAR struct esp_hosted_transport_s *transport, FAR const uint8_t *payload,
  size_t payload_length, uint32_t uid)
{
  uint64_t key;
  uint64_t value;
  size_t offset = 0;
  uint32_t mode = 0;
  int result = OK;
  int ret;

  while (offset < payload_length)
    {
      ret = esp_hosted_transport_get_varint(payload, payload_length, &offset,
                                            &key);
      if (ret < 0)
        {
          return ret;
        }

      if ((key & 7) != 0)
        {
          ret = esp_hosted_transport_skip_field(payload, payload_length,
                                                &offset, key & 7);
          if (ret < 0)
            {
              return ret;
            }

          continue;
        }

      ret = esp_hosted_transport_get_varint(payload, payload_length, &offset,
                                            &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((key >> 3) == 1)
        {
          mode = (uint32_t)value;
        }
      else if ((key >> 3) == 2)
        {
          result = (int32_t)value;
        }
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->rpc_pending && transport->rpc_uid == uid &&
      transport->rpc_response_id == ESP_HOSTED_TRANSPORT_RPC_RESP_GET_MODE)
    {
      transport->rpc_wifi_mode = mode;
      transport->rpc_result = result;
      transport->rpc_pending = false;
      syslog(LOG_INFO,
             "INFO: ESP-Hosted C6 RPC: GetWifiMode response uid=%" PRIu32
             " mode=%" PRIu32 " result=%d\n", uid, mode, result);
      nxsem_post(&transport->rpc_sem);
    }

  nxmutex_unlock(&transport->rpc_lock);
  return OK;
}

static int esp_hosted_transport_handle_result_response(
  FAR struct esp_hosted_transport_s *transport, FAR const uint8_t *payload,
  size_t payload_length, uint32_t uid, uint32_t response_id,
  FAR const char *name)
{
  uint64_t key;
  uint64_t value;
  size_t offset = 0;
  int result = OK;
  int ret;

  while (offset < payload_length)
    {
      ret = esp_hosted_transport_get_varint(payload, payload_length, &offset,
                                            &key);
      if (ret < 0)
        {
          return ret;
        }

      if ((key & 7) != 0)
        {
          ret = esp_hosted_transport_skip_field(payload, payload_length,
                                                &offset, key & 7);
          if (ret < 0)
            {
              return ret;
            }

          continue;
        }

      ret = esp_hosted_transport_get_varint(payload, payload_length, &offset,
                                            &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((key >> 3) == 1)
        {
          result = (int32_t)value;
        }
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->rpc_pending && transport->rpc_uid == uid &&
      transport->rpc_response_id == response_id)
    {
      transport->rpc_result = result;
      transport->rpc_pending = false;
      syslog(LOG_INFO, "INFO: ESP-Hosted C6 RPC: %s response uid=%" PRIu32
             " result=%d\n", name, uid, result);
      nxsem_post(&transport->rpc_sem);
    }

  nxmutex_unlock(&transport->rpc_lock);
  return OK;
}

static int esp_hosted_transport_handle_get_mac_response(
  FAR struct esp_hosted_transport_s *transport, FAR const uint8_t *payload,
  size_t payload_length, uint32_t uid)
{
  uint64_t key;
  uint64_t value;
  uint64_t length;
  uint8_t mac[6];
  size_t offset = 0;
  int result = OK;
  int ret;

  memset(mac, 0, sizeof(mac));

  while (offset < payload_length)
    {
      ret = esp_hosted_transport_get_varint(payload, payload_length, &offset,
                                            &key);
      if (ret < 0)
        {
          return ret;
        }

      if ((key >> 3) == 1 && (key & 7) == 2)
        {
          ret = esp_hosted_transport_get_varint(payload, payload_length,
                                                &offset, &length);
          if (ret < 0 || length != sizeof(mac) ||
              length > payload_length - offset)
            {
              return -EPROTO;
            }

          memcpy(mac, payload + offset, sizeof(mac));
          offset += length;
          continue;
        }

      if ((key & 7) != 0)
        {
          ret = esp_hosted_transport_skip_field(payload, payload_length,
                                                &offset, key & 7);
          if (ret < 0)
            {
              return ret;
            }

          continue;
        }

      ret = esp_hosted_transport_get_varint(payload, payload_length, &offset,
                                            &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((key >> 3) == 2)
        {
          result = (int32_t)value;
        }
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->rpc_pending && transport->rpc_uid == uid &&
      transport->rpc_response_id == ESP_HOSTED_TRANSPORT_RPC_RESP_GET_MAC)
    {
      memcpy(transport->rpc_mac, mac, sizeof(mac));
      transport->rpc_result = result;
      transport->rpc_pending = false;
      syslog(LOG_INFO,
             "INFO: ESP-Hosted C6 RPC: GetMACAddress response uid=%" PRIu32
             " result=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n", uid,
             result, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      nxsem_post(&transport->rpc_sem);
    }

  nxmutex_unlock(&transport->rpc_lock);
  return OK;
}

static int esp_hosted_transport_handle_wifi_event_no_args(
  FAR struct esp_hosted_transport_s *transport, FAR const uint8_t *payload,
  size_t payload_length)
{
  uint64_t key;
  uint64_t value;
  size_t offset = 0;
  int event_id = -1;
  int result = OK;
  int ret;

  while (offset < payload_length)
    {
      ret = esp_hosted_transport_get_varint(payload, payload_length, &offset,
                                            &key);
      if (ret < 0)
        {
          return ret;
        }

      if ((key & 7) != 0)
        {
          ret = esp_hosted_transport_skip_field(payload, payload_length,
                                                &offset, key & 7);
          if (ret < 0)
            {
              return ret;
            }

          continue;
        }

      ret = esp_hosted_transport_get_varint(payload, payload_length, &offset,
                                            &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((key >> 3) == 1)
        {
          result = (int32_t)value;
        }
      else if ((key >> 3) == 2)
        {
          event_id = (int32_t)value;
        }
    }

  syslog(LOG_INFO, "INFO: ESP-Hosted C6 event: wifi_event=%d result=%d\n",
         event_id, result);

  if (event_id != ESP_HOSTED_TRANSPORT_WIFI_EVENT_STA_START)
    {
      return OK;
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!transport->sta_started)
    {
      transport->sta_started = true;
      nxsem_post(&transport->sta_start_sem);
    }

  nxmutex_unlock(&transport->rpc_lock);
  return OK;
}

static int esp_hosted_transport_handle_sta_link_event(
  FAR const uint8_t *payload, size_t payload_length, bool up)
{
  uint64_t key;
  uint64_t value;
  size_t offset = 0;
  int result = OK;
  int ret;

  /* Rpc_Event_StaConnected and Rpc_Event_StaDisconnected both carry the
   * operation result in protobuf field 1.  The connected/disconnected
   * detail is field 2 and is intentionally skipped: only the association
   * state belongs to the NuttX data-plane adapter.
   */

  while (offset < payload_length)
    {
      ret = esp_hosted_transport_get_varint(payload, payload_length, &offset,
                                            &key);
      if (ret < 0)
        {
          return ret;
        }

      if ((key >> 3) == 1)
        {
          if ((key & 7) != 0)
            {
              return -EPROTO;
            }

          ret = esp_hosted_transport_get_varint(payload, payload_length,
                                                &offset, &value);
          if (ret < 0)
            {
              return ret;
            }

          result = (int32_t)value;
          continue;
        }

      ret = esp_hosted_transport_skip_field(payload, payload_length, &offset,
                                            key & 7);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (result != OK)
    {
      syslog(LOG_WARNING,
             "WARNING: ESP-Hosted C6 STA %s event result=%d ignored\n",
             up ? "connected" : "disconnected", result);
      return OK;
    }

#ifdef CONFIG_ESPRESSIF_HOSTED_WLAN
  esp_hosted_wlan_set_link(up);
#endif

  syslog(LOG_INFO, "INFO: ESP-Hosted C6 STA %s; wlan0 carrier %s\n",
         up ? "connected" : "disconnected", up ? "on" : "off");
  return OK;
}

static int esp_hosted_transport_handle_rpc(
  FAR struct esp_hosted_transport_s *transport, FAR const uint8_t *rpc,
  size_t rpc_length)
{
  FAR const uint8_t *event = NULL;
  FAR const uint8_t *response = NULL;
  uint64_t key;
  uint64_t value;
  uint64_t event_length = 0;
  uint64_t payload_length = 0;
  uint32_t response_id = 0;
  size_t offset = 0;
  uint32_t message_type = 0;
  uint32_t message_id = 0;
  uint32_t uid = 0;
  int ret;

  while (offset < rpc_length)
    {
      ret = esp_hosted_transport_get_varint(rpc, rpc_length, &offset, &key);
      if (ret < 0)
        {
          return ret;
        }

      if (((key >> 3) == ESP_HOSTED_TRANSPORT_RPC_RESP_GET_MAC ||
           (key >> 3) == ESP_HOSTED_TRANSPORT_RPC_RESP_GET_MODE ||
           (key >> 3) == ESP_HOSTED_TRANSPORT_RPC_RESP_SET_MODE ||
           (key >> 3) == ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_INIT ||
           (key >> 3) == ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_START ||
           (key >> 3) == ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_CONNECT ||
           (key >> 3) == ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_SET_STORAGE ||
           (key >> 3) == ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_SET_CONFIG) &&
          (key & 7) == 2)
        {
          ret = esp_hosted_transport_get_varint(rpc, rpc_length, &offset,
                                                &payload_length);
          if (ret < 0 || payload_length > rpc_length - offset)
            {
              return -EMSGSIZE;
            }

          response = rpc + offset;
          response_id = key >> 3;
          offset += payload_length;
          continue;
        }

      if (((key >> 3) == ESP_HOSTED_TRANSPORT_RPC_EVENT_WIFI_NO_ARGS ||
           (key >> 3) == ESP_HOSTED_TRANSPORT_RPC_EVENT_STA_CONNECTED ||
           (key >> 3) == ESP_HOSTED_TRANSPORT_RPC_EVENT_STA_DISCONNECTED) &&
          (key & 7) == 2)
        {
          ret = esp_hosted_transport_get_varint(rpc, rpc_length, &offset,
                                                &event_length);
          if (ret < 0 || event_length > rpc_length - offset)
            {
              return -EMSGSIZE;
            }

          event = rpc + offset;
          offset += event_length;
          continue;
        }

      if ((key & 7) != 0)
        {
          ret = esp_hosted_transport_skip_field(rpc, rpc_length, &offset,
                                                key & 7);
          if (ret < 0)
            {
              return ret;
            }

          continue;
        }

      ret = esp_hosted_transport_get_varint(rpc, rpc_length, &offset,
                                            &value);
      if (ret < 0)
        {
          return ret;
        }

      switch (key >> 3)
        {
          case 1:
            message_type = (uint32_t)value;
            break;

          case 2:
            message_id = (uint32_t)value;
            break;

          case 3:
            uid = (uint32_t)value;
            break;

          default:
            break;
        }
    }

  if (message_type == ESP_HOSTED_TRANSPORT_RPC_TYPE_EVENT)
    {
      if (message_id == ESP_HOSTED_TRANSPORT_RPC_EVENT_ESPINIT)
        {
          syslog(LOG_INFO, "INFO: ESP-Hosted C6 RX: ESPInit event\n");
        }
      else if (message_id == ESP_HOSTED_TRANSPORT_RPC_EVENT_WIFI_NO_ARGS &&
               event != NULL)
        {
          return esp_hosted_transport_handle_wifi_event_no_args(
            transport, event, event_length);
        }
      else if (message_id == ESP_HOSTED_TRANSPORT_RPC_EVENT_STA_CONNECTED &&
               event != NULL)
        {
          return esp_hosted_transport_handle_sta_link_event(
            event, event_length, true);
        }
      else if (message_id == ESP_HOSTED_TRANSPORT_RPC_EVENT_STA_DISCONNECTED &&
               event != NULL)
        {
          return esp_hosted_transport_handle_sta_link_event(
            event, event_length, false);
        }
      else
        {
          syslog(LOG_INFO, "INFO: ESP-Hosted C6 RX: event=%" PRIu32 "\n",
                 message_id);
        }

      return OK;
    }

  if (message_type != ESP_HOSTED_TRANSPORT_RPC_TYPE_RESP ||
      message_id != response_id || response == NULL)
    {
      syslog(LOG_INFO,
             "INFO: ESP-Hosted C6 RX: control type=%" PRIu32
             " id=%" PRIu32 " ignored\n", message_type, message_id);
      return OK;
    }

  if (message_id == ESP_HOSTED_TRANSPORT_RPC_RESP_GET_MODE)
    {
      return esp_hosted_transport_handle_get_mode_response(
        transport, response, payload_length, uid);
    }

  if (message_id == ESP_HOSTED_TRANSPORT_RPC_RESP_GET_MAC)
    {
      return esp_hosted_transport_handle_get_mac_response(
        transport, response, payload_length, uid);
    }

  if (message_id == ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_INIT)
    {
      return esp_hosted_transport_handle_result_response(
        transport, response, payload_length, uid,
        ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_INIT, "WifiInit");
    }

  if (message_id == ESP_HOSTED_TRANSPORT_RPC_RESP_SET_MODE)
    {
      return esp_hosted_transport_handle_result_response(
        transport, response, payload_length, uid,
        ESP_HOSTED_TRANSPORT_RPC_RESP_SET_MODE, "SetWifiMode");
    }

  if (message_id == ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_START)
    {
      return esp_hosted_transport_handle_result_response(
        transport, response, payload_length, uid,
        ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_START, "WifiStart");
    }

  if (message_id == ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_SET_CONFIG)
    {
      return esp_hosted_transport_handle_result_response(
        transport, response, payload_length, uid,
        ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_SET_CONFIG, "WifiSetConfig");
    }

  if (message_id == ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_SET_STORAGE)
    {
      return esp_hosted_transport_handle_result_response(
        transport, response, payload_length, uid,
        ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_SET_STORAGE, "WifiSetStorage");
    }

  if (message_id == ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_CONNECT)
    {
      return esp_hosted_transport_handle_result_response(
        transport, response, payload_length, uid,
        ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_CONNECT, "WifiConnect");
    }

  syslog(LOG_INFO, "INFO: ESP-Hosted C6 RX: response=%" PRIu32
         " ignored\n", message_id);
  return OK;
}

static int esp_hosted_transport_handle_wlan_packet(
  FAR struct esp_hosted_transport_s *transport, FAR const uint8_t *data,
  size_t length)
{
  esp_hosted_transport_wlan_rx_t callback;
  FAR void *arg;
  int ret;

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  callback = transport->wlan_rx;
  arg = transport->wlan_rx_arg;
  if (callback == NULL)
    {
      nxmutex_unlock(&transport->rpc_lock);
      syslog(LOG_INFO, "INFO: ESP-Hosted C6 RX: station frame dropped\n");
      return OK;
    }

  /* Keep the callback registration stable until the adapter copies this
   * frame.  Deinitialization clears the callback through the same lock.
   */

  ret = callback(arg, data, length);
  nxmutex_unlock(&transport->rpc_lock);
  return ret;
}

static int esp_hosted_transport_handle_packet(
  FAR struct esp_hosted_transport_s *transport, FAR const uint8_t *packet,
  size_t packet_length)
{
  FAR const uint8_t *rpc;
  uint16_t payload_length;
  uint16_t payload_offset;
  size_t rpc_length;
  int ret;

  if (packet_length < ESP_HOSTED_TRANSPORT_HEADER_BYTES)
    {
      return -EMSGSIZE;
    }

  payload_length = esp_hosted_transport_get_le16(packet + 2);
  payload_offset = esp_hosted_transport_get_le16(packet + 4);
  if (payload_offset != ESP_HOSTED_TRANSPORT_HEADER_BYTES ||
      payload_length != packet_length - payload_offset)
    {
      return -EPROTO;
    }

  if ((packet[0] & 0x0f) == ESP_HOSTED_TRANSPORT_IF_STA)
    {
      if ((packet[0] >> 4) != 0)
        {
          return -EPROTO;
        }

      return esp_hosted_transport_handle_wlan_packet(
        transport, packet + payload_offset, payload_length);
    }

  if ((packet[0] & 0x0f) != ESP_HOSTED_TRANSPORT_IF_SERIAL)
    {
      syslog(LOG_INFO, "INFO: ESP-Hosted C6 RX: interface=%u ignored\n",
             (unsigned int)(packet[0] & 0x0f));
      return OK;
    }

  ret = esp_hosted_transport_parse_serial_tlv(packet + payload_offset,
                                               payload_length, &rpc,
                                               &rpc_length);
  if (ret < 0)
    {
      return ret;
    }

  return esp_hosted_transport_handle_rpc(transport, rpc, rpc_length);
}

static int esp_hosted_transport_handle_packets(
  FAR struct esp_hosted_transport_s *transport, FAR const uint8_t *packets,
  size_t packets_length)
{
  uint16_t payload_length;
  size_t packet_length;
  size_t offset = 0;
  int ret;

  while (offset < packets_length)
    {
      if (packets_length - offset < ESP_HOSTED_TRANSPORT_HEADER_BYTES)
        {
          return -EMSGSIZE;
        }

      payload_length = esp_hosted_transport_get_le16(packets + offset + 2);
      packet_length = ESP_HOSTED_TRANSPORT_HEADER_BYTES + payload_length;
      if (packet_length > packets_length - offset)
        {
          return -EMSGSIZE;
        }

      ret = esp_hosted_transport_handle_packet(transport, packets + offset,
                                               packet_length);
      if (ret < 0)
        {
          return ret;
        }

      offset += packet_length;
    }

  return OK;
}

static int esp_hosted_transport_receive_one(
  FAR struct esp_hosted_transport_s *transport, FAR bool *received,
  FAR size_t *received_length)
{
  uint8_t registers[ESP_HOSTED_TRANSPORT_SLC_REGISTER_BYTES];
  uint32_t interrupts;
  uint32_t packet_count;
  size_t packet_length;
  int ret;

  *received = false;
  *received_length = 0;
  ret = nxmutex_lock(&transport->bus_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_transport_read_registers(transport, registers);
  if (ret < 0)
    {
      goto out;
    }

  interrupts = esp_hosted_transport_get_le32(registers);
  if (interrupts == 0)
    {
      goto out;
    }

  ret = esp_hosted_transport_clear_interrupts(transport, interrupts);
  if (ret < 0)
    {
      goto out;
    }

  if ((interrupts & ESP_HOSTED_TRANSPORT_SLC_NEW_PACKET) == 0)
    {
      goto out;
    }

  packet_count = esp_hosted_transport_get_le32(
    registers + ESP_HOSTED_TRANSPORT_LEN_OFFSET) &
    ESP_HOSTED_TRANSPORT_SLC_PACKET_MASK;
  packet_length = (packet_count - transport->rx_packet_count) &
                  ESP_HOSTED_TRANSPORT_SLC_PACKET_MASK;
  if (packet_length == 0 ||
      packet_length > ESP_HOSTED_TRANSPORT_RX_PACKET_MAX)
    {
      ret = -EMSGSIZE;
      goto out;
    }

  ret = esp_hosted_transport_read_fifo(transport, transport->rx_packet,
                                       packet_length);
  if (ret == OK)
    {
      transport->rx_packet_count = packet_count;
      *received = true;
      *received_length = packet_length;
    }

out:
  nxmutex_unlock(&transport->bus_lock);
  return ret;
}

static void esp_hosted_transport_rx_worker(FAR void *arg)
{
  FAR struct esp_hosted_transport_s *transport = arg;
  bool received;
  size_t received_length;
  unsigned int packet;
  int ret;

  for (packet = 0; transport->rx_active &&
       packet < ESP_HOSTED_TRANSPORT_RX_PACKETS_PER_WORK; packet++)
    {
      ret = esp_hosted_transport_receive_one(transport, &received,
                                              &received_length);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: ESP-Hosted C6 RX: receive result=%d\n",
                 ret);
          transport->rx_active = false;
          break;
        }

      if (!received)
        {
          break;
        }

      ret = esp_hosted_transport_handle_packets(transport,
                                                transport->rx_packet,
                                                received_length);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: ESP-Hosted C6 RX: packet result=%d\n",
                 ret);
        }
    }

  if (transport->rx_active)
    {
      ret = work_queue(LPWORK, &transport->rx_work,
                       esp_hosted_transport_rx_worker, transport,
                       MSEC2TICK(ESP_HOSTED_TRANSPORT_RX_WORK_DELAY_MS));
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: ESP-Hosted C6 RX: schedule result=%d\n",
                 ret);
          transport->rx_active = false;
        }
    }
}

static size_t esp_hosted_transport_put_varint(FAR uint8_t *data,
                                              uint32_t value)
{
  size_t length = 0;

  do
    {
      data[length] = value & 0x7f;
      value >>= 7;
      if (value != 0)
        {
          data[length] |= 0x80;
        }

      length++;
    }
  while (value != 0);

  return length;
}

static int esp_hosted_transport_append_varint(FAR uint8_t *data,
                                              size_t capacity,
                                              FAR size_t *offset,
                                              uint64_t value)
{
  do
    {
      if (*offset >= capacity)
        {
          return -EMSGSIZE;
        }

      data[*offset] = value & 0x7f;
      value >>= 7;
      if (value != 0)
        {
          data[*offset] |= 0x80;
        }

      (*offset)++;
    }
  while (value != 0);

  return OK;
}

static int esp_hosted_transport_append_field(FAR uint8_t *data,
                                             size_t capacity,
                                             FAR size_t *offset,
                                             uint32_t field,
                                             uint64_t value)
{
  int ret;

  if (value == 0)
    {
      return OK;
    }

  ret = esp_hosted_transport_append_varint(data, capacity, offset,
                                           (uint64_t)field << 3);
  if (ret < 0)
    {
      return ret;
    }

  return esp_hosted_transport_append_varint(data, capacity, offset, value);
}

static int esp_hosted_transport_append_bytes(FAR uint8_t *data,
                                             size_t capacity,
                                             FAR size_t *offset,
                                             uint32_t field,
                                             FAR const uint8_t *value,
                                             size_t value_length)
{
  int ret;

  if (value_length == 0)
    {
      return OK;
    }

  if (value == NULL)
    {
      return -EINVAL;
    }

  ret = esp_hosted_transport_append_varint(data, capacity, offset,
                                           ((uint64_t)field << 3) | 2);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_transport_append_varint(data, capacity, offset,
                                           value_length);
  if (ret < 0)
    {
      return ret;
    }

  if (*offset > capacity || value_length > capacity - *offset)
    {
      return -EMSGSIZE;
    }

  memcpy(data + *offset, value, value_length);
  *offset += value_length;
  return OK;
}

static int esp_hosted_transport_append_empty_message(FAR uint8_t *data,
                                                     size_t capacity,
                                                     FAR size_t *offset,
                                                     uint32_t field)
{
  int ret;

  /* Unlike empty byte strings, an empty message must retain its field key
   * and zero length.  protobuf-c then allocates the default-valued nested
   * object instead of leaving its pointer NULL.
   */

  ret = esp_hosted_transport_append_varint(data, capacity, offset,
                                           ((uint64_t)field << 3) | 2);
  if (ret < 0)
    {
      return ret;
    }

  return esp_hosted_transport_append_varint(data, capacity, offset, 0);
}

static int esp_hosted_transport_send_get_mode(
  FAR struct esp_hosted_transport_s *transport, uint32_t uid)
{
  static const uint8_t endpoint[] = "RPCRsp";
  uint8_t packet[64];
  FAR uint8_t *payload;
  FAR uint8_t *rpc;
  size_t rpc_length;
  size_t payload_length;
  int ret;

  memset(packet, 0, sizeof(packet));
  packet[0] = ESP_HOSTED_TRANSPORT_IF_SERIAL;
  esp_hosted_transport_put_le16(packet + 4,
                                ESP_HOSTED_TRANSPORT_HEADER_BYTES);
  esp_hosted_transport_put_le16(
    packet + 8, esp_hosted_transport_next_tx_sequence(transport));

  payload = packet + ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  payload[0] = ESP_HOSTED_TRANSPORT_TLV_EPNAME;
  esp_hosted_transport_put_le16(
    payload + 1, ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES);
  memcpy(payload + 3, endpoint, sizeof(endpoint) - 1);
  payload += 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES;
  payload[0] = ESP_HOSTED_TRANSPORT_TLV_DATA;
  rpc = payload + 3;

  rpc[0] = 0x08;
  rpc[1] = ESP_HOSTED_TRANSPORT_RPC_TYPE_REQ;
  rpc[2] = 0x10;
  rpc[3] = 0x83;
  rpc[4] = 0x02;
  rpc[5] = 0x18;
  rpc_length = 6 + esp_hosted_transport_put_varint(rpc + 6, uid);
  rpc[rpc_length++] = 0x9a;
  rpc[rpc_length++] = 0x10;
  rpc[rpc_length++] = 0x00;

  esp_hosted_transport_put_le16(payload + 1, rpc_length);
  payload_length = 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES + 3 +
                   rpc_length;
  esp_hosted_transport_put_le16(packet + 2, payload_length);

  if (transport->checksum_enabled)
    {
      esp_hosted_transport_put_le16(packet + 6, 0);
      esp_hosted_transport_put_le16(
        packet + 6, esp_hosted_transport_checksum(
                      packet, ESP_HOSTED_TRANSPORT_HEADER_BYTES +
                      payload_length));
    }

  payload_length += ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  ret = esp_hosted_transport_send_packet(transport, packet, payload_length);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 RPC: GetWifiMode sent uid=%" PRIu32
         " bytes=%u\n", uid, (unsigned int)payload_length);
  return OK;
}

static int esp_hosted_transport_send_scalar_request(
  FAR struct esp_hosted_transport_s *transport, uint32_t request_id,
  FAR const char *name, uint32_t value, uint32_t uid)
{
  static const uint8_t endpoint[] = "RPCRsp";
  uint8_t packet[64];
  uint8_t request[8];
  FAR uint8_t *payload;
  FAR uint8_t *rpc;
  size_t request_length = 0;
  size_t rpc_length = 0;
  size_t packet_length;
  int ret;

  ret = esp_hosted_transport_append_field(request, sizeof(request),
                                          &request_length, 1, value);
  if (ret < 0)
    {
      return ret;
    }

  memset(packet, 0, sizeof(packet));
  packet[0] = ESP_HOSTED_TRANSPORT_IF_SERIAL;
  esp_hosted_transport_put_le16(packet + 4,
                                ESP_HOSTED_TRANSPORT_HEADER_BYTES);
  esp_hosted_transport_put_le16(
    packet + 8, esp_hosted_transport_next_tx_sequence(transport));

  payload = packet + ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  payload[0] = ESP_HOSTED_TRANSPORT_TLV_EPNAME;
  esp_hosted_transport_put_le16(
    payload + 1, ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES);
  memcpy(payload + 3, endpoint, sizeof(endpoint) - 1);
  payload += 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES;
  payload[0] = ESP_HOSTED_TRANSPORT_TLV_DATA;
  rpc = payload + 3;

  ret = esp_hosted_transport_append_field(
    rpc, sizeof(packet) - (rpc - packet), &rpc_length, 1,
    ESP_HOSTED_TRANSPORT_RPC_TYPE_REQ);
  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_field(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, 2,
        request_id);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_field(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, 3, uid);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_varint(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length,
        ((uint64_t)request_id << 3) | 2);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_varint(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, request_length);
    }

  if (ret < 0 || request_length > sizeof(packet) - (rpc - packet) -
                                     rpc_length)
    {
      return ret < 0 ? ret : -EMSGSIZE;
    }

  memcpy(rpc + rpc_length, request, request_length);
  rpc_length += request_length;
  esp_hosted_transport_put_le16(payload + 1, rpc_length);
  packet_length = 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES + 3 +
                  rpc_length;
  esp_hosted_transport_put_le16(packet + 2, packet_length);

  if (transport->checksum_enabled)
    {
      esp_hosted_transport_put_le16(packet + 6, 0);
      esp_hosted_transport_put_le16(
        packet + 6, esp_hosted_transport_checksum(
                      packet, ESP_HOSTED_TRANSPORT_HEADER_BYTES +
                      packet_length));
    }

  packet_length += ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  ret = esp_hosted_transport_send_packet(transport, packet, packet_length);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 RPC: %s sent uid=%" PRIu32
         " value=%" PRIu32 " bytes=%u\n", name, uid, value,
         (unsigned int)packet_length);
  return OK;
}

static int esp_hosted_transport_send_empty_request(
  FAR struct esp_hosted_transport_s *transport, uint32_t request_id,
  FAR const char *name, uint32_t uid)
{
  static const uint8_t endpoint[] = "RPCRsp";
  uint8_t packet[64];
  FAR uint8_t *payload;
  FAR uint8_t *rpc;
  size_t rpc_length = 0;
  size_t packet_length;
  int ret;

  memset(packet, 0, sizeof(packet));
  packet[0] = ESP_HOSTED_TRANSPORT_IF_SERIAL;
  esp_hosted_transport_put_le16(packet + 4,
                                ESP_HOSTED_TRANSPORT_HEADER_BYTES);
  esp_hosted_transport_put_le16(
    packet + 8, esp_hosted_transport_next_tx_sequence(transport));

  payload = packet + ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  payload[0] = ESP_HOSTED_TRANSPORT_TLV_EPNAME;
  esp_hosted_transport_put_le16(
    payload + 1, ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES);
  memcpy(payload + 3, endpoint, sizeof(endpoint) - 1);
  payload += 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES;
  payload[0] = ESP_HOSTED_TRANSPORT_TLV_DATA;
  rpc = payload + 3;

  ret = esp_hosted_transport_append_field(
    rpc, sizeof(packet) - (rpc - packet), &rpc_length, 1,
    ESP_HOSTED_TRANSPORT_RPC_TYPE_REQ);
  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_field(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, 2,
        request_id);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_field(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, 3, uid);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_varint(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length,
        ((uint64_t)request_id << 3) | 2);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_varint(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, 0);
    }

  if (ret < 0)
    {
      return ret;
    }

  esp_hosted_transport_put_le16(payload + 1, rpc_length);
  packet_length = 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES + 3 +
                  rpc_length;
  esp_hosted_transport_put_le16(packet + 2, packet_length);

  if (transport->checksum_enabled)
    {
      esp_hosted_transport_put_le16(packet + 6, 0);
      esp_hosted_transport_put_le16(
        packet + 6, esp_hosted_transport_checksum(
                      packet, ESP_HOSTED_TRANSPORT_HEADER_BYTES +
                      packet_length));
    }

  packet_length += ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  ret = esp_hosted_transport_send_packet(transport, packet, packet_length);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 RPC: %s sent uid=%" PRIu32 " bytes=%u\n",
         name, uid, (unsigned int)packet_length);
  return OK;
}

static int esp_hosted_transport_send_wifi_init(
  FAR struct esp_hosted_transport_s *transport,
  FAR const struct esp_hosted_wifi_init_config_s *config, uint32_t uid)
{
  static const uint8_t endpoint[] = "RPCRsp";
  uint8_t packet[ESP_HOSTED_TRANSPORT_RPC_PACKET_MAX];
  uint8_t wifi_config[ESP_HOSTED_TRANSPORT_WIFI_CONFIG_MAX];
  uint8_t wifi_request[ESP_HOSTED_TRANSPORT_WIFI_CONFIG_MAX + 8];
  FAR uint8_t *payload;
  FAR uint8_t *rpc;
  size_t config_length = 0;
  size_t request_length = 0;
  size_t rpc_length = 0;
  size_t packet_length;
  int ret;

  const struct esp_hosted_wifi_config_field_s fields[] =
  {
    { 1, (uint32_t)config->static_rx_buf_num },
    { 2, (uint32_t)config->dynamic_rx_buf_num },
    { 3, (uint32_t)config->tx_buf_type },
    { 4, (uint32_t)config->static_tx_buf_num },
    { 5, (uint32_t)config->dynamic_tx_buf_num },
    { 6, (uint32_t)config->cache_tx_buf_num },
    { 7, (uint32_t)config->csi_enable },
    { 8, (uint32_t)config->ampdu_rx_enable },
    { 9, (uint32_t)config->ampdu_tx_enable },
    { 10, (uint32_t)config->amsdu_tx_enable },
    { 11, (uint32_t)config->nvs_enable },
    { 12, (uint32_t)config->nano_enable },
    { 13, (uint32_t)config->rx_ba_win },
    { 14, (uint32_t)config->wifi_task_core_id },
    { 15, (uint32_t)config->beacon_max_len },
    { 16, (uint32_t)config->mgmt_sbuf_num },
    { 17, config->feature_caps },
    { 18, config->sta_disconnected_pm },
    { 19, (uint32_t)config->espnow_max_encrypt_num },
    { 20, (uint32_t)config->magic },
    { 21, (uint32_t)config->rx_mgmt_buf_type },
    { 22, (uint32_t)config->rx_mgmt_buf_num },
    { 23, (uint32_t)config->tx_hetb_queue_num },
    { 24, (uint32_t)config->dump_hesigb_enable },
  };
  size_t index;

  for (index = 0; index < sizeof(fields) / sizeof(fields[0]); index++)
    {
      ret = esp_hosted_transport_append_field(
        wifi_config, sizeof(wifi_config), &config_length, fields[index].field,
        fields[index].value);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* Rpc.req_wifi_init is an RpcReqWifiInit message, whose cfg member is
   * another length-delimited WifiInitConfig message.  GetWifiMode has an
   * empty request body, so this wrapper was not needed by that first RPC.
   */

  ret = esp_hosted_transport_append_varint(
    wifi_request, sizeof(wifi_request), &request_length, (1 << 3) | 2);
  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_varint(
        wifi_request, sizeof(wifi_request), &request_length, config_length);
    }

  if (ret < 0 || config_length > sizeof(wifi_request) - request_length)
    {
      return ret < 0 ? ret : -EMSGSIZE;
    }

  memcpy(wifi_request + request_length, wifi_config, config_length);
  request_length += config_length;

  memset(packet, 0, sizeof(packet));
  packet[0] = ESP_HOSTED_TRANSPORT_IF_SERIAL;
  esp_hosted_transport_put_le16(packet + 4,
                                ESP_HOSTED_TRANSPORT_HEADER_BYTES);
  esp_hosted_transport_put_le16(
    packet + 8, esp_hosted_transport_next_tx_sequence(transport));

  payload = packet + ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  payload[0] = ESP_HOSTED_TRANSPORT_TLV_EPNAME;
  esp_hosted_transport_put_le16(
    payload + 1, ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES);
  memcpy(payload + 3, endpoint, sizeof(endpoint) - 1);
  payload += 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES;
  payload[0] = ESP_HOSTED_TRANSPORT_TLV_DATA;
  rpc = payload + 3;

  ret = esp_hosted_transport_append_field(
    rpc, sizeof(packet) - (rpc - packet), &rpc_length, 1,
    ESP_HOSTED_TRANSPORT_RPC_TYPE_REQ);
  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_field(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, 2,
        ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_INIT);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_field(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, 3, uid);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_varint(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length,
        ((uint64_t)ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_INIT << 3) | 2);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_varint(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, request_length);
    }

  if (ret < 0 || request_length > sizeof(packet) - (rpc - packet) -
                                    rpc_length)
    {
      return ret < 0 ? ret : -EMSGSIZE;
    }

  memcpy(rpc + rpc_length, wifi_request, request_length);
  rpc_length += request_length;
  esp_hosted_transport_put_le16(payload + 1, rpc_length);
  packet_length = 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES + 3 +
                  rpc_length;
  esp_hosted_transport_put_le16(packet + 2, packet_length);

  if (transport->checksum_enabled)
    {
      esp_hosted_transport_put_le16(packet + 6, 0);
      esp_hosted_transport_put_le16(
        packet + 6, esp_hosted_transport_checksum(
                      packet, ESP_HOSTED_TRANSPORT_HEADER_BYTES +
                      packet_length));
    }

  packet_length += ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  ret = esp_hosted_transport_send_packet(transport, packet, packet_length);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 RPC: WifiInit sent uid=%" PRIu32
         " bytes=%u config_bytes=%u\n", uid, (unsigned int)packet_length,
         (unsigned int)config_length);
  return OK;
}

static int esp_hosted_transport_send_sta_config(
  FAR struct esp_hosted_transport_s *transport, FAR const char *ssid,
  FAR const char *password, uint32_t uid)
{
  static const uint8_t endpoint[] = "RPCRsp";
  uint8_t packet[ESP_HOSTED_TRANSPORT_RPC_PACKET_MAX];
  uint8_t sta_config[ESP_HOSTED_TRANSPORT_WIFI_CONFIG_MAX];
  uint8_t wifi_config[ESP_HOSTED_TRANSPORT_WIFI_CONFIG_MAX + 8];
  uint8_t request[ESP_HOSTED_TRANSPORT_WIFI_CONFIG_MAX + 16];
  FAR uint8_t *payload;
  FAR uint8_t *rpc;
  size_t ssid_length;
  size_t password_length;
  size_t sta_length = 0;
  size_t config_length = 0;
  size_t request_length = 0;
  size_t rpc_length = 0;
  size_t packet_length;
  int ret;

  if (ssid == NULL || password == NULL)
    {
      return -EINVAL;
    }

  ssid_length = strlen(ssid);
  password_length = strlen(password);
  if (ssid_length == 0 || ssid_length > ESP_HOSTED_TRANSPORT_STA_SSID_MAX ||
      password_length > ESP_HOSTED_TRANSPORT_STA_PASSWORD_MAX)
    {
      return -EINVAL;
    }

  /* Rpc_Req_WifiSetConfig.cfg is a wifi_config oneof.  Its STA branch
   * contains byte strings, so neither value has a terminating NUL in the
   * serialized request.
   */

  ret = esp_hosted_transport_append_bytes(
    sta_config, sizeof(sta_config), &sta_length, 1,
    (FAR const uint8_t *)ssid, ssid_length);
  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_bytes(
        sta_config, sizeof(sta_config), &sta_length, 2,
        (FAR const uint8_t *)password, password_length);
    }

  /* Match the official host's message presence even when all nested
   * values are zero.  Older coprocessor handlers dereference threshold
   * and pmf_cfg without NULL checks.  append_bytes() deliberately omits
   * empty strings, so it cannot encode these default-valued messages.
   */

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_empty_message(
        sta_config, sizeof(sta_config), &sta_length, 9);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_empty_message(
        sta_config, sizeof(sta_config), &sta_length, 10);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_bytes(
        wifi_config, sizeof(wifi_config), &config_length, 2,
        sta_config, sta_length);
    }

  /* ESP_WIFI_STA is zero.  Protobuf omits its default-valued iface field,
   * and the coprocessor therefore receives the documented STA default.
   */

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_bytes(
        request, sizeof(request), &request_length, 2,
        wifi_config, config_length);
    }

  if (ret < 0)
    {
      return ret;
    }

  memset(packet, 0, sizeof(packet));
  packet[0] = ESP_HOSTED_TRANSPORT_IF_SERIAL;
  esp_hosted_transport_put_le16(packet + 4,
                                ESP_HOSTED_TRANSPORT_HEADER_BYTES);
  esp_hosted_transport_put_le16(
    packet + 8, esp_hosted_transport_next_tx_sequence(transport));

  payload = packet + ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  payload[0] = ESP_HOSTED_TRANSPORT_TLV_EPNAME;
  esp_hosted_transport_put_le16(
    payload + 1, ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES);
  memcpy(payload + 3, endpoint, sizeof(endpoint) - 1);
  payload += 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES;
  payload[0] = ESP_HOSTED_TRANSPORT_TLV_DATA;
  rpc = payload + 3;

  ret = esp_hosted_transport_append_field(
    rpc, sizeof(packet) - (rpc - packet), &rpc_length, 1,
    ESP_HOSTED_TRANSPORT_RPC_TYPE_REQ);
  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_field(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, 2,
        ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_SET_CONFIG);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_field(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, 3, uid);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_varint(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length,
        ((uint64_t)ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_SET_CONFIG << 3) | 2);
    }

  if (ret >= 0)
    {
      ret = esp_hosted_transport_append_varint(
        rpc, sizeof(packet) - (rpc - packet), &rpc_length, request_length);
    }

  if (ret < 0 || request_length > sizeof(packet) - (rpc - packet) -
                                    rpc_length)
    {
      return ret < 0 ? ret : -EMSGSIZE;
    }

  memcpy(rpc + rpc_length, request, request_length);
  rpc_length += request_length;
  esp_hosted_transport_put_le16(payload + 1, rpc_length);
  packet_length = 3 + ESP_HOSTED_TRANSPORT_RPC_ENDPOINT_BYTES + 3 +
                  rpc_length;
  esp_hosted_transport_put_le16(packet + 2, packet_length);

  if (transport->checksum_enabled)
    {
      esp_hosted_transport_put_le16(packet + 6, 0);
      esp_hosted_transport_put_le16(
        packet + 6, esp_hosted_transport_checksum(
                      packet, ESP_HOSTED_TRANSPORT_HEADER_BYTES +
                      packet_length));
    }

  packet_length += ESP_HOSTED_TRANSPORT_HEADER_BYTES;
  ret = esp_hosted_transport_send_packet(transport, packet, packet_length);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 RPC: WifiSetConfig sent uid=%" PRIu32
         " bytes=%u ssid_bytes=%u password_bytes=%u\n", uid,
         (unsigned int)packet_length, (unsigned int)ssid_length,
         (unsigned int)password_length);
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

  memset(&g_transport, 0, sizeof(g_transport));
  g_transport.sdio = sdio;
  spin_lock_init(&g_transport.tx_lock);
  ret = nxmutex_init(&g_transport.bus_lock);
  if (ret < 0)
    {
      goto deinitialize;
    }

  ret = nxmutex_init(&g_transport.rpc_lock);
  if (ret < 0)
    {
      goto destroy_bus_lock;
    }

  ret = nxsem_init(&g_transport.rpc_sem, 0, 0);
  if (ret < 0)
    {
      goto destroy_rpc_lock;
    }

  ret = nxsem_set_protocol(&g_transport.rpc_sem, SEM_PRIO_NONE);
  if (ret < 0)
    {
      goto destroy_rpc_sem;
    }

  ret = nxsem_init(&g_transport.sta_start_sem, 0, 0);
  if (ret < 0)
    {
      goto destroy_rpc_sem;
    }

  ret = nxsem_set_protocol(&g_transport.sta_start_sem, SEM_PRIO_NONE);
  if (ret < 0)
    {
      goto destroy_sta_start_sem;
    }

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

destroy_sta_start_sem:
  nxsem_destroy(&g_transport.sta_start_sem);
destroy_rpc_sem:
  nxsem_destroy(&g_transport.rpc_sem);
destroy_rpc_lock:
  nxmutex_destroy(&g_transport.rpc_lock);
destroy_bus_lock:
  nxmutex_destroy(&g_transport.bus_lock);
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

  transport->rx_active = false;
  work_cancel_sync(LPWORK, &transport->rx_work);
  ret = esp_hosted_sdio_deinitialize(transport->sdio);
  if (ret == OK)
    {
      nxsem_destroy(&transport->sta_start_sem);
      nxsem_destroy(&transport->rpc_sem);
      nxmutex_destroy(&transport->rpc_lock);
      nxmutex_destroy(&transport->bus_lock);
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

      transport->checksum_enabled =
        (info->capabilities & ESP_HOSTED_TRANSPORT_CAP_CHECKSUM) != 0;
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

int esp_hosted_transport_start_rx(
  FAR struct esp_hosted_transport_s *transport)
{
  int ret;

  if (transport != &g_transport || !transport->data_path_open)
    {
      return -EPIPE;
    }

  if (transport->rx_active)
    {
      return -EALREADY;
    }

  transport->rx_active = true;
  ret = work_queue(LPWORK, &transport->rx_work,
                   esp_hosted_transport_rx_worker, transport, 0);
  if (ret < 0)
    {
      transport->rx_active = false;
    }

  return ret;
}

int esp_hosted_transport_get_wifi_mode(
  FAR struct esp_hosted_transport_s *transport, FAR uint32_t *mode,
  FAR int *remote_result)
{
  uint32_t uid;
  int ret;

  if (transport != &g_transport || mode == NULL || remote_result == NULL ||
      !transport->data_path_open || !transport->rx_active)
    {
      return -EPIPE;
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->rpc_pending)
    {
      nxmutex_unlock(&transport->rpc_lock);
      return -EBUSY;
    }

  while (nxsem_trywait(&transport->rpc_sem) == OK)
    {
    }

  uid = ++transport->rpc_uid;
  if (uid == 0)
    {
      uid = ++transport->rpc_uid;
    }

  transport->rpc_uid = uid;
  transport->rpc_response_id = ESP_HOSTED_TRANSPORT_RPC_RESP_GET_MODE;
  transport->rpc_result = -ETIMEDOUT;
  transport->rpc_pending = true;
  ret = esp_hosted_transport_send_get_mode(transport, uid);
  if (ret < 0)
    {
      transport->rpc_pending = false;
      nxmutex_unlock(&transport->rpc_lock);
      return ret;
    }

  nxmutex_unlock(&transport->rpc_lock);
  ret = nxsem_tickwait_uninterruptible(
    &transport->rpc_sem, MSEC2TICK(ESP_HOSTED_TRANSPORT_RPC_TIMEOUT_MS));

  if (nxmutex_lock(&transport->rpc_lock) < 0)
    {
      return ret < 0 ? ret : -EIO;
    }

  if (transport->rpc_pending)
    {
      transport->rpc_pending = false;
      if (ret == OK)
        {
          ret = -EIO;
        }
    }
  else if (ret == OK)
    {
      *mode = transport->rpc_wifi_mode;
      *remote_result = transport->rpc_result;
    }

  nxmutex_unlock(&transport->rpc_lock);
  return ret;
}

int esp_hosted_transport_get_sta_mac(
  FAR struct esp_hosted_transport_s *transport, FAR uint8_t mac[6],
  FAR int *remote_result)
{
  uint32_t uid;
  int ret;

  if (transport != &g_transport || mac == NULL || remote_result == NULL ||
      !transport->data_path_open || !transport->rx_active)
    {
      return -EPIPE;
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->rpc_pending)
    {
      nxmutex_unlock(&transport->rpc_lock);
      return -EBUSY;
    }

  while (nxsem_trywait(&transport->rpc_sem) == OK)
    {
    }

  uid = ++transport->rpc_uid;
  if (uid == 0)
    {
      uid = ++transport->rpc_uid;
    }

  transport->rpc_uid = uid;
  transport->rpc_response_id = ESP_HOSTED_TRANSPORT_RPC_RESP_GET_MAC;
  transport->rpc_result = -ETIMEDOUT;
  transport->rpc_pending = true;
  ret = esp_hosted_transport_send_empty_request(
    transport, ESP_HOSTED_TRANSPORT_RPC_REQ_GET_MAC, "GetMACAddress", uid);
  if (ret < 0)
    {
      transport->rpc_pending = false;
      nxmutex_unlock(&transport->rpc_lock);
      return ret;
    }

  nxmutex_unlock(&transport->rpc_lock);
  ret = nxsem_tickwait_uninterruptible(
    &transport->rpc_sem, MSEC2TICK(ESP_HOSTED_TRANSPORT_RPC_TIMEOUT_MS));

  if (nxmutex_lock(&transport->rpc_lock) < 0)
    {
      return ret < 0 ? ret : -EIO;
    }

  if (transport->rpc_pending)
    {
      transport->rpc_pending = false;
      if (ret == OK)
        {
          ret = -EIO;
        }
    }
  else if (ret == OK)
    {
      memcpy(mac, transport->rpc_mac, sizeof(transport->rpc_mac));
      *remote_result = transport->rpc_result;
    }

  nxmutex_unlock(&transport->rpc_lock);
  return ret;
}

int esp_hosted_transport_wifi_initialize(
  FAR struct esp_hosted_transport_s *transport,
  FAR const struct esp_hosted_wifi_init_config_s *config,
  FAR int *remote_result)
{
  uint32_t uid;
  int ret;

  if (transport != &g_transport || config == NULL || remote_result == NULL ||
      !transport->data_path_open || !transport->rx_active)
    {
      return -EPIPE;
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->rpc_pending)
    {
      nxmutex_unlock(&transport->rpc_lock);
      return -EBUSY;
    }

  while (nxsem_trywait(&transport->rpc_sem) == OK)
    {
    }

  uid = ++transport->rpc_uid;
  if (uid == 0)
    {
      uid = ++transport->rpc_uid;
    }

  transport->rpc_uid = uid;
  transport->rpc_response_id = ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_INIT;
  transport->rpc_result = -ETIMEDOUT;
  transport->rpc_pending = true;
  ret = esp_hosted_transport_send_wifi_init(transport, config, uid);
  if (ret < 0)
    {
      transport->rpc_pending = false;
      nxmutex_unlock(&transport->rpc_lock);
      return ret;
    }

  nxmutex_unlock(&transport->rpc_lock);
  ret = nxsem_tickwait_uninterruptible(
    &transport->rpc_sem, MSEC2TICK(ESP_HOSTED_TRANSPORT_RPC_TIMEOUT_MS));

  if (nxmutex_lock(&transport->rpc_lock) < 0)
    {
      return ret < 0 ? ret : -EIO;
    }

  if (transport->rpc_pending)
    {
      transport->rpc_pending = false;
      if (ret == OK)
        {
          ret = -EIO;
        }
    }
  else if (ret == OK)
    {
      *remote_result = transport->rpc_result;
    }

  nxmutex_unlock(&transport->rpc_lock);
  return ret;
}

static int esp_hosted_transport_scalar_rpc(
  FAR struct esp_hosted_transport_s *transport, uint32_t request_id,
  uint32_t response_id, FAR const char *name, uint32_t value,
  FAR int *remote_result)
{
  uint32_t uid;
  int ret;

  if (transport != &g_transport || remote_result == NULL ||
      !transport->data_path_open || !transport->rx_active)
    {
      return -EPIPE;
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->rpc_pending)
    {
      nxmutex_unlock(&transport->rpc_lock);
      return -EBUSY;
    }

  while (nxsem_trywait(&transport->rpc_sem) == OK)
    {
    }

  uid = ++transport->rpc_uid;
  if (uid == 0)
    {
      uid = ++transport->rpc_uid;
    }

  transport->rpc_uid = uid;
  transport->rpc_response_id = response_id;
  transport->rpc_result = -ETIMEDOUT;
  transport->rpc_pending = true;
  ret = esp_hosted_transport_send_scalar_request(transport, request_id,
                                                 name, value, uid);
  if (ret < 0)
    {
      transport->rpc_pending = false;
      nxmutex_unlock(&transport->rpc_lock);
      return ret;
    }

  nxmutex_unlock(&transport->rpc_lock);
  ret = nxsem_tickwait_uninterruptible(
    &transport->rpc_sem, MSEC2TICK(ESP_HOSTED_TRANSPORT_RPC_TIMEOUT_MS));

  if (nxmutex_lock(&transport->rpc_lock) < 0)
    {
      return ret < 0 ? ret : -EIO;
    }

  if (transport->rpc_pending)
    {
      transport->rpc_pending = false;
      if (ret == OK)
        {
          ret = -EIO;
        }
    }
  else if (ret == OK)
    {
      *remote_result = transport->rpc_result;
    }

  nxmutex_unlock(&transport->rpc_lock);
  return ret;
}

int esp_hosted_transport_set_wifi_mode(
  FAR struct esp_hosted_transport_s *transport, uint32_t mode,
  FAR int *remote_result)
{
  return esp_hosted_transport_scalar_rpc(
    transport, ESP_HOSTED_TRANSPORT_RPC_REQ_SET_MODE,
    ESP_HOSTED_TRANSPORT_RPC_RESP_SET_MODE, "SetWifiMode", mode,
    remote_result);
}

int esp_hosted_transport_set_wifi_storage_ram(
  FAR struct esp_hosted_transport_s *transport, FAR int *remote_result)
{
  /* ESP-IDF wifi_storage_t: WIFI_STORAGE_RAM = 1. */

  return esp_hosted_transport_scalar_rpc(
    transport, ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_SET_STORAGE,
    ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_SET_STORAGE, "WifiSetStorage", 1,
    remote_result);
}

int esp_hosted_transport_wifi_start(
  FAR struct esp_hosted_transport_s *transport, FAR int *remote_result)
{
  uint32_t uid;
  int ret;

  if (transport != &g_transport || remote_result == NULL ||
      !transport->data_path_open || !transport->rx_active)
    {
      return -EPIPE;
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->rpc_pending)
    {
      nxmutex_unlock(&transport->rpc_lock);
      return -EBUSY;
    }

  while (nxsem_trywait(&transport->rpc_sem) == OK)
    {
    }

  while (nxsem_trywait(&transport->sta_start_sem) == OK)
    {
    }

  transport->sta_started = false;
  uid = ++transport->rpc_uid;
  if (uid == 0)
    {
      uid = ++transport->rpc_uid;
    }

  transport->rpc_uid = uid;
  transport->rpc_response_id = ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_START;
  transport->rpc_result = -ETIMEDOUT;
  transport->rpc_pending = true;
  ret = esp_hosted_transport_send_empty_request(
    transport, ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_START, "WifiStart", uid);
  if (ret < 0)
    {
      transport->rpc_pending = false;
      nxmutex_unlock(&transport->rpc_lock);
      return ret;
    }

  nxmutex_unlock(&transport->rpc_lock);
  ret = nxsem_tickwait_uninterruptible(
    &transport->rpc_sem, MSEC2TICK(ESP_HOSTED_TRANSPORT_RPC_TIMEOUT_MS));

  if (nxmutex_lock(&transport->rpc_lock) < 0)
    {
      return ret < 0 ? ret : -EIO;
    }

  if (transport->rpc_pending)
    {
      transport->rpc_pending = false;
      if (ret == OK)
        {
          ret = -EIO;
        }
    }
  else if (ret == OK)
    {
      *remote_result = transport->rpc_result;
    }

  nxmutex_unlock(&transport->rpc_lock);
  return ret;
}

int esp_hosted_transport_set_sta_config(
  FAR struct esp_hosted_transport_s *transport, FAR const char *ssid,
  FAR const char *password, FAR int *remote_result)
{
  uint32_t uid;
  int ret;

  if (transport != &g_transport || remote_result == NULL ||
      !transport->data_path_open || !transport->rx_active)
    {
      return -EPIPE;
    }

  if (ssid == NULL || password == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->rpc_pending)
    {
      nxmutex_unlock(&transport->rpc_lock);
      return -EBUSY;
    }

  while (nxsem_trywait(&transport->rpc_sem) == OK)
    {
    }

  uid = ++transport->rpc_uid;
  if (uid == 0)
    {
      uid = ++transport->rpc_uid;
    }

  transport->rpc_uid = uid;
  transport->rpc_response_id = ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_SET_CONFIG;
  transport->rpc_result = -ETIMEDOUT;
  transport->rpc_pending = true;
  ret = esp_hosted_transport_send_sta_config(transport, ssid, password, uid);
  if (ret < 0)
    {
      transport->rpc_pending = false;
      nxmutex_unlock(&transport->rpc_lock);
      return ret;
    }

  nxmutex_unlock(&transport->rpc_lock);
  ret = nxsem_tickwait_uninterruptible(
    &transport->rpc_sem, MSEC2TICK(ESP_HOSTED_TRANSPORT_RPC_TIMEOUT_MS));

  if (nxmutex_lock(&transport->rpc_lock) < 0)
    {
      return ret < 0 ? ret : -EIO;
    }

  if (transport->rpc_pending)
    {
      transport->rpc_pending = false;
      if (ret == OK)
        {
          ret = -EIO;
        }
    }
  else if (ret == OK)
    {
      *remote_result = transport->rpc_result;
    }

  nxmutex_unlock(&transport->rpc_lock);
  return ret;
}

int esp_hosted_transport_wifi_connect(
  FAR struct esp_hosted_transport_s *transport, FAR int *remote_result)
{
  uint32_t uid;
  int ret;

  if (transport != &g_transport || remote_result == NULL ||
      !transport->data_path_open || !transport->rx_active)
    {
      return -EPIPE;
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->rpc_pending)
    {
      nxmutex_unlock(&transport->rpc_lock);
      return -EBUSY;
    }

  while (nxsem_trywait(&transport->rpc_sem) == OK)
    {
    }

  uid = ++transport->rpc_uid;
  if (uid == 0)
    {
      uid = ++transport->rpc_uid;
    }

  transport->rpc_uid = uid;
  transport->rpc_response_id = ESP_HOSTED_TRANSPORT_RPC_RESP_WIFI_CONNECT;
  transport->rpc_result = -ETIMEDOUT;
  transport->rpc_pending = true;
  ret = esp_hosted_transport_send_empty_request(
    transport, ESP_HOSTED_TRANSPORT_RPC_REQ_WIFI_CONNECT, "WifiConnect",
    uid);
  if (ret < 0)
    {
      transport->rpc_pending = false;
      nxmutex_unlock(&transport->rpc_lock);
      return ret;
    }

  nxmutex_unlock(&transport->rpc_lock);
  ret = nxsem_tickwait_uninterruptible(
    &transport->rpc_sem, MSEC2TICK(ESP_HOSTED_TRANSPORT_RPC_TIMEOUT_MS));

  if (nxmutex_lock(&transport->rpc_lock) < 0)
    {
      return ret < 0 ? ret : -EIO;
    }

  if (transport->rpc_pending)
    {
      transport->rpc_pending = false;
      if (ret == OK)
        {
          ret = -EIO;
        }
    }
  else if (ret == OK)
    {
      *remote_result = transport->rpc_result;
    }

  nxmutex_unlock(&transport->rpc_lock);
  return ret;
}

int esp_hosted_transport_wait_sta_start(
  FAR struct esp_hosted_transport_s *transport)
{
  int ret;

  if (transport != &g_transport || !transport->data_path_open ||
      !transport->rx_active)
    {
      return -EPIPE;
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (transport->sta_started)
    {
      nxmutex_unlock(&transport->rpc_lock);
      return OK;
    }

  nxmutex_unlock(&transport->rpc_lock);
  return nxsem_tickwait_uninterruptible(
    &transport->sta_start_sem,
    MSEC2TICK(ESP_HOSTED_TRANSPORT_RPC_TIMEOUT_MS));
}

int esp_hosted_transport_register_wlan_rx(
  FAR struct esp_hosted_transport_s *transport,
  esp_hosted_transport_wlan_rx_t callback, FAR void *arg)
{
  int ret;

  if (transport != &g_transport || !transport->initialized)
    {
      return -EPIPE;
    }

  /* Removal must remain possible after RX faults.  The same rpc_lock is
   * held while invoking the callback, so clearing it waits for any current
   * consumer before the WLAN adapter is freed.
   */

  if (callback != NULL &&
      (!transport->data_path_open || !transport->rx_active))
    {
      return -EPIPE;
    }

  ret = nxmutex_lock(&transport->rpc_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (callback != NULL && transport->wlan_rx != NULL &&
      transport->wlan_rx != callback)
    {
      nxmutex_unlock(&transport->rpc_lock);
      return -EBUSY;
    }

  transport->wlan_rx = callback;
  transport->wlan_rx_arg = arg;
  nxmutex_unlock(&transport->rpc_lock);
  return OK;
}

int esp_hosted_transport_send_wlan(
  FAR struct esp_hosted_transport_s *transport, FAR const uint8_t *data,
  size_t length)
{
  FAR uint8_t *packet;
  size_t packet_length;
  int ret;

  if (transport != &g_transport || data == NULL || length == 0 ||
      !transport->data_path_open || !transport->rx_active)
    {
      return -EPIPE;
    }

  if (length > ESP_HOSTED_TRANSPORT_TX_BUFFER_BYTES -
               ESP_HOSTED_TRANSPORT_HEADER_BYTES)
    {
      return -EMSGSIZE;
    }

  ret = nxmutex_lock(&transport->bus_lock);
  if (ret < 0)
    {
      return ret;
    }

  packet = transport->tx_packet;
  memset(packet, 0, sizeof(transport->tx_packet));
  packet[0] = ESP_HOSTED_TRANSPORT_IF_STA;
  esp_hosted_transport_put_le16(packet + 2, length);
  esp_hosted_transport_put_le16(packet + 4,
                                ESP_HOSTED_TRANSPORT_HEADER_BYTES);
  esp_hosted_transport_put_le16(
    packet + 8, esp_hosted_transport_next_tx_sequence(transport));
  memcpy(packet + ESP_HOSTED_TRANSPORT_HEADER_BYTES, data, length);
  packet_length = ESP_HOSTED_TRANSPORT_HEADER_BYTES + length;

  if (transport->checksum_enabled)
    {
      esp_hosted_transport_put_le16(packet + 6, 0);
      esp_hosted_transport_put_le16(
        packet + 6, esp_hosted_transport_checksum(packet, packet_length));
    }

  ret = esp_hosted_transport_send_packet_locked(transport, packet,
                                                 packet_length);
  nxmutex_unlock(&transport->bus_lock);
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
