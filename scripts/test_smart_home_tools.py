#!/usr/bin/env python3
"""Exercise V2 SmartHome tool callbacks with mock device and gateway services."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
app = repo / "demos/smart_home"
cjson = repo.parent / "apps/netutils/cjson/cJSON"
with tempfile.TemporaryDirectory(prefix="smart-home-tools-") as temporary:
    work = Path(temporary)
    (work / "nuttx").mkdir()
    (work / "nuttx/config.h").write_text("#pragma once\n#define FAR\n")
    includes = [work, repo / "packages/cAGENT/include", app / "src", cjson,
                *sorted((repo / "packages/cAGENT/ports").glob("*/include")),
                repo / "packages/cAGENT/providers/model/openai/include"]
    for miloco in (False, True):
        sources = [app / "src/tools/smart_home_tools.c", app / "tests/tools_v2_test.c", cjson / "cJSON.c"]
        if miloco:
            sources += [app / "src/miloco/smart_home_miloco_tools.c",
                        app / "src/smart_home_memory.c"]
        executable = work / ("tools-miloco" if miloco else "tools")
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                   '-DAGENT_BUILD_CONFIG_HEADER="nuttx/config.h"']
        if miloco:
            command.append("-DCONFIG_SMART_HOME_MILOCO_BRIDGE=1")
        command += [item for path in includes for item in ["-I", str(path)]]
        subprocess.run([*command, *map(str, sources), "-lm", "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1"))
