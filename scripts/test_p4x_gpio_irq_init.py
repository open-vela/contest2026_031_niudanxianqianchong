#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host failure-injection tests of real GPIO init functions; no hardware."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


STUBS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_INTR_FLAG_IRAM 1
#define ESP_INTR_FLAG_DEFAULT 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DEFAULT 0
#define GPIO_NUM_MAX 64
#define GPIO_ISR_CORE_ID_UNINIT 3
#define GPIO_LL_INTR_SOURCE0 7
#define CONFIG_ESPRESSIF_GPIO_IRQ 1
#define CONFIG_SMP_NCPUS 2
#define NR_IRQS 2
#define IRQ_UNMAPPED 0
#define ESP_IRQ_DEMUX 1
#define OK 0
#define LOG_ERR 3
#define GPIO_CHECK(condition, message, error) \
    do { if (!(condition)) return error; } while (0)
#define ESP_LOGE(...) ((void)0)
#define OSINIT_OS_READY() os_ready
#define OS_PORT_GET_CORE_ID() current_cpu
#define PANIC() longjmp(panic_env, 1)

typedef int esp_err_t;
typedef void (*gpio_isr_t)(void *);
typedef void *gpio_isr_handle_t;
typedef struct { gpio_isr_t fn; void *args; } gpio_isr_func_t;
typedef struct {
    int source, intr_alloc_flags;
    void (*fn)(void *);
    void *arg, *handle;
    esp_err_t ret;
} gpio_isr_alloc_t;
static struct {
    int gpio_spinlock;
    uint32_t isr_core_id;
    gpio_isr_func_t *gpio_isr_func;
    gpio_isr_handle_t gpio_isr_handle;
} gpio_context;
static bool os_ready, fail_table, race_install, table_live;
static unsigned current_cpu, allocated_cpu;
static int ipc_calls, alloc_calls, frees, logs, last_error, attached;
static int ipc_result, alloc_result;
static gpio_isr_func_t table[GPIO_NUM_MAX], other_table[GPIO_NUM_MAX];
static int handle, g_handle_map[CONFIG_SMP_NCPUS][NR_IRQS];
static jmp_buf panic_env;

static void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{
    (void)caps;
    assert(n == GPIO_NUM_MAX && size == sizeof(gpio_isr_func_t));
    if (fail_table) return NULL;
    assert(!table_live);
    table_live = true;
    return table;
}
static void heap_caps_free(void *ptr)
{
    assert(ptr == table && table_live);
    table_live = false;
    frees++;
}
/* Ordinary free must never receive the kernel-heap ISR table. */
#define free(ptr) \
    do { (void)(ptr); assert(!"cross-heap free of ISR table"); } while (0)
static void esp_os_enter_critical(int *lock)
{
    (void)lock;
    if (race_install) {
        gpio_context.gpio_isr_func = other_table;
        race_install = false;
    }
}
static void esp_os_exit_critical(int *lock) { (void)lock; }
static int esp_intr_free(void *ptr)
{
    assert(ptr == NULL || ptr == &handle);
    return ESP_OK;
}
static void gpio_intr_service(void *arg) { (void)arg; }
static int esp_os_intr_alloc(int source, int flags, gpio_isr_t fn,
                             void *arg, void *out)
{
    (void)source; (void)flags; (void)fn; (void)arg;
    alloc_calls++;
    allocated_cpu = current_cpu;
    if (alloc_result == ESP_OK) *(void **)out = &handle;
    return alloc_result;
}
static int esp_ipc_call_blocking(unsigned cpu, void (*fn)(void *), void *arg)
{
    unsigned saved = current_cpu;
    ipc_calls++;
    if (!os_ready) return ESP_ERR_INVALID_STATE;
    if (ipc_result != ESP_OK) return ipc_result;
    current_cpu = cpu;
    fn(arg);
    current_cpu = saved;
    return ESP_OK;
}
static int test_syslog(int priority, const char *fmt, int error)
{
    assert(priority == LOG_ERR && error != ESP_OK);
    (void)fmt;
    logs++;
    last_error = error;
    return 0;
}
#define syslog test_syslog
static void esp_cpuint_initialize(void) {}
static void esp_rtcioirqinitialize(void) {}
static void riscv_exception_attach(void) { attached++; }
static void esp_isr_demultiplexing(void) {}
static void irq_attach(int irq, void (*fn)(void), void *arg)
{ (void)irq; (void)fn; (void)arg; }
static void riscv_color_intstack(void) {}
static void up_irq_enable(void) {}

esp_err_t gpio_uninstall_isr_service(void);
esp_err_t gpio_isr_register(void (*fn)(void *), void *arg, int flags,
                          gpio_isr_handle_t *out);
