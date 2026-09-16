/****************************************************************************
 * chips/esp32p4/include/esp_hosted_transport.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_TRANSPORT_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_TRANSPORT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

#include <arch/chip/esp_hosted_sdio.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct esp_hosted_transport_s;

typedef int (*esp_hosted_transport_wlan_rx_t)(FAR void *arg,
                                               FAR const uint8_t *data,
                                               size_t length);

struct esp_hosted_transport_config_s
{
  struct esp_hosted_sdio_config_s sdio;
};

/* The Wi-Fi configuration is owned by the board because it must match the
 * ESP-Hosted coprocessor firmware.  The transport only serializes it into a
 * Req_WifiInit RPC.
 */

struct esp_hosted_wifi_init_config_s
{
  int32_t static_rx_buf_num;
  int32_t dynamic_rx_buf_num;
  int32_t tx_buf_type;
  int32_t static_tx_buf_num;
  int32_t dynamic_tx_buf_num;
  int32_t cache_tx_buf_num;
  int32_t csi_enable;
  int32_t ampdu_rx_enable;
  int32_t ampdu_tx_enable;
  int32_t amsdu_tx_enable;
  int32_t nvs_enable;
  int32_t nano_enable;
  int32_t rx_ba_win;
  int32_t wifi_task_core_id;
  int32_t beacon_max_len;
  int32_t mgmt_sbuf_num;
  uint64_t feature_caps;
  uint8_t sta_disconnected_pm;
  int32_t espnow_max_encrypt_num;
  int32_t magic;
  int32_t rx_mgmt_buf_type;
  int32_t rx_mgmt_buf_num;
  int32_t tx_hetb_queue_num;
  int32_t dump_hesigb_enable;
};

/* Information advertised by the ESP-Hosted private initialization event. */

