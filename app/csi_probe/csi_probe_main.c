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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arch/board/board.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void csi_probe_usage(FAR const char *program)
{
  fprintf(stderr, "Usage: %s sensor\n", program);
}

static int csi_probe_sensor(void)
{
  uint16_t product_id = 0;
  int ret;

  printf("=== ESP32-P4X SC2336 SCCB Probe ===\n");
  printf("control: I2C%d address=0x%02x frequency=%lu; "
         "CSI/ISP/DMA: disabled\n",
         BOARD_SC2336_I2C_BUS, BOARD_SC2336_I2C_ADDRESS,
         (unsigned long)BOARD_SC2336_I2C_FREQUENCY);

  ret = board_sc2336_probe(&product_id);
  if (ret < 0)
    {
      fprintf(stderr,
              "csi_probe: FAIL step=sc2336_product_id ret=%d\n", ret);
      return EXIT_FAILURE;
    }

  printf("csi_probe: PASS sensor=SC2336 product_id=0x%04x\n",
         product_id);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  if (argc != 2 || strcmp(argv[1], "sensor") != 0)
    {
      csi_probe_usage(argv[0]);
      return EXIT_FAILURE;
    }

  return csi_probe_sensor();
}
