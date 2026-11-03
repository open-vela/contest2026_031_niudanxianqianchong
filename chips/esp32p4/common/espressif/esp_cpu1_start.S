/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_cpu1_start.S
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <arch/irq.h>

#include "riscv_internal.h"
#include "chip.h"

  .section .iram1, "ax"
  .balign 4
  .global esp_cpu1_start
  .type esp_cpu1_start, @function

esp_cpu1_start:
  csrc CSR_STATUS, STATUS_IE

  .option push
  .option norelax
  la   gp, __global_pointer$
  .option pop

  /* CPU0 uses the first idle stack above BSS; CPU1 uses the next one. */

  la   sp, _ebss
  li   t0, 2 * SMP_STACK_SIZE
  add  sp, sp, t0
  mv   tp, zero
  fence rw, rw
  tail esp_cpu1_boot

  .size esp_cpu1_start, . - esp_cpu1_start
