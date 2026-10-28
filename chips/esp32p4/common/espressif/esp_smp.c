/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_smp.c
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

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <strings.h>
#include <syslog.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/sched_note.h>
#include <arch/barriers.h>

#include "init/init.h"
#include "sched/sched.h"
#include "riscv_internal.h"
#include "riscv_percpu.h"
#include "esp_irq.h"
#include "esp_smp.h"

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_intr_alloc.h"
#include "esp_private/startup_internal.h"
#include "hal/cpu_utility_ll.h"
#include "hal/crosscore_int_ll.h"
#include "hal/interrupt_clic_ll.h"
#include "rom/ets_sys.h"
#include "riscv/rv_utils.h"
#include "soc/interrupts.h"
#include "soc/soc_caps.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if !defined(CONFIG_ESP32P4_SMP) || CONFIG_SMP_NCPUS != 2
#  error "ESP32-P4 SMP requires ESP32P4_SMP and exactly two HP CPUs"
#endif

#if defined(CONFIG_ESPRESSIF_SPIFLASH) || defined(CONFIG_ESPRESSIF_SPIRAM) || \
    defined(CONFIG_PM)
#  error "Flash MTD, PSRAM and power management need separate SMP validation"
#endif

#define ESP_SMP_BOOT_POLLS  100000
#define ESP_SMP_BOOT_US     10

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Startup state is shared before scheduling is available on CPU1. */

static int g_cpu1_status;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int esp_smp_irq_initialize(void)
{
  int cpu = this_cpu();
  int source = ETS_FROM_CPU_INTR0_SOURCE + cpu;
  int ret;

  crosscore_int_ll_clear_interrupt(cpu);
  ret = esp_setup_irq_with_flags(source,
                                ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM,
                                riscv_smp_call_handler, NULL);
  if (ret < 0)
    {
      return ret;
    }

  up_enable_irq(ESP_SOURCE2IRQ(source));
  return OK;
}

static int esp_smp_secondary_initialize(void)
{
  extern esp_system_init_fn_t _esp_system_init_fn_array_start;
  extern esp_system_init_fn_t _esp_system_init_fn_array_end;
  FAR esp_system_init_fn_t *fn;

  /* Run only CPU1's secondary hooks.  The CORE hooks and constructors have
   * already run on CPU0; re-entering __esp_start() would clear shared BSS.
   */

  for (fn = &_esp_system_init_fn_array_start;
       fn < &_esp_system_init_fn_array_end; fn++)
    {
      if (fn->stage == ESP_SYSTEM_INIT_STAGE_SECONDARY &&
          (fn->cores & (1 << 1)) != 0 && fn->fn() != ESP_OK)
        {
          return -EIO;
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int IRAM_ATTR riscv_smp_call_handler(int irq, FAR void *context,
                                    FAR void *arg)
{
  FAR struct tcb_s *tcb;
  int cpu = this_cpu();

  /* Clear before processing requests so a new request can reassert the IPI.
   * P4 has peripheral cross-core interrupts, not CLINT/MSIP interrupts.
   */

  crosscore_int_ll_clear_interrupt(cpu);
  UP_DMB();
  nxsched_smp_call_handler(irq, context, arg);

  tcb = current_task(cpu);
  riscv_savecontext(tcb);
  nxsched_process_delivered(cpu);
  tcb = current_task(cpu);
  riscv_restorecontext(tcb);
  return OK;
}

int IRAM_ATTR up_send_smp_sched(int cpu)
{
  DEBUGASSERT(cpu >= 0 && cpu < CONFIG_SMP_NCPUS);

  UP_DMB();
  crosscore_int_ll_trigger_interrupt(cpu);
  return OK;
}

void up_send_smp_call(cpu_set_t cpuset)
{
  int cpu;

  DEBUGASSERT((cpuset & ~SCHED_ALL_CPUS) == 0);

  for (; cpuset != 0; cpuset &= ~(1 << cpu))
    {
      cpu = ffs(cpuset) - 1;
      up_send_smp_sched(cpu);
    }
}

void esp_cpu1_boot(void)
{
  extern uint32_t _vector_table;
  extern uint32_t _mtvt_table;
  int ret;
  int source;

  ets_set_appcpu_boot_addr(0);
  esp_cpu_intr_set_ivt_addr(&_vector_table);
  esp_cpu_intr_set_mtvt_addr(&_mtvt_table);

#if SOC_BRANCH_PREDICTOR_SUPPORTED
  esp_cpu_branch_prediction_enable();
#endif

#if SOC_CPU_SUPPORT_WFE
  rv_utils_disable_wfe_mode();
#endif

#ifdef CONFIG_RISCV_PERCPU_SCRATCH
  riscv_percpu_add_hart(1);
#endif

  up_update_task(current_task(1));
  DEBUGASSERT(this_cpu() == 1);

#ifdef CONFIG_STACK_COLORATION
  riscv_stack_color(this_task()->stack_alloc_ptr, 0);
#endif

#ifdef CONFIG_ESPRESSIF_REGION_PROTECTION
  esp_cpu_configure_region_protection();
#endif

  for (source = 0; source < ETS_MAX_INTR_SOURCE; source++)
    {
      interrupt_clic_ll_route(1, source, ETS_INVALID_INUM);
    }

  esp_irq_initialize_cpu();
  ret = esp_smp_secondary_initialize();
  if (ret == OK)
    {
      ret = esp_smp_irq_initialize();
    }

  if (ret < 0)
    {
      __atomic_store_n(&g_cpu1_status, ret, __ATOMIC_RELEASE);
      for (; ; )
        {
          asm volatile ("wfi");
        }
    }

  __atomic_store_n(&g_cpu1_status, 1, __ATOMIC_RELEASE);

#ifdef CONFIG_SCHED_INSTRUMENTATION
  sched_note_cpu_started(this_task());
#endif

  up_irq_enable();
  nx_idle_trampoline();
  PANIC();
  for (; ; )
    {
      asm volatile ("wfi");
    }
}

int up_cpu_start(int cpu)
{
  int ret;
  int poll;

  if (cpu != 1 || this_cpu() != 0)
    {
      return -EINVAL;
    }

  ret = esp_smp_irq_initialize();
  if (ret < 0)
    {
      serr("ERROR: CPU0 SMP interrupt setup failed: %d\n", ret);
      PANIC();
      return ret;
    }

#ifdef CONFIG_SCHED_INSTRUMENTATION
  sched_note_cpu_start(this_task(), cpu);
#endif

  __atomic_store_n(&g_cpu1_status, 0, __ATOMIC_RELEASE);
  esp_cpu_unstall(cpu);
  cpu_utility_ll_enable_clock_and_reset_app_cpu();
  cpu_utility_ll_enable_clock_and_reset_app_cpu_int_matrix();
  UP_DMB();
  ets_set_appcpu_boot_addr((uint32_t)esp_cpu1_start);

  for (poll = 0; poll < ESP_SMP_BOOT_POLLS; poll++)
    {
      ret = __atomic_load_n(&g_cpu1_status, __ATOMIC_ACQUIRE);
      if (ret != 0)
        {
          break;
        }

      up_udelay(ESP_SMP_BOOT_US);
    }

  if (ret != 1)
    {
      serr("ERROR: CPU1 startup failed: %d\n", ret == 0 ? -ETIMEDOUT : ret);
      PANIC();
      return ret == 0 ? -ETIMEDOUT : ret;
    }

  syslog(LOG_INFO, "ESP32-P4 SMP: CPU1 ready\n");
  return OK;
}
