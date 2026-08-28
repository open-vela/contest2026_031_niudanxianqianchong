/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/include/board.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_BOARD_H
#define __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO pins used by the GPIO Subsystem */

#define BOARD_NGPIOOUT    2 /* Amount of GPIO Output pins */
#define BOARD_NGPIOINT    1 /* Amount of GPIO Input w/ Interruption pins */

/* ESP32P4-Generic GPIOs ****************************************************/

/* BOOT Button */

#define BUTTON_BOOT  35

/* MIPI-DSI panel control pins.  GPIO27 is the active-low RST_LCD signal on
 * the ESP32-P4X Function EV Board LCD adapter.  GPIO26 controls panel
 * backlight PWM and remains unused by the command-only DSI probe.
 */

#define BOARD_MIPI_DSI_PANEL_RESET_GPIO  27
#define BOARD_MIPI_DSI_BACKLIGHT_GPIO     26

/* GT911 shares I2C0 with the P4X LCD adapter.  The official adapter does
 * not route the GT911 reset or interrupt pins to the SoC, so board bring-up
 * deliberately uses the driver's polling mode rather than inventing GPIO
 * ownership for those signals.
 */

#define BOARD_GT911_I2C_BUS               0
#define BOARD_GT911_I2C_ADDRESS            0x5d
#define BOARD_GT911_I2C_BACKUP_ADDRESS     0x14
#define BOARD_GT911_I2C_FREQUENCY          100000
#define BOARD_GT911_POLL_INTERVAL_MS       20
#define BOARD_GT911_X_RESOLUTION            1024
#define BOARD_GT911_Y_RESOLUTION             600

/* Keep the board endpoint compatible with the stock LVGL NuttX input
 * adapter when an older external-board Kconfig refresh does not materialize
 * the optional max-points symbol in nuttx/config.h.
 */

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911_MAX_POINTS
#  define BOARD_GT911_MAX_POINTS \
    CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911_MAX_POINTS
#else
#  define BOARD_GT911_MAX_POINTS 1
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct mipi_dsi_host;
struct esp_mipi_dsi_dpi_panel_config_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_MIPI_DSI

/****************************************************************************
 * Name: board_mipi_dsi_initialize
 *
 * Description:
 *   Initialize the P4X command-mode MIPI-DSI host using the board PHY LDO
 *   and link parameters.  This does not reset the panel or enable video
 *   scanout.  With video support enabled it prepares the backlight GPIO in
 *   its off state; the caller enables it only after a frame is submitted.
 *
 ****************************************************************************/

int board_mipi_dsi_initialize(FAR struct mipi_dsi_host **host);

/****************************************************************************
 * Name: board_mipi_dsi_panel_reset
 *
 * Description:
 *   Apply the board-specific active-low hardware reset sequence for the LCD
 *   adapter.  This does not send panel DCS commands.
 *
 ****************************************************************************/

int board_mipi_dsi_panel_reset(void);

#ifdef CONFIG_ESPRESSIF_MIPI_DSI_VIDEO

/****************************************************************************
 * Name: board_mipi_dsi_dpi_panel_config_get
 *
 * Description:
 *   Return the tested EK79007 1024x600 DPI profile.  The configuration
 *   matches Espressif's EK79007 60 Hz macro: two lanes, RGB565 and 52 MHz
 *   nominal pixel clock.  The returned object has static lifetime.
 *
 ****************************************************************************/

FAR const struct esp_mipi_dsi_dpi_panel_config_s *
board_mipi_dsi_dpi_panel_config_get(void);

/****************************************************************************
 * Name: board_mipi_dsi_video_pattern_start
 *
 * Description:
 *   Start the P4X Host built-in vertical colour-bar scanout path.  This is
 *   the framebuffer-free M2a profile: the Bridge uses its own flow
 *   controller and no GDMA channel is created.  It requires a clean video
 *   state and must not be called on top of a running DMA scanout pipeline.
 *
 ****************************************************************************/

int board_mipi_dsi_video_pattern_start(FAR struct mipi_dsi_host *host);

/****************************************************************************
 * Name: board_mipi_dsi_video_dump_status
 *
 * Description:
 *   Print a board-visible snapshot of the active P4 DSI DMA scanout.  It is
 *   a bring-up diagnostic and does not clear pending hardware status bits.
 *
 ****************************************************************************/

int board_mipi_dsi_video_dump_status(FAR struct mipi_dsi_host *host,
                                     FAR const char *stage);

/****************************************************************************
 * Name: board_mipi_dsi_video_sample_phy_status
 *
 * Description:
 *   Collect a read-only high-frequency D-PHY lane-status sample while video
 *   is running.  This diagnostic does not force the clock or data lanes.
 *
 ****************************************************************************/

int board_mipi_dsi_video_sample_phy_status(
  FAR struct mipi_dsi_host *host, FAR const char *stage,
  uint32_t sample_count, uint32_t interval_us);

/****************************************************************************
 * Name: board_mipi_dsi_video_stop
 *
 * Description:
 *   Stop the P4X DPI scanout path before command Host shutdown.
 *
 ****************************************************************************/

int board_mipi_dsi_video_stop(FAR struct mipi_dsi_host *host);

/****************************************************************************
 * Name: board_mipi_dsi_backlight_set
 *
 * Description:
 *   Drive the LCD adapter PWM input to a static on/off level for board
 *   bring-up.  Duty-cycle control is intentionally deferred to the later
 *   board PWM integration.
 *
 ****************************************************************************/

int board_mipi_dsi_backlight_set(bool enable);

#endif /* CONFIG_ESPRESSIF_MIPI_DSI_VIDEO */

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_DSI_FRAMEBUFFER

/****************************************************************************
 * Name: board_mipi_dsi_fb_initialize
 *
 * Description:
 *   Initialize the EK79007 RGB565 scanout path and register its persistent
 *   P4 DMA-capable PSRAM buffer as /dev/fb0.
 *
 ****************************************************************************/

int board_mipi_dsi_fb_initialize(int display);

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_DSI_FRAMEBUFFER */

/****************************************************************************
 * Name: board_mipi_dsi_shutdown
 *
 * Description:
 *   Stop the command-mode Host and release its board-owned D-PHY LDO.
 *
 ****************************************************************************/

int board_mipi_dsi_shutdown(FAR struct mipi_dsi_host *host);

#endif /* CONFIG_ESPRESSIF_MIPI_DSI */

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911

/****************************************************************************
 * Name: board_gt911_initialize
 *
 * Description:
 *   Initialize the P4X Goodix GT911 touch controller on I2C0 and register
 *   the touchscreen upper-half endpoint at /dev/input0.
 *
 ****************************************************************************/

int board_gt911_initialize(void);

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_GT911 */

#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_BOARD_H */
