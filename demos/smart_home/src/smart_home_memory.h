/* SPDX-License-Identifier: Apache-2.0 */

#ifndef SMART_HOME_MEMORY_H
#define SMART_HOME_MEMORY_H

#include <nuttx/config.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_ARCH_CHIP_ESP32S3)
#  define SMART_HOME_SRAM_DATA \
  __attribute__((section(".dram1"), aligned(16)))
#else
#  define SMART_HOME_SRAM_DATA __attribute__((aligned(16)))
#endif

void *smart_home_bulk_alloc(size_t size);
void smart_home_bulk_free(void *pointer);
void smart_home_bulk_diag(const char *point);
void smart_home_memory_log_region(const char *label, const void *pointer);

/* cAGENT V1 的 runtime 提供过 ov_mem_region_log；V2 起该接口随
 * runtime_openvela.h 一并移除，这里映射到本工程自带的区域诊断，
 * 保持各 addon 的调用点不变。 */
#define ov_mem_region_log(label, pointer) \
    smart_home_memory_log_region((label), (pointer))

#ifdef __cplusplus
}
#endif

#endif /* SMART_HOME_MEMORY_H */