"""

MAIN = r"""
static void reset_case(void)
{
    memset(&gpio_context, 0, sizeof(gpio_context));
    gpio_context.isr_core_id = GPIO_ISR_CORE_ID_UNINIT;
    os_ready = fail_table = race_install = table_live = false;
    current_cpu = allocated_cpu = 0;
    ipc_calls = alloc_calls = frees = logs = last_error = attached = 0;
    ipc_result = alloc_result = ESP_OK;
}
int main(void)
{
    bool early_local = CONFIG_FREERTOS_UNICORE;
    if (setjmp(panic_env) != 0) {
        fprintf(stderr, "unexpected GPIO initialization panic\n");
        return 1;
    }
#ifdef __NuttX__
    early_local = true;
#endif
    reset_case();
    if (early_local) {
        up_irqinitialize();
        assert(ipc_calls == 0 && alloc_calls == 1 && attached == 1);
        assert(gpio_context.gpio_isr_handle == &handle);
        assert(gpio_uninstall_isr_service() == ESP_OK);
        assert(frees == 1 && !table_live);
    } else {
        assert(esp_gpioirqinitialize() == -EIO);
        assert(ipc_calls == 1 && alloc_calls == 0 && frees == 1);
    }

    reset_case(); os_ready = true;
    assert(esp_gpioirqinitialize() == OK);
    assert(ipc_calls == !CONFIG_FREERTOS_UNICORE && alloc_calls == 1);
    assert(allocated_cpu == 0);
    assert(gpio_uninstall_isr_service() == ESP_OK && frees == 1);

#if !CONFIG_FREERTOS_UNICORE
    reset_case(); os_ready = true; gpio_context.isr_core_id = 1;
    assert(esp_gpioirqinitialize() == OK);
    assert(ipc_calls == 1 && allocated_cpu == 1);
    gpio_uninstall_isr_service();

    reset_case(); gpio_context.isr_core_id = 1;
    assert(esp_gpioirqinitialize() == -EIO);
    assert(ipc_calls == 1 && alloc_calls == 0 && frees == 1);
    assert(gpio_context.isr_core_id == GPIO_ISR_CORE_ID_UNINIT);

    reset_case(); os_ready = true; ipc_result = ESP_ERR_INVALID_STATE;
    assert(esp_gpioirqinitialize() == -EIO);
    assert(alloc_calls == 0 && frees == 1 && logs == 1);
    assert(last_error == ESP_ERR_INVALID_STATE);
    ipc_result = ESP_OK;
    assert(esp_gpioirqinitialize() == OK);
    gpio_uninstall_isr_service();
#endif

    reset_case(); fail_table = true;
    if (setjmp(panic_env) == 0) {
        up_irqinitialize();
        assert(!"GPIO initialization failure must stop interrupt setup");
    }
    assert(attached == 0 && logs == 1 && alloc_calls == 0);

    reset_case(); os_ready = true; alloc_result = ESP_ERR_NO_MEM;
    assert(esp_gpioirqinitialize() == -ENOMEM);
    assert(frees == 1 && !table_live && logs == 1);
    assert(gpio_context.gpio_isr_func == NULL);
    assert(gpio_context.gpio_isr_handle == NULL);
    alloc_result = ESP_OK;
    assert(esp_gpioirqinitialize() == OK);
    gpio_uninstall_isr_service();

    reset_case(); os_ready = true; alloc_result = ESP_ERR_NOT_FOUND;
    assert(esp_gpioirqinitialize() == -EIO);
    assert(frees == 1 && !table_live && logs == 1);

    reset_case(); race_install = true;
    assert(gpio_install_isr_service(0) == ESP_ERR_INVALID_STATE);
    assert(frees == 1 && !table_live);
    assert(gpio_context.gpio_isr_func == other_table);
    assert(alloc_calls == 0);
    return 0;
}
"""


def function(text, signature):
    start = text.index(signature)
    end = re.search(r"^}", text[start:], re.MULTILINE)
    if end is None:
        raise ValueError(f"Missing function end: {signature}")
    return text[start:start + end.end()] + "\n"


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hal", type=Path,
                        default=root / "chips/esp32p4/esp-hal-3rdparty")
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    gpio = (args.hal / "components/upper_hal_gpio/src/gpio.c").read_text()
    code = STUBS
    for signature in ("esp_err_t gpio_install_isr_service(",
                      "esp_err_t gpio_uninstall_isr_service(",
                      "static void gpio_isr_register_on_core_static(",
                      "esp_err_t gpio_isr_register("):
        code += function(gpio, signature)
    common = root / "chips/esp32p4/common/espressif"
    code += function((common / "esp_gpio.c").read_text(),
                     "int esp_gpioirqinitialize(")
    code += function((common / "esp_irq.c").read_text(),
                     "void up_irqinitialize(")
    with tempfile.TemporaryDirectory(prefix="p4-gpio-init-host-") as tmp:
        source = Path(tmp) / "gpio.c"
        source.write_text(code + MAIN)
        for nuttx, unicore in ((True, 0), (True, 1), (False, 0)):
            binary = Path(tmp) / f"gpio-{nuttx}-{unicore}"
            command = [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-function",
                       f"-DCONFIG_FREERTOS_UNICORE={unicore}"]
            if nuttx:
                command.append("-D__NuttX__=1")
            subprocess.run(command + [str(source), "-o", str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True)
            print(f"PASS: NuttX={int(nuttx)} UNICORE={unicore}")


if __name__ == "__main__":
    main()
