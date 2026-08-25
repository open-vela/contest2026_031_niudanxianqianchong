/****************************************************************************
 * chips/esp32p4/common/espressif/esp_mipi_dsi.c
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

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>
#include <nuttx/spinlock.h>
#include <nuttx/video/mipi_display.h>
#include <nuttx/video/mipi_dsi.h>
#include <nuttx/wqueue.h>

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "esp_clk_tree.h"
#include "esp_err.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/periph_ctrl.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_brg_ll.h"
#include "hal/mipi_dsi_host_ll.h"
#include "hal/mipi_dsi_ll.h"
#include "hal/mipi_dsi_phy_ll.h"

#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
#  include "esp_cache.h"
#  include "esp_irq.h"
#  include "hal/dw_gdma_ll.h"
#  include "soc/reg_base.h"
#  include "soc/interrupts.h"
#endif

#include "esp_mipi_dsi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_MIPI_DSI_MIN_RATE_MBPS      80
#define ESP_MIPI_DSI_MAX_RATE_MBPS      1500
#define ESP_MIPI_DSI_MIN_PHY_REF_HZ      5000000
#define ESP_MIPI_DSI_MAX_PHY_REF_HZ     40000000
#define ESP_MIPI_DSI_DEFAULT_TIMEOUT_MS  CONFIG_ESPRESSIF_MIPI_DSI_TIMEOUT_MS
#define ESP_MIPI_DSI_POLL_US              100
#define ESP_MIPI_DSI_TIMEOUT_CLOCK_MHZ     10
#define ESP_MIPI_DSI_ESCAPE_CLOCK_MHZ      18
#define ESP_MIPI_DSI_MAX_READ_TIME          6000
#define ESP_MIPI_DSI_STOP_WAIT_TIME         0x3f
#define ESP_MIPI_DSI_MIN_DPI_CLOCK_HZ       1000000
#define ESP_MIPI_DSI_MAX_DPI_CLOCK_HZ     240000000
#define ESP_MIPI_DSI_DMA_TRANSFER_WIDTH_BYTES    8
#define ESP_MIPI_DSI_DMA_BURST_WORDS           256
#define ESP_MIPI_DSI_DMA_EMPTY_THRESHOLD       768
#define ESP_MIPI_DSI_DMA_BUFFER_ALIGNMENT        64
#define ESP_MIPI_DSI_DMA_CHANNEL                  0
#define ESP_MIPI_DSI_PHY_LOCK_MASK                (1u << 0)
#define ESP_MIPI_DSI_PHY_CLK_STOP_MASK            (1u << 2)
#define ESP_MIPI_DSI_PHY_DATA0_STOP_MASK          (1u << 4)
#define ESP_MIPI_DSI_PHY_DATA1_STOP_MASK          (1u << 7)
#define ESP_MIPI_DSI_PHY_STOP_MASK \
  (ESP_MIPI_DSI_PHY_CLK_STOP_MASK | \
   ESP_MIPI_DSI_PHY_DATA0_STOP_MASK | \
   ESP_MIPI_DSI_PHY_DATA1_STOP_MASK)
#define ESP_MIPI_DSI_PHY_SAMPLE_MAX_COUNT         100000
#define ESP_MIPI_DSI_PHY_SAMPLE_MAX_INTERVAL_US   10000

/* LP-transmit evidence sampling.  The window begins immediately after the
 * command header lands in the Host FIFO.  A 2ms window at 10us resolution
 * covers the LP escape transmission of a short or long packet while also
 * tolerating the PHY wake-up latency of the first command after idle.
 */

#define ESP_MIPI_DSI_TX_SAMPLE_COUNT     200
#define ESP_MIPI_DSI_TX_SAMPLE_INTERVAL_US   10

/* Keep transfer completion separate from faults in the diagnostic output.
 * The channel status bits are latched for polling only; DMA global interrupt
 * delivery remains disabled because this M1 scanout has no ISR yet.
 */

#define ESP_MIPI_DSI_DMA_DONE_EVENTS \
  (DW_GDMA_LL_CHANNEL_EVENT_BLOCK_TFR_DONE | \
   DW_GDMA_LL_CHANNEL_EVENT_DMA_TFR_DONE)

#define ESP_MIPI_DSI_DMA_ERROR_EVENTS \
  (DW_GDMA_LL_CHANNEL_EVENT_SRC_DEC_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_DST_DEC_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_SRC_SLV_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_DST_SLV_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_RD_DEC_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_WR_DEC_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_RD_SLV_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_LLI_WR_SLV_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_SHADOWREG_OR_LLI_INVALID_ERR | \
   DW_GDMA_LL_CHANNEL_EVENT_ABORTED)

#define ESP_MIPI_DSI_DMA_EVENTS \
  (ESP_MIPI_DSI_DMA_DONE_EVENTS | ESP_MIPI_DSI_DMA_ERROR_EVENTS)

/* The P4 DSI DPI clock defaults to PLL_F240M.  A requested 52 MHz pixel
 * clock therefore becomes 48 MHz with divider 5; the timing helper applies
 * the matching horizontal compensation to preserve the frame rate.
 */

#define ESP_MIPI_DSI_DPI_CLK_SRC SOC_MOD_CLK_PLL_F240M

/* ESP32-P4 revision 3.0 and later use a different D-PHY PLL reference
 * clock mux.  The P4X board uses a revision 3.x chip, which requires XTAL
 * instead of the legacy PLL_F20M selection.
 */

#if defined(CONFIG_ESP32P4_REV_MIN_301)
#  define ESP_MIPI_DSI_PHY_PLLREF_CLK_SRC \
  MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT
#else
#  define ESP_MIPI_DSI_PHY_PLLREF_CLK_SRC \
  MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY
#endif

/* MIPI DSI protocol data type: Set Maximum Return Packet Size. */

