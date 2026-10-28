/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_atomic64.c
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

#include <stdbool.h>
#include <stdint.h>
#include <nuttx/spinlock.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RV32 implements native atomics up to 32 bits.  All 64-bit operations share
 * this lock; IRQ masking also prevents re-entry from an ISR on the same CPU.
 * The lock provides sequential consistency for every requested memory order.
 */

#define ATOMIC64_OP(name, expression) \
  uint64_t __atomic_fetch_##name##_8(volatile void *ptr, uint64_t value, \
                                     int memorder) \
  { \
    volatile uint64_t *obj = ptr; \
    irqstate_t flags = spin_lock_irqsave(&g_atomic64_lock); \
    uint64_t old = *obj; \
    *obj = (expression); \
    spin_unlock_irqrestore(&g_atomic64_lock, flags); \
    return old; \
  } \
  uint64_t __atomic_##name##_fetch_8(volatile void *ptr, uint64_t value, \
                                     int memorder) \
  { \
    volatile uint64_t *obj = ptr; \
    irqstate_t flags = spin_lock_irqsave(&g_atomic64_lock); \
    uint64_t old = *obj; \
    uint64_t result = (expression); \
    *obj = result; \
    spin_unlock_irqrestore(&g_atomic64_lock, flags); \
    return result; \
  }

/****************************************************************************
 * Private Data
 ****************************************************************************/

static spinlock_t g_atomic64_lock = SP_UNLOCKED;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

uint64_t __atomic_load_8(const volatile void *ptr, int memorder)
{
  const volatile uint64_t *obj = ptr;
  irqstate_t flags = spin_lock_irqsave(&g_atomic64_lock);
  uint64_t value = *obj;

  spin_unlock_irqrestore(&g_atomic64_lock, flags);
  return value;
}

void __atomic_store_8(volatile void *ptr, uint64_t value, int memorder)
{
  volatile uint64_t *obj = ptr;
  irqstate_t flags = spin_lock_irqsave(&g_atomic64_lock);

  *obj = value;
  spin_unlock_irqrestore(&g_atomic64_lock, flags);
}

uint64_t __atomic_exchange_8(volatile void *ptr, uint64_t value,
                              int memorder)
{
  volatile uint64_t *obj = ptr;
  irqstate_t flags = spin_lock_irqsave(&g_atomic64_lock);
  uint64_t old = *obj;

  *obj = value;
  spin_unlock_irqrestore(&g_atomic64_lock, flags);
  return old;
}

bool __atomic_compare_exchange_8(volatile void *ptr, void *expected,
                                 uint64_t desired, bool weak,
                                 int success, int failure)
{
  volatile uint64_t *obj = ptr;
  uint64_t *expect = expected;
  irqstate_t flags = spin_lock_irqsave(&g_atomic64_lock);
  bool equal = *obj == *expect;

  if (equal)
    {
      *obj = desired;
    }
  else
    {
      *expect = *obj;
    }

  spin_unlock_irqrestore(&g_atomic64_lock, flags);
  return equal;
}

ATOMIC64_OP(add, old + value)
ATOMIC64_OP(sub, old - value)
ATOMIC64_OP(and, old & value)
ATOMIC64_OP(or, old | value)
ATOMIC64_OP(xor, old ^ value)
ATOMIC64_OP(nand, ~(old & value))
