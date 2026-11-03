/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_graphics_context.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_GRAPHICS_CONTEXT_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_GRAPHICS_CONTEXT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <errno.h>
#include <nuttx/arch.h>
#include <nuttx/sched.h>

#if defined(CONFIG_SMP) && !defined(CONFIG_ESP32P4_SMP_GRAPHICS_CPU0)
#  error "SMP graphics requires ESP32P4_SMP_GRAPHICS_CPU0"
#endif

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

/* Check affinity as well as the current CPU: a blocking call may migrate. */

static inline int esp_graphics_check_context(void)
{
#ifdef CONFIG_SMP
  cpu_set_t mask;
  cpu_set_t cpu0;
  int ret;
#endif

  if (up_interrupt_context())
    {
      return -EPERM;
    }

#ifdef CONFIG_SMP
  ret = nxsched_get_affinity(0, sizeof(mask), &mask);
  if (ret < 0)
    {
      return ret;
    }

  CPU_ZERO(&cpu0);
  CPU_SET(0, &cpu0);
  if (!CPU_EQUAL(&mask, &cpu0) || up_cpu_index() != 0)
    {
      return -EXDEV;
    }
#endif

  return OK;
}

#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_GRAPHICS_CONTEXT_H */
