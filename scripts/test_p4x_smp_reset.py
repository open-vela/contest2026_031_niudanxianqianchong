#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host call-order checks for the actual P4 HAL restart function; no hardware."""

import argparse
from pathlib import Path
import subprocess
import tempfile


STUBS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IRAM_ATTR
#define CONFIG_SPIRAM 0
#define CONFIG_SPIRAM_INSTRUCTIONS_RODATA 0
#define CONFIG_IDF_ENV_FPGA 0
#define WDT_RWDT 0
#define WDT_MWDT0 1
#define WDT_MWDT1 2
#define WDT_STAGE0 0
#define WDT_STAGE1 1
#define WDT_STAGE_ACTION_RESET_SYSTEM 0
#define WDT_STAGE_ACTION_RESET_RTC 1
#define ESP_INFINITE_LOOP() abort()

typedef struct { int inst; int *mwdt_dev; } wdt_hal_context_t;
static int TIMERG0, TIMERG1;
static jmp_buf reset_done;
static unsigned current_cpu;
static char calls[512];

static void record(const char *name, unsigned cpu)
{
    size_t len = strlen(calls);
    int count = snprintf(calls + len, sizeof(calls) - len, "%s:%u;", name, cpu);
    assert(count > 0 && (size_t)count < sizeof(calls) - len);
}

static void rv_utils_intr_global_disable(void) { record("irq", current_cpu); }
static uint32_t esp_cpu_get_core_id(void) { return current_cpu; }
static uint32_t rtc_clk_slow_freq_get_hz(void) { return 32768; }
static void wdt_hal_init(wdt_hal_context_t *ctx, int inst, int id, bool en)
{
    (void)id; (void)en; ctx->inst = inst;
}
#define wdt_hal_write_protect_disable(ctx) ((void)(ctx))
#define wdt_hal_write_protect_enable(ctx) ((void)(ctx))
#define wdt_hal_set_flashboot_en(ctx, en) ((void)(ctx))
#define wdt_hal_disable(ctx) ((void)(ctx))
#define wdt_hal_config_stage(ctx, stage, ticks, action) \
    do { (void)(ctx); (void)(ticks); } while (0)

static void esp_cpu_reset(unsigned cpu)
{
    record("reset", cpu);
    if (cpu == current_cpu) { longjmp(reset_done, 1); }
}
static void esp_cpu_stall(unsigned cpu) { record("stall", cpu); }
static void esp_cpu_unstall(unsigned cpu) { record("unstall", cpu); }
static void Cache_Disable_L2_Cache(void) { record("cache", 0); }
static void esp_system_reset_modules_on_exit(void) { record("modules", 0); }
static void rtc_clk_cpu_set_to_default_config(void) { record("clock", 0); }
static void ets_set_appcpu_boot_addr(unsigned addr) { record("boot", addr); }
"""

MAIN = r"""
int main(int argc, char **argv)
{
    assert(argc == 3);
    current_cpu = (unsigned)strtoul(argv[1], NULL, 10);
    if (setjmp(reset_done) == 0) { esp_restart_noos(); }
    if (strcmp(calls, argv[2]) != 0) {
        fprintf(stderr, "expected %s\nactual   %s\n", argv[2], calls);
        return 1;
    }
    return 0;
}
"""


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hal", type=Path,
                        default=root / "chips/esp32p4/esp-hal-3rdparty")
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = args.hal / "components/esp_system/port/soc/esp32p4/system_internal.c"
    text = source.read_text()
    # Compile the HAL's real function body, replacing only its hardware calls.
    start = text.index("void IRAM_ATTR esp_restart_noos(void)")
    function = text[start:]
    shared = "cache:0;modules:0;clock:0;"
    cases = [
        (True, 1, 0, "irq:0;reset:1;stall:1;" + shared +
         "boot:0;reset:1;reset:0;"),
        (True, 1, 1, "irq:1;reset:0;stall:0;" + shared +
         "boot:0;reset:0;unstall:0;reset:1;"),
        (False, 1, 0, "irq:0;" + shared + "reset:0;"),
        (False, 0, 0, "irq:0;reset:1;stall:1;" + shared +
         "boot:0;reset:1;reset:0;"),
        (False, 0, 1, "irq:1;reset:0;stall:0;" + shared +
         "boot:0;reset:0;unstall:0;reset:1;"),
    ]
    with tempfile.TemporaryDirectory(prefix="p4-reset-host-") as tmp:
        test = Path(tmp) / "reset.c"
        test.write_text(STUBS + function + MAIN)
        for smp, single_core, cpu, expected in cases:
            executable = Path(tmp) / f"reset-{smp}-{single_core}-{cpu}"
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-function",
                       f"-DCONFIG_ESP_SYSTEM_SINGLE_CORE_MODE={single_core}"]
            if smp:
                command.append("-DCONFIG_SMP=1")
            subprocess.run(command + [str(test), "-o", str(executable)], check=True)
            subprocess.run([str(executable), str(cpu), expected], check=True)
            print(f"PASS: SMP={int(smp)} HAL_SINGLE_CORE={single_core} CPU{cpu}")


if __name__ == "__main__":
    main()
