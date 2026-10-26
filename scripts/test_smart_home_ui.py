#!/usr/bin/env python3
"""Run the real asynchronous LVGL bridge using mock model and widget functions."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
app = repo / "demos/smart_home"
package = repo / "packages/cAGENT"
with tempfile.TemporaryDirectory(prefix="smart-home-ui-") as temporary:
    work = Path(temporary)
    (work / "nuttx").mkdir()
    (work / "nuttx/config.h").write_text("#pragma once\n#define FAR\n#define CONFIG_SMART_HOME_DEMO_OFFLINE_UI 1\n")
    (work / "nuttx/sched.h").write_text("#pragma once\n")
    (work / "nuttx/irq.h").write_text(
        "#pragma once\n#define STACK_ALIGNMENT 16u\n#define STACK_ALIGN_UP(a) (((a) + 15u) & ~(uintptr_t)15u)\n")
    includes = [work, app / "src", package / "include",
                repo.parent / "apps/netutils/cjson/cJSON", repo.parent / "apps/graphics/lvgl"]
    command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
               "-DLV_CONF_SKIP=1", '-DAGENT_BUILD_CONFIG_HEADER="nuttx/config.h"',
               "-Wall", "-Wextra", "-Werror", "-g", "-pthread", "-ffunction-sections", "-fdata-sections",
               "-Wl,--gc-sections", "-fsanitize=address,undefined", "-no-pie"]
    command += [item for path in includes for item in ["-I", str(path)]]
    for test in ("ui", "chat"):
        executable = work / test
        subprocess.run([*command, str(app / f"tests/{test}_v2_test.c"), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1"))
