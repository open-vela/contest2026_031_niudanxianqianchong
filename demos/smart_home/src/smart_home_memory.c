/* SPDX-License-Identifier: Apache-2.0 */

#include "smart_home_memory.h"
#include <stdio.h>
#include <stdlib.h>

#if defined(CONFIG_ARCH_CHIP_ESP32S3)
#include <arch/esp32s3/chip.h>
#endif

void smart_home_memory_log_region(const char *label, const void *pointer)
{
    const char *region;

    if (!pointer) {
        region = "null";
    }
#if defined(CONFIG_ARCH_CHIP_ESP32S3) && \
    defined(CONFIG_ESP32S3_SPIRAM_BULK_POOL_SIZE) && \
    CONFIG_ESP32S3_SPIRAM_BULK_POOL_SIZE > 0
    else if (esp32s3_ptr_is_extram(pointer)) {
        region = "PSRAM";
    } else {
        region = "SRAM";
    }
#else
    else {
        region = "unknown";
    }
#endif

    printf("[smart_home_mem] %s ptr=%p region=%s\n",
           label ? label : "pointer", pointer, region);
}

void *smart_home_bulk_alloc(size_t size)
{
#if defined(CONFIG_ARCH_CHIP_ESP32S3) && \
    defined(CONFIG_ESP32S3_SPIRAM_BULK_POOL_SIZE) && \
    CONFIG_ESP32S3_SPIRAM_BULK_POOL_SIZE > 0
    return esp32s3_psram_bulk_alloc(size);
#else
    return malloc(size);
#endif
}

void smart_home_bulk_free(void *pointer)
{
#if defined(CONFIG_ARCH_CHIP_ESP32S3) && \
    defined(CONFIG_ESP32S3_SPIRAM_BULK_POOL_SIZE) && \
    CONFIG_ESP32S3_SPIRAM_BULK_POOL_SIZE > 0
    esp32s3_psram_bulk_free(pointer);
#else
    free(pointer);
#endif
}

void smart_home_bulk_diag(const char *point)
{
#if defined(CONFIG_ARCH_CHIP_ESP32S3) && \
    defined(CONFIG_ESP32S3_SPIRAM_BULK_POOL_SIZE) && \
    CONFIG_ESP32S3_SPIRAM_BULK_POOL_SIZE > 0
    struct esp32s3_psram_bulk_info_s info;

    if (esp32s3_psram_bulk_info(&info) == 0) {
        printf("[smart_home_mem] %s psram_bulk total=%zu free=%zu largest=%zu\n",
               point ? point : "bulk", info.total_bytes, info.free_bytes,
               info.largest_free_bytes);
    }
#else
    (void)point;
#endif
}