#define ESP_MIPI_DSI_DT_SET_MAX_RETURN_PACKET_SIZE 0x37

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_mipi_dsi_s
{
  struct mipi_dsi_host       host;
  mipi_dsi_hal_context_t     hal;
  FAR struct esp_ldo_channel_s *phy_ldo;
  soc_module_clk_t           phy_cfg_clk_src;
  soc_module_clk_t           phy_pllref_clk_src;
  mutex_t                    lock;
  clock_t                    timeout_ticks;
  bool                       registered;
  bool                       ready;
  bool                       video_running;
  soc_module_clk_t           dpi_clk_src;
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
  FAR dw_gdma_dev_t          *dma_dev;
  dw_gdma_link_list_item_t   dma_lli;
  FAR const void             *dma_active_frame_buffer;
  FAR const void             *dma_pending_frame_buffer;
  size_t                     dma_frame_buffer_bytes;
  volatile uint32_t          dma_frame_count;
  volatile uint32_t          dma_error_events;
  int                        dma_cpuint;
  spinlock_t                 dma_irq_lock;
  esp_mipi_dsi_video_dma_frame_done_t dma_frame_done;
  FAR void                   *dma_frame_done_arg;
  int                        bridge_cpuint;
  spinlock_t                 bridge_irq_lock;
  struct work_s              bridge_underrun_work;
  uint32_t                   bridge_underrun_count;
  uint32_t                   bridge_first_underrun_raw;
  uint32_t                   bridge_first_underrun_status;
  uint32_t                   bridge_first_underrun_frame;
  bool                       bridge_first_underrun_seen;
  bool                       bridge_underrun_report_pending;
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int esp_mipi_dsi_attach(FAR struct mipi_dsi_host *host,
                               FAR struct mipi_dsi_device *device);
static int esp_mipi_dsi_detach(FAR struct mipi_dsi_host *host,
                               FAR struct mipi_dsi_device *device);
static ssize_t esp_mipi_dsi_transfer(FAR struct mipi_dsi_host *host,
                                     FAR const struct mipi_dsi_msg *msg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct mipi_dsi_host_ops g_esp_mipi_dsi_ops =
{
  .attach   = esp_mipi_dsi_attach,
  .detach   = esp_mipi_dsi_detach,
  .transfer = esp_mipi_dsi_transfer,
};

static struct esp_mipi_dsi_s g_esp_mipi_dsi =
{
  .host =
    {
      .bus = ESP_MIPI_DSI_BUS0,
      .ops = &g_esp_mipi_dsi_ops,
    },
  .lock = NXMUTEX_INITIALIZER,
  .phy_cfg_clk_src = SOC_MOD_CLK_INVALID,
  .phy_pllref_clk_src = SOC_MOD_CLK_INVALID,
  .dpi_clk_src = SOC_MOD_CLK_INVALID,
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
  .dma_cpuint = -1,
  .bridge_cpuint = -1,
  .dma_irq_lock = SP_UNLOCKED,
  .bridge_irq_lock = SP_UNLOCKED,
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool esp_mipi_dsi_timeout(FAR struct esp_mipi_dsi_s *priv,
                                 clock_t start)
{
  return (clock_systime_ticks() - start) >= priv->timeout_ticks;
}

static int esp_mipi_dsi_clock_result(esp_err_t result)
{
  if (result == ESP_OK)
    {
      return OK;
    }

  if (result == ESP_ERR_INVALID_ARG)
    {
      return -EINVAL;
    }

  if (result == ESP_ERR_INVALID_STATE)
    {
      return -EALREADY;
    }

  if (result == ESP_ERR_NO_MEM)
    {
      return -ENOMEM;
    }

  if (result == ESP_ERR_NOT_FOUND)
    {
      return -ENOSPC;
    }

  return -EIO;
}

static void esp_mipi_dsi_dump_status(FAR struct esp_mipi_dsi_s *priv,
                                     FAR const char *stage)
{
  FAR dsi_host_dev_t *host = priv->hal.host;

  if (host == NULL)
    {
      syslog(LOG_ERR, "ERROR: MIPI-DSI timeout stage=%s host unavailable\n",
             stage);
      return;
    }

  syslog(LOG_ERR,
         "ERROR: MIPI-DSI timeout stage=%s int_st0=%08" PRIx32
         " int_st1=%08" PRIx32 " phy_status=%08" PRIx32
         " cmd_pkt_status=%08" PRIx32 "\n",
         stage, host->int_st0.val, host->int_st1.val,
         host->phy_status.val, host->cmd_pkt_status.val);
}

static int esp_mipi_dsi_wait_while(FAR struct esp_mipi_dsi_s *priv,
                                   bool (*predicate)(dsi_host_dev_t *),
                                   FAR const char *stage)
{
  clock_t start = clock_systime_ticks();

  while (predicate(priv->hal.host))
    {
      if (esp_mipi_dsi_timeout(priv, start))
        {
          esp_mipi_dsi_dump_status(priv, stage);
          return -ETIMEDOUT;
        }

      nxsig_usleep(ESP_MIPI_DSI_POLL_US);
    }

  return OK;
}

static int esp_mipi_dsi_wait_pll(FAR struct esp_mipi_dsi_s *priv)
{
  clock_t start = clock_systime_ticks();

  while (!mipi_dsi_phy_ll_is_pll_locked(priv->hal.host))
    {
      if (esp_mipi_dsi_timeout(priv, start))
        {
          esp_mipi_dsi_dump_status(priv, "phy_pll_lock");
          return -ETIMEDOUT;
        }

      nxsig_usleep(ESP_MIPI_DSI_POLL_US);
    }

  return OK;
}

static int esp_mipi_dsi_wait_lanes_stopped(
  FAR struct esp_mipi_dsi_s *priv, uint8_t lane_num)
{
  clock_t start = clock_systime_ticks();

  while (!mipi_dsi_phy_ll_are_lanes_stopped(priv->hal.host, lane_num))
    {
      if (esp_mipi_dsi_timeout(priv, start))
        {
          esp_mipi_dsi_dump_status(priv, "phy_lanes_stop");
          return -ETIMEDOUT;
        }

      nxsig_usleep(ESP_MIPI_DSI_POLL_US);
    }

  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_sample_tx_lp
 *
 * Description:
 *   Densely sample the D0 stop-state bit immediately after a command header
 *   is written, capturing the LP escape transmission window.  A lane that
 *   leaves LP11 proves the Host actually emitted the command; a window
 *   without any D0 activity means the packet never reached the lane.
 ****************************************************************************/

static void esp_mipi_dsi_sample_tx_lp(FAR struct esp_mipi_dsi_s *priv,
                                      uint8_t channel, uint8_t type,
                                      uint8_t hdr_msb, uint8_t hdr_lsb)
{
  FAR dsi_host_dev_t *host_dev = priv->hal.host;
  uint32_t active_samples = 0;
  uint32_t transitions = 0;
  uint32_t previous_stop;
  uint32_t first_status = 0;
  uint32_t last_status = 0;
  uint32_t status;
  uint32_t stop;
  uint32_t i;

  for (i = 0; i < ESP_MIPI_DSI_TX_SAMPLE_COUNT; i++)
    {
      status = host_dev->phy_status.val;
      stop = status & ESP_MIPI_DSI_PHY_DATA0_STOP_MASK;
      if (i == 0)
        {
          first_status = status;
        }
      else if (stop != previous_stop)
        {
          transitions++;
        }

      previous_stop = stop;
      last_status = status;
      if (stop == 0)
        {
          active_samples++;
        }

      up_udelay(ESP_MIPI_DSI_TX_SAMPLE_INTERVAL_US);
    }

  syslog(LOG_INFO,
         "INFO: MIPI-DSI TX LP sample vc=%u type=0x%02x hdr=%02x%02x "
         "d0_active=%" PRIu32 "/%d transitions=%" PRIu32
         " first=%08" PRIx32 " last=%08" PRIx32 "\n",
         channel, type, hdr_msb, hdr_lsb, active_samples,
         ESP_MIPI_DSI_TX_SAMPLE_COUNT, transitions, first_status,
         last_status);
}

static int esp_mipi_dsi_write_packet(
  FAR struct esp_mipi_dsi_s *priv,
  FAR const struct mipi_dsi_packet *packet,
  uint8_t channel, uint8_t type)
{
  FAR const uint8_t *payload = packet->payload;
  size_t remaining = packet->payload_length;
  uint32_t word;
  size_t bytes;
  int ret;

  if (mipi_dsi_packet_format_is_long(type))
    {
      while (remaining > 0)
        {
          bytes = remaining > sizeof(word) ? sizeof(word) : remaining;
          word = 0;
          memcpy(&word, payload, bytes);

          ret = esp_mipi_dsi_wait_while(
            priv, mipi_dsi_host_ll_gen_is_write_fifo_full,
            "write_payload_fifo");
          if (ret < 0)
            {
              return ret;
            }

          mipi_dsi_host_ll_gen_write_payload_fifo(priv->hal.host, word);
          payload += bytes;
          remaining -= bytes;
        }
    }

  ret = esp_mipi_dsi_wait_while(priv, mipi_dsi_host_ll_gen_is_cmd_fifo_full,
                                 "write_command_fifo");
  if (ret < 0)
    {
      return ret;
    }

  mipi_dsi_host_ll_gen_set_packet_header(priv->hal.host, channel,
    (mipi_dsi_data_type_t)type, packet->header[2], packet->header[1]);

  /* The header write arms the transmission.  Sample the D0 lane state
   * immediately, while the LP escape burst is expected on the wire.
   */

  esp_mipi_dsi_sample_tx_lp(priv, channel, type, packet->header[2],
                            packet->header[1]);
  return OK;
}

static int esp_mipi_dsi_read_packet(FAR struct esp_mipi_dsi_s *priv,
                                    FAR const struct mipi_dsi_packet *packet,
                                    FAR const struct mipi_dsi_msg *msg)
{
  FAR uint8_t *buffer = msg->rx_buf;
  uint32_t word;
  size_t copied = 0;
  uint8_t index;
  int ret;

  if (msg->rx_len > UINT16_MAX)
    {
      return -EMSGSIZE;
    }

  /* The maximum-return-size command must complete before the read request.
   * Command mode remains selected in M1; video mode is configured in M2.
   */

  ret = esp_mipi_dsi_wait_while(priv, mipi_dsi_host_ll_gen_is_cmd_fifo_full,
                                 "set_max_return_packet_size");
  if (ret < 0)
    {
      return ret;
    }

  mipi_dsi_host_ll_gen_set_packet_header(priv->hal.host, msg->channel,
    (mipi_dsi_data_type_t)ESP_MIPI_DSI_DT_SET_MAX_RETURN_PACKET_SIZE,
    (uint8_t)(msg->rx_len >> 8), (uint8_t)msg->rx_len);
  mipi_dsi_host_ll_enable_video_mode(priv->hal.host, false);
  mipi_dsi_host_ll_enable_bta(priv->hal.host, true);
  mipi_dsi_host_ll_gen_set_rx_vcid(priv->hal.host, msg->channel);

  ret = esp_mipi_dsi_write_packet(priv, packet, msg->channel, msg->type);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_mipi_dsi_wait_while(
    priv, mipi_dsi_host_ll_gen_is_read_cmd_busy, "read_command_complete");
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_mipi_dsi_wait_while(
    priv, mipi_dsi_host_ll_gen_is_read_fifo_empty, "read_response_fifo");
  if (ret < 0)
    {
      return ret;
    }

  while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(priv->hal.host))
    {
      word = mipi_dsi_host_ll_gen_read_payload_fifo(priv->hal.host);
      for (index = 0; index < sizeof(word) && copied < msg->rx_len; index++)
        {
          buffer[copied++] = word & 0xff;
          word >>= 8;
        }
    }

  return copied;
}

static void esp_mipi_dsi_enable_clocks(uint8_t bus, bool enable)
{
  PERIPH_RCC_ATOMIC()
    {
      mipi_dsi_ll_enable_bus_clock(bus, enable);

      if (enable)
        {
          mipi_dsi_ll_reset_register(bus);
        }
    }
}

static int esp_mipi_dsi_enable_phy_clock_sources(
  FAR struct esp_mipi_dsi_s *priv, uint8_t bus, FAR uint32_t *ref_hz)
{
  soc_module_clk_t cfg_source =
    (soc_module_clk_t)MIPI_DSI_PHY_CFG_CLK_SRC_DEFAULT;
  soc_module_clk_t pllref_source =
    (soc_module_clk_t)ESP_MIPI_DSI_PHY_PLLREF_CLK_SRC;
  int ret;

  ret = esp_mipi_dsi_clock_result(
    esp_clk_tree_enable_src(pllref_source, true));
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_mipi_dsi_clock_result(esp_clk_tree_enable_src(cfg_source, true));
  if (ret < 0)
    {
      esp_clk_tree_enable_src(pllref_source, false);
      return ret;
    }

  PERIPH_RCC_ATOMIC()
    {
      mipi_dsi_ll_set_phy_config_clock_source(
        bus, (soc_periph_mipi_dsi_phy_cfg_clk_src_t)cfg_source);
      mipi_dsi_ll_enable_phy_config_clock(bus, true);
      mipi_dsi_ll_set_phy_pllref_clock_source(
        bus, (mipi_dsi_phy_pllref_clock_source_t)pllref_source);
      mipi_dsi_ll_set_phy_pll_ref_clock_div(bus, 1);
      mipi_dsi_ll_enable_phy_pllref_clock(bus, true);
    }

  ret = esp_mipi_dsi_clock_result(esp_clk_tree_src_get_freq_hz(
    pllref_source, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, ref_hz));
  if (ret < 0)
    {
      PERIPH_RCC_ATOMIC()
        {
          mipi_dsi_ll_enable_phy_pllref_clock(bus, false);
          mipi_dsi_ll_enable_phy_config_clock(bus, false);
        }

      esp_clk_tree_enable_src(cfg_source, false);
      esp_clk_tree_enable_src(pllref_source, false);
      return ret;
    }

  priv->phy_cfg_clk_src = cfg_source;
  priv->phy_pllref_clk_src = pllref_source;
  syslog(LOG_INFO,
         "INFO: MIPI-DSI PHY clock sources cfg=%d pllref=%d ref_hz=%" PRIu32
         "\n", cfg_source, pllref_source, *ref_hz);
  return OK;
}

static void esp_mipi_dsi_disable_phy_clock_sources(
  FAR struct esp_mipi_dsi_s *priv, uint8_t bus)
{
  PERIPH_RCC_ATOMIC()
    {
      mipi_dsi_ll_enable_phy_pllref_clock(bus, false);
      mipi_dsi_ll_enable_phy_config_clock(bus, false);
    }

  if (priv->phy_cfg_clk_src != SOC_MOD_CLK_INVALID)
    {
      esp_clk_tree_enable_src(priv->phy_cfg_clk_src, false);
      priv->phy_cfg_clk_src = SOC_MOD_CLK_INVALID;
    }

  if (priv->phy_pllref_clk_src != SOC_MOD_CLK_INVALID)
    {
      esp_clk_tree_enable_src(priv->phy_pllref_clk_src, false);
      priv->phy_pllref_clk_src = SOC_MOD_CLK_INVALID;
    }
}

static void esp_mipi_dsi_configure_command_mode(
  FAR struct esp_mipi_dsi_s *priv, uint32_t lane_bit_rate_mbps)
{
  FAR dsi_host_dev_t *host = priv->hal.host;
  uint32_t lane_byte_clock_mhz = lane_bit_rate_mbps / 8;
  uint32_t escape_clock_div;

  escape_clock_div =
    (lane_byte_clock_mhz + ESP_MIPI_DSI_ESCAPE_CLOCK_MHZ - 1) /
    ESP_MIPI_DSI_ESCAPE_CLOCK_MHZ;
  if (escape_clock_div < 2)
    {
      escape_clock_div = 2;
    }

  mipi_dsi_host_ll_enable_video_mode(host, false);
  mipi_dsi_host_ll_set_clock_lane_state(
    host, MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);
  mipi_dsi_phy_ll_set_switch_time(host, 50, 104, 46, 128);
  mipi_dsi_host_ll_enable_rx_crc(host, true);
  mipi_dsi_host_ll_enable_rx_ecc(host, true);
  mipi_dsi_host_ll_enable_tx_eotp(host, true, false);
  mipi_dsi_host_ll_set_timeout_clock_division(
    host, (lane_byte_clock_mhz + ESP_MIPI_DSI_TIMEOUT_CLOCK_MHZ - 1) /
    ESP_MIPI_DSI_TIMEOUT_CLOCK_MHZ);
  mipi_dsi_host_ll_set_escape_clock_division(host, escape_clock_div);
  mipi_dsi_host_ll_set_timeout_count(host, 0, 0, 0, 0, 0, 0, 0);
  mipi_dsi_phy_ll_set_max_read_time(host, ESP_MIPI_DSI_MAX_READ_TIME);
  mipi_dsi_phy_ll_set_stop_wait_time(host, ESP_MIPI_DSI_STOP_WAIT_TIME);

  /* Match esp_lcd_new_panel_io_dbi() exactly.  Panel control packets use
   * LP mode, command acknowledgements are requested, and TE acknowledgement
   * remains disabled.  Without this block the reset value selects HS mode
   * for the EK79007 DCS initialization sequence.
   */

  mipi_dsi_host_ll_enable_te_ack(host, false);
  mipi_dsi_host_ll_enable_cmd_ack(host, true);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(
    host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(
    host, 1, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_wr_speed_mode(
    host, 2, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_long_wr_speed_mode(
    host, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(
    host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(
    host, 1, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_gen_short_rd_speed_mode(
    host, 2, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(
    host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_short_wr_speed_mode(
    host, 1, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_long_wr_speed_mode(
    host, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_dcs_short_rd_speed_mode(
    host, 0, MIPI_DSI_LL_TRANS_SPEED_LP);
  mipi_dsi_host_ll_set_mrps_speed_mode(
    host, MIPI_DSI_LL_TRANS_SPEED_LP);

  syslog(LOG_INFO,
         "INFO: MIPI-DSI DBI configured cmd_mode_cfg=%08" PRIx32
         " command_ack=1 transfer=LP\n",
         host->cmd_mode_cfg.val);
}

#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO

/****************************************************************************
 * Name: esp_mipi_dsi_video_pattern_type
 ****************************************************************************/

static int esp_mipi_dsi_video_pattern_type(
  enum esp_mipi_dsi_video_pattern_e pattern,
  FAR mipi_dsi_pattern_type_t *hal_pattern)
{
  switch (pattern)
    {
      case ESP_MIPI_DSI_VIDEO_PATTERN_NONE:
        *hal_pattern = MIPI_DSI_PATTERN_NONE;
        return OK;

      case ESP_MIPI_DSI_VIDEO_PATTERN_VERTICAL_BARS:
        *hal_pattern = MIPI_DSI_PATTERN_BAR_VERTICAL;
        return OK;

      case ESP_MIPI_DSI_VIDEO_PATTERN_HORIZONTAL_BARS:
        *hal_pattern = MIPI_DSI_PATTERN_BAR_HORIZONTAL;
        return OK;

      case ESP_MIPI_DSI_VIDEO_PATTERN_BER_VERTICAL:
        *hal_pattern = MIPI_DSI_PATTERN_BER_VERTICAL;
        return OK;

      default:
        return -EINVAL;
    }
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_timing_valid
 ****************************************************************************/

static bool esp_mipi_dsi_video_timing_valid(uint8_t channel,
                                            uint16_t hactive,
                                            uint16_t vactive,
                                            uint32_t pixel_clock_hz)
{
  return channel <= 3 && hactive != 0 && vactive != 0 &&
         pixel_clock_hz >= ESP_MIPI_DSI_MIN_DPI_CLOCK_HZ &&
         pixel_clock_hz <= ESP_MIPI_DSI_MAX_DPI_CLOCK_HZ;
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_color_format
 ****************************************************************************/

static int esp_mipi_dsi_video_color_format(
  enum esp_mipi_dsi_dpi_color_format_e format,
  FAR lcd_color_format_t *hal_format, FAR size_t *bytes_per_pixel)
{
  if (hal_format == NULL || bytes_per_pixel == NULL)
    {
      return -EINVAL;
    }

  switch (format)
    {
      case ESP_MIPI_DSI_DPI_COLOR_RGB565:
        *hal_format = LCD_COLOR_FMT_RGB565;
        *bytes_per_pixel = 2;
        return OK;

      case ESP_MIPI_DSI_DPI_COLOR_RGB888:
        *hal_format = LCD_COLOR_FMT_RGB888;
        *bytes_per_pixel = 3;
        return OK;

      default:
        return -EINVAL;
    }
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_configure_host
 ****************************************************************************/

static void esp_mipi_dsi_video_configure_host(
  FAR struct esp_mipi_dsi_s *priv, uint8_t channel, uint16_t hactive,
  uint16_t hsync, uint16_t hback_porch, uint16_t hfront_porch,
  uint16_t vactive, uint16_t vsync, uint16_t vback_porch,
  uint16_t vfront_porch, bool hsync_active_low, bool vsync_active_low,
  lcd_color_format_t output_format)
{
  mipi_dsi_host_ll_dpi_set_vcid(priv->hal.host, channel);
  mipi_dsi_host_ll_dpi_set_color_coding(priv->hal.host,
                                        output_format, 0);
  mipi_dsi_host_ll_dpi_enable_loosely18_packet(priv->hal.host, false);
  mipi_dsi_host_ll_dpi_set_timing_polarity(
    priv->hal.host, hsync_active_low, vsync_active_low, false, false,
    false);
  mipi_dsi_host_ll_dpi_enable_frame_ack(priv->hal.host, true);
  mipi_dsi_host_ll_dpi_enable_lp_horizontal_timing(priv->hal.host,
                                                    true, true);
  mipi_dsi_host_ll_dpi_enable_lp_vertical_timing(priv->hal.host,
                                                  true, true, true, true);
  mipi_dsi_host_ll_dpi_enable_lp_command(priv->hal.host, true);
  mipi_dsi_host_ll_dpi_set_video_burst_type(
    priv->hal.host, MIPI_DSI_LL_VIDEO_BURST_WITH_SYNC_PULSES);
  mipi_dsi_host_ll_dpi_set_null_packet_size(priv->hal.host, 0);
  mipi_dsi_host_ll_dpi_set_trunks_num(priv->hal.host, 0);
  mipi_dsi_host_ll_dpi_set_video_packet_pixel_num(priv->hal.host, hactive);

  mipi_dsi_hal_host_dpi_set_horizontal_timing(
    &priv->hal, hsync, hback_porch, hactive, hfront_porch);
  mipi_dsi_hal_host_dpi_set_vertical_timing(
    &priv->hal, vsync, vback_porch, vactive, vfront_porch);
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_configure_bridge
 ****************************************************************************/

static void esp_mipi_dsi_video_configure_bridge(
  FAR struct esp_mipi_dsi_s *priv, uint16_t hactive, uint16_t vactive,
  size_t bits_per_pixel, lcd_color_format_t input_format,
  lcd_color_format_t output_format,
  mipi_dsi_ll_flow_controller_t flow_controller)
{
  mipi_dsi_brg_ll_set_num_pixel_bits(
    priv->hal.bridge, (uint32_t)hactive * (uint32_t)vactive *
    bits_per_pixel);
  mipi_dsi_brg_ll_set_underrun_discard_count(priv->hal.bridge, hactive);
  mipi_dsi_brg_ll_set_input_color_format(priv->hal.bridge,
                                          input_format);
  mipi_dsi_brg_ll_set_output_color_format(priv->hal.bridge,
                                           output_format, 0);
  mipi_dsi_brg_ll_set_flow_controller(priv->hal.bridge, flow_controller);
}

#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
/****************************************************************************
 * Name: esp_mipi_dsi_bridge_underrun_worker
 *
 * Description:
 *   Report the first Bridge underrun outside interrupt context.  The ISR
 *   records only register snapshots and queues this worker so logging never
 *   delays DSI or GDMA interrupt delivery.
 ****************************************************************************/

static void esp_mipi_dsi_bridge_underrun_worker(FAR void *arg)
{
  FAR struct esp_mipi_dsi_s *priv = arg;
  irqstate_t flags;
  uint32_t count;
  uint32_t raw;
  uint32_t status;
  uint32_t frame;

  if (priv == NULL)
    {
      return;
    }

  flags = spin_lock_irqsave(&priv->bridge_irq_lock);
  if (!priv->bridge_underrun_report_pending)
    {
      spin_unlock_irqrestore(&priv->bridge_irq_lock, flags);
      return;
    }

  priv->bridge_underrun_report_pending = false;
  count = priv->bridge_underrun_count;
  raw = priv->bridge_first_underrun_raw;
  status = priv->bridge_first_underrun_status;
  frame = priv->bridge_first_underrun_frame;
  spin_unlock_irqrestore(&priv->bridge_irq_lock, flags);

  syslog(LOG_ERR,
         "ERROR: MIPI-DSI Bridge first underrun count=%" PRIu32
         " raw=%08" PRIx32 " status=%08" PRIx32
         " dma_frames=%" PRIu32 "\n",
         count, raw, status, frame);
}

/****************************************************************************
 * Name: esp_mipi_dsi_bridge_interrupt
 *
 * Description:
 *   Latch and clear Bridge underrun only.  A DSI Bridge interrupt can occur
 *   while GDMA is serving the next frame, so this handler must not take the
 *   video mutex, allocate memory, or print directly.
 ****************************************************************************/

static int esp_mipi_dsi_bridge_interrupt(int irq, FAR void *context,
                                         FAR void *arg)
{
  FAR struct esp_mipi_dsi_s *priv = arg;
  FAR dsi_brg_dev_t *bridge;
  irqstate_t flags;
  uint32_t status;
  uint32_t raw;
  bool queue_report = false;

  (void)irq;
  (void)context;

  if (priv == NULL || priv->hal.bridge == NULL)
    {
      return OK;
    }

  bridge = priv->hal.bridge;
  status = mipi_dsi_brg_ll_get_interrupt_status(bridge);
  if ((status & MIPI_DSI_BRG_LL_EVENT_UNDERRUN) == 0)
    {
      return OK;
    }

  raw = bridge->int_raw.val;
  mipi_dsi_brg_ll_clear_interrupt_status(
    bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN);

  flags = spin_lock_irqsave(&priv->bridge_irq_lock);
  priv->bridge_underrun_count++;
  if (!priv->bridge_first_underrun_seen)
    {
      priv->bridge_first_underrun_seen = true;
      priv->bridge_first_underrun_raw = raw;
      priv->bridge_first_underrun_status = status;
      priv->bridge_first_underrun_frame = priv->dma_frame_count;
      priv->bridge_underrun_report_pending = true;
      queue_report = true;
    }

  spin_unlock_irqrestore(&priv->bridge_irq_lock, flags);

  if (queue_report)
    {
      work_queue(LPWORK, &priv->bridge_underrun_work,
                 esp_mipi_dsi_bridge_underrun_worker, priv, 0);
    }

  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_bridge_interrupt_prepare
 *
 * Description:
 *   Attach and enable the CPU-side source before video begins, while the
 *   Bridge event itself remains masked.  The caller unmasks underrun only
 *   after DPI output is enabled, matching the ESP-IDF panel start sequence.
 ****************************************************************************/

static int esp_mipi_dsi_bridge_interrupt_prepare(
  FAR struct esp_mipi_dsi_s *priv)
{
  FAR dsi_brg_dev_t *bridge = priv->hal.bridge;
  irqstate_t flags;
  int cpuint;

  if (bridge == NULL)
    {
      return -ENODEV;
    }

  mipi_dsi_brg_ll_enable_interrupt(
    bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN, false);
  mipi_dsi_brg_ll_clear_interrupt_status(
    bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN);

  flags = spin_lock_irqsave(&priv->bridge_irq_lock);
  priv->bridge_underrun_count = 0;
  priv->bridge_first_underrun_raw = 0;
  priv->bridge_first_underrun_status = 0;
  priv->bridge_first_underrun_frame = 0;
  priv->bridge_first_underrun_seen = false;
  priv->bridge_underrun_report_pending = false;
  spin_unlock_irqrestore(&priv->bridge_irq_lock, flags);

  cpuint = esp_setup_irq(ETS_DSI_BRIDGE_INTR_SOURCE,
                         ESP_IRQ_PRIORITY_DEFAULT,
                         ESP_IRQ_TRIGGER_LEVEL,
                         esp_mipi_dsi_bridge_interrupt, priv);
  if (cpuint < 0)
    {
      return cpuint;
    }

  priv->bridge_cpuint = cpuint;
  up_enable_irq(ESP_SOURCE2IRQ(ETS_DSI_BRIDGE_INTR_SOURCE));
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_bridge_interrupt_release
 ****************************************************************************/

static void esp_mipi_dsi_bridge_interrupt_release(
  FAR struct esp_mipi_dsi_s *priv)
{
  FAR dsi_brg_dev_t *bridge = priv->hal.bridge;

  if (bridge != NULL)
    {
      mipi_dsi_brg_ll_enable_interrupt(
        bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN, false);
      mipi_dsi_brg_ll_clear_interrupt_status(
        bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN);
    }

  if (priv->bridge_cpuint >= 0)
    {
      up_disable_irq(ESP_SOURCE2IRQ(ETS_DSI_BRIDGE_INTR_SOURCE));
      esp_teardown_irq(ETS_DSI_BRIDGE_INTR_SOURCE, priv->bridge_cpuint);
      priv->bridge_cpuint = -1;
    }

  work_cancel_sync(LPWORK, &priv->bridge_underrun_work);
}

/****************************************************************************
 * Name: esp_mipi_dsi_dma_interrupt
 *
 * Description:
 *   DW-GDMA invalidates a completed list item.  Revalidate the single-frame
 *   descriptor and rebind it from the completion interrupt so a static
 *   frame buffer is scanned repeatedly.  No mutex or logging is used here.
 ****************************************************************************/

static int esp_mipi_dsi_dma_interrupt(int irq, FAR void *context,
                                      FAR void *arg)
{
  FAR struct esp_mipi_dsi_s *priv = arg;
  FAR dw_gdma_dev_t *dma_dev;
  esp_mipi_dsi_video_dma_frame_done_t frame_done;
  FAR const void *pending_frame_buffer;
  FAR void *frame_done_arg;
  irqstate_t flags;
  uint32_t status;

  (void)irq;
  (void)context;

  if (priv == NULL || !priv->video_running || priv->dma_dev == NULL)
    {
      return OK;
    }

  dma_dev = priv->dma_dev;
  status = dw_gdma_ll_channel_get_intr_status(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL);
  if (status == 0)
    {
      return OK;
    }

  dw_gdma_ll_channel_clear_intr(dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, status);

  if ((status & ESP_MIPI_DSI_DMA_ERROR_EVENTS) != 0)
    {
      priv->dma_error_events |= status & ESP_MIPI_DSI_DMA_ERROR_EVENTS;
      return OK;
    }

  if ((status & ESP_MIPI_DSI_DMA_DONE_EVENTS) != 0)
    {
      /* The vendor DPI path uses one terminal descriptor per framebuffer.
       * Hardware clears its valid state after the frame completes, therefore
       * every refresh must make it valid again before re-enabling the
       * channel.
       */

      flags = spin_lock_irqsave(&priv->dma_irq_lock);
      pending_frame_buffer = priv->dma_pending_frame_buffer;
      if (pending_frame_buffer != NULL)
        {
          priv->dma_active_frame_buffer = pending_frame_buffer;
          priv->dma_pending_frame_buffer = NULL;
        }

      frame_done = priv->dma_frame_done;
      frame_done_arg = priv->dma_frame_done_arg;
      spin_unlock_irqrestore(&priv->dma_irq_lock, flags);

      if (pending_frame_buffer != NULL)
        {
          dw_gdma_ll_lli_set_src_addr(
            &priv->dma_lli, (uint32_t)(uintptr_t)pending_frame_buffer);
          dw_gdma_ll_lli_set_src_master_port(
            &priv->dma_lli, (intptr_t)pending_frame_buffer);
        }

      dw_gdma_ll_lli_set_block_markers(&priv->dma_lli, false, true, true);
      esp_cache_msync(&priv->dma_lli, sizeof(priv->dma_lli),
                      ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                      ESP_CACHE_MSYNC_FLAG_UNALIGNED);
      dw_gdma_ll_channel_set_link_list_head_addr(
        dma_dev, ESP_MIPI_DSI_DMA_CHANNEL,
        (uint32_t)(uintptr_t)&priv->dma_lli);
      dw_gdma_ll_channel_enable(dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, true);
      priv->dma_frame_count++;

      if (frame_done != NULL)
        {
          frame_done(frame_done_arg);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_release
 ****************************************************************************/

static void esp_mipi_dsi_video_dma_release(
  FAR struct esp_mipi_dsi_s *priv)
{
  esp_mipi_dsi_bridge_interrupt_release(priv);

  if (priv->dma_cpuint >= 0)
    {
      up_disable_irq(ESP_SOURCE2IRQ(ETS_DW_GDMA_INTR_SOURCE));
      esp_teardown_irq(ETS_DW_GDMA_INTR_SOURCE, priv->dma_cpuint);
      priv->dma_cpuint = -1;
    }

  if (priv->dma_dev != NULL)
    {
      dw_gdma_ll_channel_enable(priv->dma_dev,
                                 ESP_MIPI_DSI_DMA_CHANNEL, false);
      dw_gdma_ll_channel_enable_intr_generation(
        priv->dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, UINT32_MAX, false);
      dw_gdma_ll_channel_enable_intr_propagation(
        priv->dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, UINT32_MAX, false);
      dw_gdma_ll_enable_intr_global(priv->dma_dev, false);
      dw_gdma_ll_enable_controller(priv->dma_dev, false);

      PERIPH_RCC_ATOMIC()
        {
          dw_gdma_ll_enable_bus_clock(ESP_MIPI_DSI_BUS0, false);
        }

      priv->dma_dev = NULL;
    }

  memset(&priv->dma_lli, 0, sizeof(priv->dma_lli));

  priv->dma_active_frame_buffer = NULL;
  priv->dma_pending_frame_buffer = NULL;
  priv->dma_frame_buffer_bytes = 0;
  priv->dma_frame_done = NULL;
  priv->dma_frame_done_arg = NULL;

  priv->dma_frame_count = 0;
  priv->dma_error_events = 0;
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_prepare
 ****************************************************************************/

static int esp_mipi_dsi_video_dma_prepare(
  FAR struct esp_mipi_dsi_s *priv, FAR const void *frame_buffer,
  size_t frame_buffer_bytes)
{
  FAR dw_gdma_dev_t *dma_dev = NULL;
  FAR dw_gdma_link_list_item_t *lli;
  int ret;

  if (((uintptr_t)frame_buffer &
       (ESP_MIPI_DSI_DMA_TRANSFER_WIDTH_BYTES - 1)) != 0 ||
      (frame_buffer_bytes % ESP_MIPI_DSI_DMA_TRANSFER_WIDTH_BYTES) != 0)
    {
      return -EINVAL;
    }

  if (priv->dma_dev != NULL)
    {
      return -EALREADY;
    }

  /* ESP-IDF's dw_gdma.c is a FreeRTOS upper-half driver.  The probe needs
   * only one DSI-owned circular LLI, so configure the already-built HAL/LL
   * directly and keep this NuttX adapter independent of FreeRTOS.
   */

  PERIPH_RCC_ATOMIC()
    {
      dw_gdma_ll_enable_bus_clock(ESP_MIPI_DSI_BUS0, true);
      dw_gdma_ll_reset_register(ESP_MIPI_DSI_BUS0);
    }

  dma_dev = DW_GDMA_LL_GET_HW(ESP_MIPI_DSI_BUS0);
  if (dma_dev == NULL)
    {
      ret = -ENODEV;
      goto errout;
    }

  priv->dma_dev = dma_dev;
  priv->dma_frame_count = 0;
  priv->dma_error_events = 0;
  priv->dma_active_frame_buffer = frame_buffer;
  priv->dma_pending_frame_buffer = NULL;
  priv->dma_frame_buffer_bytes = frame_buffer_bytes;
  priv->dma_frame_done = NULL;
  priv->dma_frame_done_arg = NULL;
  lli = &priv->dma_lli;
  memset(lli, 0, sizeof(*lli));

  dw_gdma_ll_reset(dma_dev);
  dw_gdma_ll_enable_controller(dma_dev, true);
  dw_gdma_ll_enable_intr_global(dma_dev, false);
  dw_gdma_ll_channel_set_trans_flow(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, DW_GDMA_ROLE_MEM,
    DW_GDMA_ROLE_PERIPH_DSI, DW_GDMA_FLOW_CTRL_SELF);
  dw_gdma_ll_channel_set_src_multi_block_type(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, DW_GDMA_BLOCK_TRANSFER_LIST);
  dw_gdma_ll_channel_set_dst_multi_block_type(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, DW_GDMA_BLOCK_TRANSFER_LIST);
  dw_gdma_ll_channel_set_src_handshake_interface(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, DW_GDMA_HANDSHAKE_HW);
  dw_gdma_ll_channel_set_dst_handshake_interface(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, DW_GDMA_HANDSHAKE_HW);
  dw_gdma_ll_channel_set_dst_handshake_periph(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, DW_GDMA_ROLE_PERIPH_DSI);
  dw_gdma_ll_channel_set_priority(dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, 1);
  dw_gdma_ll_channel_set_src_outstanding_limit(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, 5);
  dw_gdma_ll_channel_set_dst_outstanding_limit(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, 2);
  dw_gdma_ll_channel_set_dst_periph_status_addr(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, MIPI_DSI_BRG_MEM_BASE);

  /* Latch and propagate only terminal and fault events.  A completed frame
   * is revalidated in esp_mipi_dsi_dma_interrupt(), which keeps scanout
   * continuous without the invalid self-linked descriptor used previously.
   */

  dw_gdma_ll_channel_clear_intr(dma_dev, ESP_MIPI_DSI_DMA_CHANNEL,
                                UINT32_MAX);
  dw_gdma_ll_channel_enable_intr_generation(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, UINT32_MAX, false);
  dw_gdma_ll_channel_enable_intr_propagation(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, UINT32_MAX, false);
  dw_gdma_ll_channel_enable_intr_generation(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, ESP_MIPI_DSI_DMA_EVENTS, true);
  dw_gdma_ll_channel_enable_intr_propagation(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, ESP_MIPI_DSI_DMA_EVENTS, true);
  dw_gdma_ll_channel_set_link_list_master_port(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, DW_GDMA_LL_MASTER_PORT_MEMORY);
  dw_gdma_ll_channel_set_link_list_head_addr(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, (uint32_t)(uintptr_t)lli);

  dw_gdma_ll_lli_set_src_addr(lli, (uint32_t)(uintptr_t)frame_buffer);
  dw_gdma_ll_lli_set_dst_addr(lli, MIPI_DSI_BRG_MEM_BASE);
  dw_gdma_ll_lli_set_trans_block_size(
    lli, frame_buffer_bytes / ESP_MIPI_DSI_DMA_TRANSFER_WIDTH_BYTES);
  dw_gdma_ll_lli_set_src_master_port(lli, (intptr_t)frame_buffer);
  dw_gdma_ll_lli_set_dst_master_port(lli, MIPI_DSI_BRG_MEM_BASE);
  dw_gdma_ll_lli_set_src_trans_width(lli, DW_GDMA_TRANS_WIDTH_64);
  dw_gdma_ll_lli_set_dst_trans_width(lli, DW_GDMA_TRANS_WIDTH_64);
  dw_gdma_ll_lli_set_src_burst_mode(lli, DW_GDMA_BURST_MODE_INCREMENT);
  dw_gdma_ll_lli_set_dst_burst_mode(lli, DW_GDMA_BURST_MODE_FIXED);
  dw_gdma_ll_lli_set_src_burst_items(lli, DW_GDMA_BURST_ITEMS_512);
  dw_gdma_ll_lli_set_dst_burst_items(lli, DW_GDMA_BURST_ITEMS_256);
  dw_gdma_ll_lli_set_src_burst_len(lli, 16);
  dw_gdma_ll_lli_set_dst_burst_len(lli, 16);
  dw_gdma_ll_lli_set_block_markers(lli, false, true, true);
  dw_gdma_ll_lli_set_link_list_master_port(
    lli, DW_GDMA_LL_MASTER_PORT_MEMORY);
  dw_gdma_ll_lli_set_next_item_addr(lli, 0);

  ret = esp_mipi_dsi_clock_result(esp_cache_msync(
    lli, sizeof(*lli), ESP_CACHE_MSYNC_FLAG_DIR_C2M |
    ESP_CACHE_MSYNC_FLAG_UNALIGNED));
  if (ret < 0)
    {
      goto errout;
    }

  priv->dma_cpuint = esp_setup_irq(ETS_DW_GDMA_INTR_SOURCE,
                                    ESP_IRQ_PRIORITY_DEFAULT,
                                    ESP_IRQ_TRIGGER_LEVEL,
                                    esp_mipi_dsi_dma_interrupt, priv);
  if (priv->dma_cpuint < 0)
    {
      ret = priv->dma_cpuint;
      priv->dma_cpuint = -1;
      goto errout;
    }

  dw_gdma_ll_enable_intr_global(dma_dev, true);
  up_enable_irq(ESP_SOURCE2IRQ(ETS_DW_GDMA_INTR_SOURCE));
  return OK;

errout:
  if (priv->dma_dev != NULL)
    {
      esp_mipi_dsi_video_dma_release(priv);
    }
  else
    {
      PERIPH_RCC_ATOMIC()
        {
          dw_gdma_ll_enable_bus_clock(ESP_MIPI_DSI_BUS0, false);
        }
    }

  return ret;
}
#endif

/****************************************************************************
 * Name: esp_mipi_dsi_enable_dpi_clock
 ****************************************************************************/

static int esp_mipi_dsi_enable_dpi_clock(
  FAR struct esp_mipi_dsi_s *priv, uint32_t pixel_clock_hz)
{
  uint32_t source_hz;
  uint32_t divider;
  int ret;

  ret = esp_mipi_dsi_clock_result(
    esp_clk_tree_enable_src(ESP_MIPI_DSI_DPI_CLK_SRC, true));
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_mipi_dsi_clock_result(esp_clk_tree_src_get_freq_hz(
    ESP_MIPI_DSI_DPI_CLK_SRC, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
    &source_hz));
  if (ret < 0)
    {
      esp_clk_tree_enable_src(ESP_MIPI_DSI_DPI_CLK_SRC, false);
      return ret;
    }

  divider = mipi_dsi_hal_host_dpi_calculate_divider(
    &priv->hal, (float)source_hz / 1000000.0f,
    (float)pixel_clock_hz / 1000000.0f);
  if (divider == 0 || divider > MIPI_DSI_LL_MAX_DPI_CLK_DIV)
    {
      esp_clk_tree_enable_src(ESP_MIPI_DSI_DPI_CLK_SRC, false);
      return -ERANGE;
    }

  PERIPH_RCC_ATOMIC()
    {
      mipi_dsi_ll_set_dpi_clock_source(
        ESP_MIPI_DSI_BUS0,
        (mipi_dsi_dpi_clock_source_t)ESP_MIPI_DSI_DPI_CLK_SRC);
      mipi_dsi_ll_set_dpi_clock_div(ESP_MIPI_DSI_BUS0, divider);
      mipi_dsi_ll_enable_dpi_clock(ESP_MIPI_DSI_BUS0, true);
    }

  priv->dpi_clk_src = ESP_MIPI_DSI_DPI_CLK_SRC;
  syslog(LOG_INFO,
         "INFO: MIPI-DSI DPI clock source_hz=%" PRIu32
         " requested_hz=%" PRIu32 " divider=%" PRIu32
         " actual_hz=%" PRIu32 "\n",
         source_hz, pixel_clock_hz, divider,
         (uint32_t)(priv->hal.real_dpi_clock_freq_mhz * 1000000.0f));
  return OK;
}

/****************************************************************************
 * Name: esp_mipi_dsi_disable_dpi_clock
 ****************************************************************************/

static void esp_mipi_dsi_disable_dpi_clock(
  FAR struct esp_mipi_dsi_s *priv)
{
  PERIPH_RCC_ATOMIC()
    {
      mipi_dsi_ll_enable_dpi_clock(ESP_MIPI_DSI_BUS0, false);
    }

  if (priv->dpi_clk_src != SOC_MOD_CLK_INVALID)
    {
      esp_clk_tree_enable_src(priv->dpi_clk_src, false);
      priv->dpi_clk_src = SOC_MOD_CLK_INVALID;
    }
}

/****************************************************************************
 * Name: esp_mipi_dsi_dump_video_state
 *
 * Description:
 *   The Host accepting a video-pattern configuration only proves that the
 *   CPU-side register programming completed.  Keep a compact snapshot of
 *   the Host and bridge state so a missing image can be distinguished from
 *   an incomplete video configuration before a framebuffer/DMA path exists.
 ****************************************************************************/

static void esp_mipi_dsi_dump_video_state(
  FAR struct esp_mipi_dsi_s *priv, FAR const char *stage)
{
  FAR dsi_host_dev_t *host = priv->hal.host;
  FAR dsi_brg_dev_t *bridge = priv->hal.bridge;

  syslog(LOG_INFO,
         "INFO: MIPI-DSI video state stage=%s host_mode=%08" PRIx32
         " vid_mode=%08" PRIx32 " color=%08" PRIx32
         " pkt_size=%08" PRIx32 " phy=%08" PRIx32 "\n",
         stage, host->mode_cfg.val, host->vid_mode_cfg.val,
         host->dpi_color_coding.val, host->vid_pkt_size.val,
         host->phy_status.val);
  syslog(LOG_INFO,
         "INFO: MIPI-DSI bridge state stage=%s en=%08" PRIx32
         " host_ctrl=%08" PRIx32 " h0=%08" PRIx32 " h1=%08" PRIx32
         " v0=%08" PRIx32 " v1=%08" PRIx32 " misc=%08" PRIx32 "\n",
         stage, bridge->en.val, bridge->host_ctrl.val,
         bridge->dpi_h_cfg0.val, bridge->dpi_h_cfg1.val,
         bridge->dpi_v_cfg0.val, bridge->dpi_v_cfg1.val,
         bridge->dpi_misc_config.val);
  syslog(LOG_INFO,
         "INFO: MIPI-DSI bridge flow stage=%s pixel=%08" PRIx32
         " flow=%08" PRIx32 " raw=%08" PRIx32 " int_raw=%08" PRIx32
         " int_st=%08" PRIx32 "\n",
         stage, bridge->pixel_type.val, bridge->dma_flow_ctrl.val,
         bridge->raw_num_cfg.val, bridge->int_raw.val, bridge->int_st.val);
  syslog(LOG_INFO,
         "INFO: MIPI-DSI format decode stage=%s bridge_raw=%" PRIu32
         " bridge_dpi=%" PRIu32 " data_in=%" PRIu32
         " host_color=%" PRIu32 "\n",
         stage, (uint32_t)bridge->pixel_type.raw_type,
         (uint32_t)bridge->pixel_type.dpi_type,
         (uint32_t)bridge->pixel_type.data_in_type,
         (uint32_t)host->dpi_color_coding.dpi_color_coding);
  syslog(LOG_INFO,
         "INFO: MIPI-DSI PHY snapshot stage=%s lock=%" PRIu32
         " clk_stop=%" PRIu32 " d0_stop=%" PRIu32
         " d1_stop=%" PRIu32 "\n",
         stage, (uint32_t)host->phy_status.phy_lock,
         (uint32_t)host->phy_status.phy_stopstateclklane,
         (uint32_t)host->phy_status.phy_stopstate0lane,
         (uint32_t)host->phy_status.phy_stopstate1lane);
}

#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
/****************************************************************************
 * Name: esp_mipi_dsi_dump_dma_status
 *
 * Description:
 *   Capture the state which distinguishes a correctly programmed scanout
 *   from one that is actually consuming pixels.  This function is read-only:
 *   it deliberately does not clear DMA or Bridge event bits.
 ****************************************************************************/

static void esp_mipi_dsi_dump_dma_status(
  FAR struct esp_mipi_dsi_s *priv, FAR const char *stage)
{
  FAR dw_gdma_dev_t *dma_dev = priv->dma_dev;
  FAR dsi_brg_dev_t *bridge = priv->hal.bridge;
  uint32_t transfer_items;
  uint32_t fifo_remaining;
  uint32_t channel_status;
  uint32_t bridge_raw;
  uint32_t bridge_status;
  uint32_t bridge_underrun_count;
  bool bridge_first_underrun_seen;
  uint64_t transfer_bytes;
  intptr_t current_lli;
  irqstate_t flags;
  bool channel_enabled;

  if (dma_dev == NULL || bridge == NULL)
    {
      syslog(LOG_WARNING,
             "WARNING: MIPI-DSI DMA status stage=%s unavailable\n", stage);
      return;
    }

  channel_enabled = (dma_dev->chen0.val &
                     (1u << ESP_MIPI_DSI_DMA_CHANNEL)) != 0;
  current_lli = dw_gdma_ll_channel_get_current_link_list_item_addr(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL);
  transfer_items = dw_gdma_ll_channel_get_trans_amount(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL);
  fifo_remaining = dw_gdma_ll_channel_get_fifo_remain(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL);
  channel_status = dw_gdma_ll_channel_get_intr_status(
    dma_dev, ESP_MIPI_DSI_DMA_CHANNEL);
  bridge_raw = bridge->int_raw.val;
  bridge_status = mipi_dsi_brg_ll_get_interrupt_status(bridge);
  flags = spin_lock_irqsave(&priv->bridge_irq_lock);
  bridge_underrun_count = priv->bridge_underrun_count;
  bridge_first_underrun_seen = priv->bridge_first_underrun_seen;
  spin_unlock_irqrestore(&priv->bridge_irq_lock, flags);
  transfer_bytes = (uint64_t)transfer_items *
                   ESP_MIPI_DSI_DMA_TRANSFER_WIDTH_BYTES;

  syslog(LOG_INFO,
         "INFO: MIPI-DSI DMA status stage=%s enabled=%d controller=%08"
         PRIx32 " lli=%08" PRIxPTR " transferred_items=%" PRIu32
         " transferred_bytes=%" PRIu64 " fifo_left=%" PRIu32
         " frames=%" PRIu32 " latched_errors=%08" PRIx32 "\n",
         stage, channel_enabled, dma_dev->cfg0.val,
         (uintptr_t)current_lli, transfer_items, transfer_bytes,
         fifo_remaining, priv->dma_frame_count, priv->dma_error_events);
  syslog(LOG_INFO,
         "INFO: MIPI-DSI DMA events stage=%s enable=%08" PRIx32
         " status=%08" PRIx32 " done=%08" PRIx32 " errors=%08" PRIx32
         " channel_cfg0=%08" PRIx32 " channel_cfg1=%08" PRIx32 "\n",
         stage, dma_dev->ch[ESP_MIPI_DSI_DMA_CHANNEL].int_st_ena0.val,
         channel_status, channel_status & ESP_MIPI_DSI_DMA_DONE_EVENTS,
         channel_status & ESP_MIPI_DSI_DMA_ERROR_EVENTS,
         dma_dev->ch[ESP_MIPI_DSI_DMA_CHANNEL].cfg0.val,
         dma_dev->ch[ESP_MIPI_DSI_DMA_CHANNEL].cfg1.val);
  syslog(LOG_INFO,
         "INFO: MIPI-DSI Bridge events stage=%s raw=%08" PRIx32
         " enable=%08" PRIx32 " status=%08" PRIx32
         " underrun_raw=%d underrun_status=%d underrun_count=%" PRIu32
         " first_underrun=%d\n",
         stage, bridge_raw, bridge->int_ena.val, bridge_status,
         (bridge_raw & MIPI_DSI_BRG_LL_EVENT_UNDERRUN) != 0,
         (bridge_status & MIPI_DSI_BRG_LL_EVENT_UNDERRUN) != 0,
         bridge_underrun_count, bridge_first_underrun_seen);
}
#endif

/****************************************************************************
 * Name: esp_mipi_dsi_video_stop_locked
 ****************************************************************************/

static void esp_mipi_dsi_video_stop_locked(FAR struct esp_mipi_dsi_s *priv)
{
  if (!priv->video_running)
    {
      return;
    }

  mipi_dsi_host_ll_enable_video_mode(priv->hal.host, false);
  mipi_dsi_host_ll_dpi_set_pattern_type(priv->hal.host,
                                        MIPI_DSI_PATTERN_NONE);
  mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
  esp_mipi_dsi_video_dma_release(priv);
#endif
  mipi_dsi_brg_ll_enable(priv->hal.bridge, false);
  mipi_dsi_brg_ll_enable_ref_clock(priv->hal.bridge, false);
  mipi_dsi_brg_ll_force_enable_reg_clock(priv->hal.bridge, false);
  esp_mipi_dsi_disable_dpi_clock(priv);
  priv->video_running = false;
  syslog(LOG_INFO, "INFO: MIPI-DSI DPI video stopped\n");
}

#endif /* CONFIG_ESPRESSIF_MIPI_DSI_VIDEO */

static bool esp_mipi_dsi_message_is_read(uint8_t type)
{
  /* A DSI data type, rather than incidental receive-buffer fields, defines
   * whether the Host must start BTA and wait for an RX payload.  In
   * particular, DCS write helpers only require transmit fields to be set.
   */

  switch (type)
    {
      case MIPI_DSI_GENERIC_READ_0_PARAM:
      case MIPI_DSI_GENERIC_READ_1_PARAM:
      case MIPI_DSI_GENERIC_READ_2_PARAM:
      case MIPI_DSI_DCS_READ_0_PARAM:
        return true;

      default:
        return false;
    }
}

static int esp_mipi_dsi_attach(FAR struct mipi_dsi_host *host,
                               FAR struct mipi_dsi_device *device)
{
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  int ret;

  if (host != &priv->host || device == NULL || device->channel > 3 ||
      device->lanes == 0 || device->lanes > ESP_MIPI_DSI_MAX_DATA_LANES)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = priv->ready ? OK : -ESHUTDOWN;
  nxmutex_unlock(&priv->lock);
  return ret;
}

static int esp_mipi_dsi_detach(FAR struct mipi_dsi_host *host,
                               FAR struct mipi_dsi_device *device)
{
  if (host != &g_esp_mipi_dsi.host || device == NULL)
    {
      return -EINVAL;
    }

  return OK;
}

static ssize_t esp_mipi_dsi_transfer(FAR struct mipi_dsi_host *host,
                                     FAR const struct mipi_dsi_msg *msg)
{
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  struct mipi_dsi_packet packet;
  bool is_read;
  ssize_t ret;

  if (host != &priv->host || msg == NULL || msg->channel > 3 ||
      (msg->tx_len > 0 && msg->tx_buf == NULL))
    {
      return -EINVAL;
    }

  is_read = esp_mipi_dsi_message_is_read(msg->type);
  if (is_read && (msg->rx_len == 0 || msg->rx_buf == NULL))
    {
      return -EINVAL;
    }

  ret = mipi_dsi_create_packet(&packet, msg);
  if (ret < 0)
    {
      return ret;
    }

  if (!mipi_dsi_packet_format_is_short(msg->type) &&
      !mipi_dsi_packet_format_is_long(msg->type))
    {
      return -ENOTSUP;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->ready)
    {
      ret = -ESHUTDOWN;
    }
  else if (is_read)
    {
      ret = esp_mipi_dsi_read_packet(priv, &packet, msg);
    }
  else
    {
      ret = esp_mipi_dsi_write_packet(priv, &packet, msg->channel,
                                      msg->type);
      if (ret == OK)
        {
          ret = msg->tx_len;
        }
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_mipi_dsi_host_initialize(
  FAR const struct esp_mipi_dsi_host_config_s *config,
  FAR struct mipi_dsi_host **host)
{
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  mipi_dsi_hal_config_t hal_config;
  uint32_t phy_ref_clock_hz;
  uint32_t timeout_ms;
  int ret;

  if (config == NULL || host == NULL || config->bus != ESP_MIPI_DSI_BUS0 ||
      config->lane_num == 0 ||
      config->lane_num > ESP_MIPI_DSI_MAX_DATA_LANES ||
      config->lane_bit_rate_mbps < ESP_MIPI_DSI_MIN_RATE_MBPS ||
      config->lane_bit_rate_mbps > ESP_MIPI_DSI_MAX_RATE_MBPS ||
      config->phy_ref_clock_hz < ESP_MIPI_DSI_MIN_PHY_REF_HZ ||
      config->phy_ref_clock_hz > ESP_MIPI_DSI_MAX_PHY_REF_HZ ||
      config->phy_ldo.voltage_mv != ESP_MIPI_DSI_DPHY_VOLTAGE_MV)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->ready)
    {
      *host = &priv->host;
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  ret = esp_ldo_acquire(&config->phy_ldo, &priv->phy_ldo);
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "ERROR: MIPI-DSI D-PHY LDO acquire failed channel=%u"
             " voltage_mv=%d ret=%d\n",
             config->phy_ldo.channel_id, config->phy_ldo.voltage_mv, ret);
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  syslog(LOG_INFO,
         "INFO: MIPI-DSI D-PHY LDO ready channel=%u voltage_mv=%d"
         " lanes=%u rate_mbps=%" PRIu32 " ref_hz=%" PRIu32 "\n",
         config->phy_ldo.channel_id, config->phy_ldo.voltage_mv,
         config->lane_num, config->lane_bit_rate_mbps,
         config->phy_ref_clock_hz);

  timeout_ms = config->timeout_ms == 0 ? ESP_MIPI_DSI_DEFAULT_TIMEOUT_MS :
                                         config->timeout_ms;
  priv->timeout_ticks = MSEC2TICK(timeout_ms);
  if (priv->timeout_ticks == 0)
    {
      priv->timeout_ticks = 1;
    }

  esp_mipi_dsi_enable_clocks(config->bus, true);
  syslog(LOG_INFO, "INFO: MIPI-DSI clocks enabled bus=%u\n", config->bus);

  ret = esp_mipi_dsi_enable_phy_clock_sources(priv, config->bus,
                                               &phy_ref_clock_hz);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: MIPI-DSI PHY clock setup ret=%d\n", ret);
      esp_mipi_dsi_enable_clocks(config->bus, false);
      esp_ldo_release(priv->phy_ldo);
      priv->phy_ldo = NULL;
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  if (phy_ref_clock_hz != config->phy_ref_clock_hz)
    {
      syslog(LOG_WARNING,
             "WARNING: MIPI-DSI configured ref_hz=%" PRIu32
             " differs from active ref_hz=%" PRIu32 "\n",
             config->phy_ref_clock_hz, phy_ref_clock_hz);
    }

  memset(&hal_config, 0, sizeof(hal_config));
  hal_config.bus_id = config->bus;
  hal_config.lane_bit_rate_mbps = config->lane_bit_rate_mbps;
  hal_config.num_data_lanes = config->lane_num;
  mipi_dsi_hal_init(&priv->hal, &hal_config);
  mipi_dsi_hal_configure_phy_pll(&priv->hal, phy_ref_clock_hz,
                                 config->lane_bit_rate_mbps);

  syslog(LOG_INFO,
         "INFO: MIPI-DSI PHY PLL configured timeout_ms=%" PRIu32 "\n",
         timeout_ms);

  ret = esp_mipi_dsi_wait_pll(priv);
  if (ret < 0)
    {
      mipi_dsi_hal_deinit(&priv->hal);
      esp_mipi_dsi_disable_phy_clock_sources(priv, config->bus);
      esp_mipi_dsi_enable_clocks(config->bus, false);
      esp_ldo_release(priv->phy_ldo);
      priv->phy_ldo = NULL;
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  ret = esp_mipi_dsi_wait_lanes_stopped(priv, config->lane_num);
  if (ret < 0)
    {
      mipi_dsi_hal_deinit(&priv->hal);
      esp_mipi_dsi_disable_phy_clock_sources(priv, config->bus);
      esp_mipi_dsi_enable_clocks(config->bus, false);
      esp_ldo_release(priv->phy_ldo);
      priv->phy_ldo = NULL;
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  esp_mipi_dsi_configure_command_mode(priv, config->lane_bit_rate_mbps);
  priv->ready = true;

  syslog(LOG_INFO, "INFO: MIPI-DSI Host ready bus=%u\n", config->bus);

  if (!priv->registered)
    {
      ret = mipi_dsi_host_register(&priv->host);
      if (ret < 0)
        {
          priv->ready = false;
          mipi_dsi_hal_deinit(&priv->hal);
          esp_mipi_dsi_disable_phy_clock_sources(priv, config->bus);
          esp_mipi_dsi_enable_clocks(config->bus, false);
          esp_ldo_release(priv->phy_ldo);
          priv->phy_ldo = NULL;
          nxmutex_unlock(&priv->lock);
          return ret;
        }

      priv->registered = true;
    }

  *host = &priv->host;
  nxmutex_unlock(&priv->lock);
  return OK;
}

int esp_mipi_dsi_host_shutdown(FAR struct mipi_dsi_host *host)
{
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  int ret;

  if (host != &priv->host)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->ready)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO
  esp_mipi_dsi_video_stop_locked(priv);
#endif
  mipi_dsi_hal_deinit(&priv->hal);
  esp_mipi_dsi_disable_phy_clock_sources(priv, ESP_MIPI_DSI_BUS0);
  esp_mipi_dsi_enable_clocks(ESP_MIPI_DSI_BUS0, false);
  priv->ready = false;
  ret = esp_ldo_release(priv->phy_ldo);
  priv->phy_ldo = NULL;
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_pattern_start
 ****************************************************************************/

int esp_mipi_dsi_video_pattern_start(
  FAR struct mipi_dsi_host *host,
  FAR const struct esp_mipi_dsi_video_pattern_config_s *config)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  mipi_dsi_pattern_type_t pattern;
  int ret;

  if (host != &priv->host || config == NULL ||
      config->pattern == ESP_MIPI_DSI_VIDEO_PATTERN_NONE ||
      !esp_mipi_dsi_video_timing_valid(config->channel, config->hactive,
                                        config->vactive,
                                        config->pixel_clock_hz))
    {
      return -EINVAL;
    }

  ret = esp_mipi_dsi_video_pattern_type(config->pattern, &pattern);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->ready)
    {
      ret = -ESHUTDOWN;
    }
  else if (priv->video_running)
    {
      ret = -EALREADY;
    }
  else
    {
      ret = esp_mipi_dsi_enable_dpi_clock(priv, config->pixel_clock_hz);
      if (ret == OK)
        {
          /* The bridge owns the DPI timing registers.  Its register clock
           * must be running before those registers are programmed.
           */

          mipi_dsi_brg_ll_force_enable_reg_clock(priv->hal.bridge, true);
          mipi_dsi_brg_ll_enable_ref_clock(priv->hal.bridge, true);

          esp_mipi_dsi_video_configure_host(
            priv, config->channel, config->hactive, config->hsync,
            config->hback_porch, config->hfront_porch, config->vactive,
            config->vsync, config->vback_porch, config->vfront_porch,
            config->hsync_active_low, config->vsync_active_low,
            LCD_COLOR_FMT_RGB888);
          mipi_dsi_host_ll_dpi_set_pattern_type(priv->hal.host, pattern);

          /* Keep the same bridge-side format and timing contract as the
           * ESP-IDF DPI panel path.  M2a intentionally selects the bridge
           * flow controller because its Host video pattern generator has no
           * framebuffer or GDMA producer.
           */

          esp_mipi_dsi_video_configure_bridge(
            priv, config->hactive, config->vactive,
            24, LCD_COLOR_FMT_RGB888, LCD_COLOR_FMT_RGB888,
            MIPI_DSI_LL_FLOW_CONTROLLER_BRIDGE);
          mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
          mipi_dsi_brg_ll_enable(priv->hal.bridge, true);
          mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
          mipi_dsi_host_ll_enable_bta(priv->hal.host, false);
          mipi_dsi_host_ll_set_clock_lane_state(
            priv->hal.host, MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);
          mipi_dsi_host_ll_enable_video_mode(priv->hal.host, true);
          mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, true);
          mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
          priv->video_running = true;
          esp_mipi_dsi_dump_video_state(priv, "started");
          syslog(LOG_INFO,
                 "INFO: MIPI-DSI DPI pattern Host started channel=%u "
                 "size=%ux%u pixel_clock_hz=%" PRIu32 " pattern=%d; "
                 "visual verification pending\n",
                 config->channel, config->hactive, config->vactive,
                 config->pixel_clock_hz, config->pattern);
        }
    }

  nxmutex_unlock(&priv->lock);
  return ret;
#else
  (void)host;
  (void)config;
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_pattern_set
 ****************************************************************************/

int esp_mipi_dsi_video_pattern_set(
  FAR struct mipi_dsi_host *host,
  enum esp_mipi_dsi_video_pattern_e pattern)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  mipi_dsi_pattern_type_t hal_pattern;
  int ret;

  if (host != &priv->host)
    {
      return -EINVAL;
    }

  ret = esp_mipi_dsi_video_pattern_type(pattern, &hal_pattern);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->ready)
    {
      ret = -ESHUTDOWN;
    }
  else if (!priv->video_running)
    {
      ret = -EPIPE;
    }
  else
    {
      /* Match esp_lcd_dpi_panel_set_pattern(): the Host pattern generator
       * and the Bridge DPI producer must not drive the Host input at the
       * same time.  Disable Bridge output before selecting a test pattern;
       * restore it only when the pattern is turned off.
       */

      if (hal_pattern != MIPI_DSI_PATTERN_NONE)
        {
          mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
          mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
        }

      mipi_dsi_host_ll_dpi_set_pattern_type(priv->hal.host, hal_pattern);

      if (hal_pattern == MIPI_DSI_PATTERN_NONE)
        {
          mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, true);
          mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
        }

      esp_mipi_dsi_dump_video_state(priv, "pattern-selected");
      syslog(LOG_INFO, "INFO: MIPI-DSI Host pattern selected pattern=%d\n",
             pattern);
      ret = OK;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
#else
  (void)host;
  (void)pattern;
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_start
 ****************************************************************************/

int esp_mipi_dsi_video_dma_start(
  FAR struct mipi_dsi_host *host,
  FAR const struct esp_mipi_dsi_video_dma_config_s *config)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  lcd_color_format_t input_format;
  lcd_color_format_t output_format;
  size_t bytes_per_pixel;
  size_t output_bytes_per_pixel;
  size_t required_bytes;
  int ret;

  if (host != &priv->host || config == NULL ||
      !esp_mipi_dsi_video_timing_valid(config->channel, config->hactive,
                                        config->vactive,
                                        config->pixel_clock_hz) ||
      config->frame_buffer == NULL)
    {
      return -EINVAL;
    }

  ret = esp_mipi_dsi_video_color_format(config->input_format,
                                        &input_format, &bytes_per_pixel);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_mipi_dsi_video_color_format(config->output_format,
                                        &output_format,
                                        &output_bytes_per_pixel);
  if (ret < 0)
    {
      return ret;
    }

  if (output_bytes_per_pixel == 0)
    {
      return -EINVAL;
    }

  if (config->hactive > SIZE_MAX / config->vactive ||
      (size_t)config->hactive * config->vactive >
        SIZE_MAX / bytes_per_pixel)
    {
      return -EOVERFLOW;
    }

  required_bytes = (size_t)config->hactive * config->vactive *
                   bytes_per_pixel;
  if (config->frame_buffer_bytes < required_bytes ||
      (required_bytes % ESP_MIPI_DSI_DMA_TRANSFER_WIDTH_BYTES) != 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->ready)
    {
      ret = -ESHUTDOWN;
      goto errout_unlock;
    }

  if (priv->video_running)
    {
      ret = -EALREADY;
      goto errout_unlock;
    }

  ret = esp_mipi_dsi_enable_dpi_clock(priv, config->pixel_clock_hz);
  if (ret < 0)
    {
      goto errout_unlock;
    }

  /* The bridge timing registers require both bridge clocks before their
   * configuration is written.  Unlike M2a, M2b uses DW-GDMA as the actual
   * pixel producer and therefore does not select the Host pattern source.
   */

  mipi_dsi_brg_ll_force_enable_reg_clock(priv->hal.bridge, true);
  mipi_dsi_brg_ll_enable_ref_clock(priv->hal.bridge, true);
  esp_mipi_dsi_video_configure_host(
    priv, config->channel, config->hactive, config->hsync,
    config->hback_porch, config->hfront_porch, config->vactive,
    config->vsync, config->vback_porch, config->vfront_porch,
    config->hsync_active_low, config->vsync_active_low, output_format);
  mipi_dsi_host_ll_dpi_set_pattern_type(priv->hal.host,
                                        MIPI_DSI_PATTERN_NONE);
  esp_mipi_dsi_video_configure_bridge(
    priv, config->hactive, config->vactive,
    bytes_per_pixel * 8, input_format, output_format,
    MIPI_DSI_LL_FLOW_CONTROLLER_DMA);
  mipi_dsi_brg_ll_set_multi_block_number(priv->hal.bridge, 1);
  mipi_dsi_brg_ll_set_burst_len(priv->hal.bridge,
                                 ESP_MIPI_DSI_DMA_BURST_WORDS);
  mipi_dsi_brg_ll_set_empty_threshold(priv->hal.bridge,
                                       ESP_MIPI_DSI_DMA_EMPTY_THRESHOLD);
  mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);

  ret = esp_mipi_dsi_video_dma_prepare(priv, config->frame_buffer,
                                       required_bytes);
  if (ret < 0)
    {
      goto errout_video;
    }

  ret = esp_mipi_dsi_bridge_interrupt_prepare(priv);
  if (ret < 0)
    {
      goto errout_video;
    }

  mipi_dsi_brg_ll_enable(priv->hal.bridge, true);
  mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
  mipi_dsi_host_ll_enable_bta(priv->hal.host, false);
  mipi_dsi_host_ll_set_clock_lane_state(
    priv->hal.host, MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);

  /* The Bridge can request pixels as soon as DPI output is enabled.  Match
   * the ESP-IDF DPI panel ordering: make the GDMA producer runnable before
   * enabling Host video mode and Bridge output.
   */

  dw_gdma_ll_channel_enable(priv->dma_dev, ESP_MIPI_DSI_DMA_CHANNEL, true);
  mipi_dsi_host_ll_enable_video_mode(priv->hal.host, true);
  mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, true);
  mipi_dsi_brg_ll_update_dpi_config(priv->hal.bridge);
  mipi_dsi_brg_ll_enable_interrupt(
    priv->hal.bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN, true);
  syslog(LOG_INFO, "INFO: MIPI-DSI Bridge underrun interrupt enabled\n");

  priv->video_running = true;
  esp_mipi_dsi_dump_video_state(priv, "dma-started");
  esp_mipi_dsi_dump_dma_status(priv, "dma-started");
  syslog(LOG_INFO,
         "INFO: MIPI-DSI DPI DMA started channel=%u size=%ux%u "
         "pixel_clock_hz=%" PRIu32 " frame_bytes=%zu\n",
         config->channel, config->hactive, config->vactive,
         config->pixel_clock_hz, required_bytes);
  nxmutex_unlock(&priv->lock);
  return OK;

errout_video:
  mipi_dsi_host_ll_enable_video_mode(priv->hal.host, false);
  mipi_dsi_brg_ll_enable_dpi_output(priv->hal.bridge, false);
  esp_mipi_dsi_video_dma_release(priv);
  mipi_dsi_brg_ll_enable(priv->hal.bridge, false);
  mipi_dsi_brg_ll_enable_ref_clock(priv->hal.bridge, false);
  mipi_dsi_brg_ll_force_enable_reg_clock(priv->hal.bridge, false);
  esp_mipi_dsi_disable_dpi_clock(priv);
errout_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
#else
  (void)host;
  (void)config;
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_queue_frame_buffer
 ****************************************************************************/

int esp_mipi_dsi_video_dma_queue_frame_buffer(
  FAR struct mipi_dsi_host *host, FAR const void *frame_buffer,
  size_t frame_buffer_bytes)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  irqstate_t flags;
  int ret;

  if (host != &priv->host || frame_buffer == NULL ||
      ((uintptr_t)frame_buffer &
       (ESP_MIPI_DSI_DMA_TRANSFER_WIDTH_BYTES - 1)) != 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->ready)
    {
      ret = -ESHUTDOWN;
    }
  else if (!priv->video_running || priv->dma_dev == NULL)
    {
      ret = -EPIPE;
    }
  else if (frame_buffer_bytes != priv->dma_frame_buffer_bytes)
    {
      ret = -EINVAL;
    }
  else
    {
      /* Only the DMA interrupt consumes this field.  Replacing an older
       * pending page is intentional: for a video scanout, displaying the
       * newest fully rendered framebuffer minimizes UI latency.
       */

      flags = spin_lock_irqsave(&priv->dma_irq_lock);
      priv->dma_pending_frame_buffer = frame_buffer;
      spin_unlock_irqrestore(&priv->dma_irq_lock, flags);
      ret = OK;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
#else
  (void)host;
  (void)frame_buffer;
  (void)frame_buffer_bytes;
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_set_frame_done_callback
 ****************************************************************************/

int esp_mipi_dsi_video_dma_set_frame_done_callback(
  FAR struct mipi_dsi_host *host,
  esp_mipi_dsi_video_dma_frame_done_t callback, FAR void *arg)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  irqstate_t flags;
  int ret;

  if (host != &priv->host)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->ready)
    {
      ret = -ESHUTDOWN;
    }
  else if (!priv->video_running || priv->dma_dev == NULL)
    {
      ret = -EPIPE;
    }
  else
    {
      flags = spin_lock_irqsave(&priv->dma_irq_lock);
      priv->dma_frame_done = callback;
      priv->dma_frame_done_arg = arg;
      spin_unlock_irqrestore(&priv->dma_irq_lock, flags);
      ret = OK;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
#else
  (void)host;
  (void)callback;
  (void)arg;
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: esp_mipi_dsi_dma_buffer_allocate
 ****************************************************************************/

int esp_mipi_dsi_dma_buffer_allocate(size_t bytes, FAR void **buffer)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
  if (buffer == NULL || bytes == 0)
    {
      return -EINVAL;
    }

  /* The ESP HAL heap_caps compatibility layer maps non-retention requests
   * to kmm_memalign().  On this board CONFIG_MM_KERNEL_HEAP reserves that
   * heap for internal SRAM, while CONFIG_ESPRESSIF_SPIRAM_USER_HEAP makes
   * the standard NuttX user heap the PSRAM heap.  Allocate through memalign
   * deliberately instead of asking heap_caps for MALLOC_CAP_SPIRAM, so the
   * 1024x600 RGB888 scanout buffer is not constrained by the small internal
   * kernel heap.
   */

  *buffer = memalign(ESP_MIPI_DSI_DMA_BUFFER_ALIGNMENT, bytes);
  if (*buffer == NULL)
    {
      syslog(LOG_ERR,
             "ERROR: MIPI-DSI DMA PSRAM allocation failed bytes=%zu\n",
             bytes);
      return -ENOMEM;
    }

  memset(*buffer, 0, bytes);
  syslog(LOG_INFO,
         "INFO: MIPI-DSI DMA PSRAM buffer allocated bytes=%zu "
         "alignment=%u\n",
         bytes, ESP_MIPI_DSI_DMA_BUFFER_ALIGNMENT);
  return OK;
#else
  (void)bytes;
  (void)buffer;
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: esp_mipi_dsi_dma_buffer_sync_for_device
 ****************************************************************************/

int esp_mipi_dsi_dma_buffer_sync_for_device(FAR void *buffer, size_t bytes)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
  esp_err_t result;

  if (buffer == NULL || bytes == 0)
    {
      return -EINVAL;
    }

  result = esp_cache_msync(buffer, bytes,
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                           ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  return esp_mipi_dsi_clock_result(result);
#else
  (void)buffer;
  (void)bytes;
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: esp_mipi_dsi_dma_buffer_free
 ****************************************************************************/

void esp_mipi_dsi_dma_buffer_free(FAR void *buffer)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
  if (buffer != NULL)
    {
      free(buffer);
    }
#else
  (void)buffer;
#endif
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_dump_status
 ****************************************************************************/

int esp_mipi_dsi_video_dma_dump_status(FAR struct mipi_dsi_host *host,
                                       FAR const char *stage)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO_DMA
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  int ret;

  if (host != &priv->host || stage == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->ready)
    {
      ret = -ESHUTDOWN;
    }
  else if (!priv->video_running || priv->dma_dev == NULL)
    {
      ret = -EPIPE;
    }
  else
    {
      esp_mipi_dsi_dump_dma_status(priv, stage);
      ret = OK;
    }

  nxmutex_unlock(&priv->lock);
  return ret;
#else
  (void)host;
  (void)stage;
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_phy_sample_status
 ****************************************************************************/

int esp_mipi_dsi_video_phy_sample_status(
  FAR struct mipi_dsi_host *host, FAR const char *stage,
  uint32_t sample_count, uint32_t interval_us)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  FAR dsi_host_dev_t *host_dev;
  uint32_t clock_active_samples = 0;
  uint32_t data0_active_samples = 0;
  uint32_t data1_active_samples = 0;
  uint32_t all_active_samples = 0;
  uint32_t any_active_samples = 0;
  uint32_t lock_lost_samples = 0;
  uint32_t stop_transitions = 0;
  uint32_t previous_stop = 0;
  uint32_t first_status = 0;
  uint32_t last_status = 0;
  uint32_t status;
  uint32_t stop;
  uint32_t i;
  int ret;

  if (host != &priv->host || stage == NULL || sample_count == 0 ||
      sample_count > ESP_MIPI_DSI_PHY_SAMPLE_MAX_COUNT ||
      interval_us > ESP_MIPI_DSI_PHY_SAMPLE_MAX_INTERVAL_US)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->ready)
    {
      ret = -ESHUTDOWN;
      goto out_unlock;
    }

  if (!priv->video_running)
    {
      ret = -EPIPE;
      goto out_unlock;
    }

  host_dev = priv->hal.host;
  for (i = 0; i < sample_count; i++)
    {
      status = host_dev->phy_status.val;
      stop = status & ESP_MIPI_DSI_PHY_STOP_MASK;

      if (i == 0)
        {
          first_status = status;
        }
      else if (stop != previous_stop)
        {
          stop_transitions++;
        }

      previous_stop = stop;
      last_status = status;

      if ((status & ESP_MIPI_DSI_PHY_LOCK_MASK) == 0)
        {
          lock_lost_samples++;
        }

      if ((status & ESP_MIPI_DSI_PHY_CLK_STOP_MASK) == 0)
        {
          clock_active_samples++;
        }

      if ((status & ESP_MIPI_DSI_PHY_DATA0_STOP_MASK) == 0)
        {
          data0_active_samples++;
        }

      if ((status & ESP_MIPI_DSI_PHY_DATA1_STOP_MASK) == 0)
        {
          data1_active_samples++;
        }

      if (stop == 0)
        {
          all_active_samples++;
        }

      if (stop != ESP_MIPI_DSI_PHY_STOP_MASK)
        {
          any_active_samples++;
        }

      if (interval_us > 0 && i + 1 < sample_count)
        {
          up_udelay(interval_us);
        }
    }

  syslog(LOG_INFO,
         "INFO: MIPI-DSI PHY sample stage=%s samples=%" PRIu32
         " interval_us=%" PRIu32 " nominal_us=%" PRIu64
         " first=%08" PRIx32 " last=%08" PRIx32
         " transitions=%" PRIu32 " lock_lost=%" PRIu32 "\n",
         stage, sample_count, interval_us,
         (uint64_t)(sample_count - 1) * interval_us,
         first_status, last_status,
         stop_transitions, lock_lost_samples);
  syslog(LOG_INFO,
         "INFO: MIPI-DSI PHY activity stage=%s clk=%" PRIu32 "/%" PRIu32
         "(%" PRIu32 "permille) d0=%" PRIu32 "/%" PRIu32
         "(%" PRIu32 "permille) d1=%" PRIu32 "/%" PRIu32
         "(%" PRIu32 "permille) all=%" PRIu32 " any=%" PRIu32 "\n",
         stage, clock_active_samples, sample_count,
         (uint32_t)((uint64_t)clock_active_samples * 1000 / sample_count),
         data0_active_samples, sample_count,
         (uint32_t)((uint64_t)data0_active_samples * 1000 / sample_count),
         data1_active_samples, sample_count,
         (uint32_t)((uint64_t)data1_active_samples * 1000 / sample_count),
         all_active_samples, any_active_samples);
  ret = OK;

out_unlock:
  nxmutex_unlock(&priv->lock);
  return ret;
#else
  (void)host;
  (void)stage;
  (void)sample_count;
  (void)interval_us;
  return -ENOTSUP;
#endif
}

/****************************************************************************
 * Name: esp_mipi_dsi_video_stop
 ****************************************************************************/

int esp_mipi_dsi_video_stop(FAR struct mipi_dsi_host *host)
{
#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO
  FAR struct esp_mipi_dsi_s *priv = &g_esp_mipi_dsi;
  int ret;

  if (host != &priv->host)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  esp_mipi_dsi_video_stop_locked(priv);
  nxmutex_unlock(&priv->lock);
  return OK;
#else
  (void)host;
  return -ENOTSUP;
#endif
}
