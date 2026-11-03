#!/usr/bin/env python3
"""Verify real V2 context/Skills/device services with a deterministic mock model."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
app = repo / "demos/smart_home"
package = repo / "packages/cAGENT"
cjson = repo.parent / "apps/netutils/cjson/cJSON"
with tempfile.TemporaryDirectory(prefix="smart-home-context-") as temporary:
    work = Path(temporary)
    shutil.copytree(app / "res/skills", work / "data/res/skills")
    (work / "nuttx").mkdir()
    (work / "nuttx/config.h").write_text(
        '#pragma once\n#define FAR\n'
        '#define CONFIG_AGENT_SCRATCH_BYTES 32768\n'
        '#define CONFIG_AGENT_CORE_WORKSPACE_BYTES 65536\n'
        '#define CONFIG_AGENT_MAX_TOOL_OUTPUT_BYTES 4096\n'
        f'#define CONFIG_SMART_HOME_DEMO_DATA_ROOT "{work}/data"\n')
    sources = sorted(package.glob("src/*/*.c"))
    sources += [package / "codecs/json/reader.c", package / "codecs/json/writer.c",
                package / "ports/runtime/src/runtime.c", cjson / "cJSON.c"]
    sources += [app / "src" / name for name in [
        "device/smart_home_device.c", "device/smart_home_device_service.c",
        "skills/smart_home_scene_catalog.c", "skills/smart_home_skill_loader.c",
        "skills/smart_home_skills.c", "tools/smart_home_tools.c"]]
    sources.append(app / "tests/context_v2_test.c")
    includes = [work, package / "include", package / "src", package / "codecs/json",
                package / "codecs/json/vendor/jsmn", package / "ports/runtime/include", app / "src", cjson]
    executable = work / "context"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
               "-Wall", "-Wextra", "-Werror", "-g", "-pthread", "-fsanitize=address,undefined", "-no-pie",
               '-DAGENT_BUILD_CONFIG_HEADER="nuttx/config.h"']
    command += [item for path in includes for item in ["-I", str(path)]]
    subprocess.run([*command, *map(str, sources), "-lm", "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30,
                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1"))
    shutil.copytree(app / "res/skills", work / "data/res/res/skills")
    (work / "data/res/skills/smart_home_safety.md").write_text("invalid front matter")
    subprocess.run([str(executable), "invalid"], check=True, timeout=30,
                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1"))
