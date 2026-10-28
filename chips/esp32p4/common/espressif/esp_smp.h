/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_smp.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_SMP_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_SMP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#ifdef CONFIG_SMP

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void esp_cpu1_start(void);
void esp_cpu1_boot(void) noreturn_function;
void esp_irq_initialize_cpu(void);

#endif /* __ASSEMBLY__ */
#endif /* CONFIG_SMP */
#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_SMP_H */