struct esp_hosted_init_info_s
{
  uint32_t firmware_version;
  uint8_t  capabilities;
  uint8_t  chip_id;
  uint8_t  rx_queue_size;
  uint8_t  tx_queue_size;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_transport_initialize
 *
 * Description:
 *   Enumerate an ESP-Hosted SDIO coprocessor after the board has configured
 *   the P4 SDMMC pins and released the coprocessor reset.  The current
 *   control-plane implementation performs CMD0, CMD5, CMD3 (RCA), CMD7
 *   (select card) and a function-0 CMD52 read.  It does not register a
 *   wireless network device.
 *
 ****************************************************************************/

int esp_hosted_transport_initialize(
  FAR const struct esp_hosted_transport_config_s *config,
  FAR struct esp_hosted_transport_s **transport);

/****************************************************************************
 * Name: esp_hosted_transport_deinitialize
 *
 * Description:
 *   Release an initialized ESP-Hosted transport after a data-phase failure
 *   or before the board resets the coprocessor.  The caller must discard its
 *   transport pointer when this function returns OK.
 *
 ****************************************************************************/

int esp_hosted_transport_deinitialize(
  FAR struct esp_hosted_transport_s *transport);

/****************************************************************************
 * Name: esp_hosted_transport_start
 *
 * Description:
 *   Open the ESP-Hosted SDIO data path and wait for one private
 *   initialization event.  The transport validates the Function-1 FIFO
 *   packet format, returns the C6 firmware capabilities and sends the
 *   matching host configuration event.  It does not register wlan0 or issue
 *   Wi-Fi RPC commands.
 *
 ****************************************************************************/

int esp_hosted_transport_start(FAR struct esp_hosted_transport_s *transport,
                               FAR struct esp_hosted_init_info_s *info);

/****************************************************************************
 * Name: esp_hosted_transport_start_rx
 *
 * Description:
 *   Start the persistent Function-1 receive path.  The initial implementation
 *   polls the C6 status registers from LPWORK, keeps FIFO and command traffic
 *   out of interrupt context, and dispatches ESP-Hosted serial control
 *   responses.  The caller must have completed esp_hosted_transport_start().
 *
 ****************************************************************************/

int esp_hosted_transport_start_rx(
  FAR struct esp_hosted_transport_s *transport);

/****************************************************************************
 * Name: esp_hosted_transport_get_wifi_mode
 *
 * Description:
 *   Send the ESP-Hosted Req_GetWifiMode RPC and wait for its matching
 *   Resp_GetWifiMode response.  Return transport or protocol failures through
 *   the function result and return the C6 service result separately through
 *   remote_result.  This is a read-only control-plane request and is used to
 *   validate the SDIO receive path before Wi-Fi state changes or a network
 *   device are introduced.
 *
 ****************************************************************************/

int esp_hosted_transport_get_wifi_mode(
  FAR struct esp_hosted_transport_s *transport, FAR uint32_t *mode,
  FAR int *remote_result);

/****************************************************************************
 * Name: esp_hosted_transport_wifi_initialize
 *
 * Description:
 *   Send Req_WifiInit with the board-selected ESP32-C6 Wi-Fi configuration
 *   and wait for Resp_WifiInit.  The return value reports local transport or
 *   protocol failures.  The C6 esp_wifi_init() result is returned through
 *   remote_result.
 *
 ****************************************************************************/

int esp_hosted_transport_wifi_initialize(
  FAR struct esp_hosted_transport_s *transport,
  FAR const struct esp_hosted_wifi_init_config_s *config,
  FAR int *remote_result);

/****************************************************************************
 * Name: esp_hosted_transport_set_wifi_mode
 *
 * Description:
 *   Send Req_SetWifiMode and wait for its matching response.  The caller
 *   supplies an ESP-Hosted Wi-Fi mode value.  The return value reports local
 *   transport or protocol failures; the C6 service result is returned
 *   through remote_result.
 *
 ****************************************************************************/

int esp_hosted_transport_set_wifi_mode(
  FAR struct esp_hosted_transport_s *transport, uint32_t mode,
  FAR int *remote_result);

/****************************************************************************
 * Name: esp_hosted_transport_wifi_start
 *
 * Description:
 *   Send Req_WifiStart and wait for its matching Resp_WifiStart.  The return
 *   value reports local transport or protocol failures; the C6 esp_wifi_start
 *   result is returned through remote_result.  A successful response does not
 *   by itself prove that the selected Wi-Fi role started; callers must also
 *   wait for the corresponding event.
 *
 ****************************************************************************/

int esp_hosted_transport_wifi_start(
  FAR struct esp_hosted_transport_s *transport, FAR int *remote_result);

/****************************************************************************
 * Name: esp_hosted_transport_set_sta_config
 *
 * Description:
 *   Send Req_WifiSetConfig for ESP_WIFI_STA with the supplied SSID and
 *   password.  The byte strings are serialized without NUL terminators.  An
 *   empty password is valid for an open network; an empty SSID is rejected.
 *   The return value reports local transport or protocol failures and the C6
 *   service result is returned through remote_result.
 *
 ****************************************************************************/

int esp_hosted_transport_set_sta_config(
  FAR struct esp_hosted_transport_s *transport, FAR const char *ssid,
  FAR const char *password, FAR int *remote_result);

/****************************************************************************
 * Name: esp_hosted_transport_wifi_connect
 *
 * Description:
 *   Send Req_WifiConnect after Wi-Fi has started and a station configuration
 *   has been accepted.  A successful response only accepts the association
 *   attempt; connection and disconnection events are handled separately.
 *
 ****************************************************************************/

int esp_hosted_transport_wifi_connect(
  FAR struct esp_hosted_transport_s *transport, FAR int *remote_result);

/****************************************************************************
 * Name: esp_hosted_transport_wait_sta_start
 *
 * Description:
 *   Wait for Event_WifiEventNoArgs carrying WIFI_EVENT_STA_START after a
 *   successful station-mode WiFiStart request.  The event latch is cleared
 *   before each WiFiStart request, so a successful return proves the C6
 *   emitted a start event for that request.
 *
 ****************************************************************************/

int esp_hosted_transport_wait_sta_start(
  FAR struct esp_hosted_transport_s *transport);

/****************************************************************************
 * Name: esp_hosted_transport_get_sta_mac
 *
 * Description:
 *   Query the C6 station MAC address through Req_GetMACAddress.  A wireless
 *   netdevice must use this address as its link-layer address before it sends
 *   frames through the C6 station interface.
 *
 ****************************************************************************/

int esp_hosted_transport_get_sta_mac(
  FAR struct esp_hosted_transport_s *transport, FAR uint8_t mac[6],
  FAR int *remote_result);

/****************************************************************************
 * Name: esp_hosted_transport_register_wlan_rx
 *
 * Description:
 *   Register or remove the consumer of received ESP-Hosted station frames.
 *   The callback runs in the persistent LPWORK receive path and must copy or
 *   queue the data before it returns.  A NULL callback removes the consumer
 *   even after RX stops, provided the transport is still initialized.  The
 *   caller must remove it before freeing the consumer or the transport.
 *
 ****************************************************************************/

int esp_hosted_transport_register_wlan_rx(
  FAR struct esp_hosted_transport_s *transport,
  esp_hosted_transport_wlan_rx_t callback, FAR void *arg);

/* Select volatile C6 credential storage after WifiInit and before setting
 * the station configuration.  The host must supply credentials each boot.
 */

int esp_hosted_transport_set_wifi_storage_ram(
  FAR struct esp_hosted_transport_s *transport, FAR int *remote_result);

/****************************************************************************
 * Name: esp_hosted_transport_send_wlan
 *
 * Description:
 *   Wrap an Ethernet frame in an ESP-Hosted station-interface packet and
 *   synchronously write it through Function 1.
 *
 ****************************************************************************/

int esp_hosted_transport_send_wlan(
  FAR struct esp_hosted_transport_s *transport, FAR const uint8_t *data,
  size_t length);

/****************************************************************************
 * Name: esp_hosted_transport_diagnose
 *
 * Description:
 *   Sample Function-1 status registers without writing a slave register,
 *   clearing an interrupt, opening the Hosted data path, or reading FIFO
 *   data.  It cross-checks individual CMD53 reads against CMD52 byte reads
 *   while diagnosing an unknown C6 firmware image.
 *
 ****************************************************************************/

int esp_hosted_transport_diagnose(
  FAR struct esp_hosted_transport_s *transport);

/* Function-1 access for the private Hosted protocol layer.  Callers must
 * serialize complete packet transactions, not only individual commands.
 */

int esp_hosted_transport_enable_function(
  FAR struct esp_hosted_transport_s *transport);
int esp_hosted_transport_read_reg(
  FAR struct esp_hosted_transport_s *transport, uint32_t address,
  FAR uint8_t *value);
int esp_hosted_transport_write_reg(
  FAR struct esp_hosted_transport_s *transport, uint32_t address,
  uint8_t value);
int esp_hosted_transport_transfer(
  FAR struct esp_hosted_transport_s *transport, bool write,
  uint32_t address, FAR void *buffer, size_t length, bool blocks);

/****************************************************************************
 * Name: esp_hosted_transport_get_sdio
 *
 * Description:
 *   Return the P4 SDMMC host associated with an initialized ESP-Hosted
 *   transport.  CMD53 transport and function-interrupt support use this
 *   handle in a later phase.
 *
 ****************************************************************************/

FAR struct esp_hosted_sdio_s *esp_hosted_transport_get_sdio(
  FAR struct esp_hosted_transport_s *transport);

#endif /* __ARCH_RISCV_SRC_ESP32P4_INCLUDE_ESP_HOSTED_TRANSPORT_H */
