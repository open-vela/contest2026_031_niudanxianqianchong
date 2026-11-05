#!/usr/bin/env python3
"""Exercise the real SmartHome application with the generated P4 standard capacities."""
from pathlib import Path
import argparse
import os
import re
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--persistent", action="store_true", help="Use real filesystem Session storage")
persistent = parser.parse_args().persistent
repo = Path(__file__).resolve().parents[1]
package = repo / "packages/cAGENT"
app = repo / "demos/smart_home"
cjson = repo.parent / "apps/netutils/cjson/cJSON"
configuration = (repo.parent / "nuttx/.config").read_text()
if not re.search(r'^CONFIG_BASE_DEFCONFIG="[^"]*/configs/smart_home"$', configuration, re.M):
    raise SystemExit("Configure/build P4 smart_home before verifying its generated capacities")
if "CONFIG_ARCH_CHIP_ESP32P4=y" not in configuration.splitlines():
    raise SystemExit("Expected ESP32-P4 standard firmware configuration")
for option in ("SMART_HOME_DEMO", "SMART_HOME_DEMO_UI_LVGL", "SMART_HOME_MILOCO_BRIDGE", "CAGENT_SESSION_RAM"):
    if f"CONFIG_{option}=y" not in configuration.splitlines():
        raise SystemExit(f"Standard profile requires CONFIG_{option}=y")
for option in ("SMART_HOME_NODE_GATEWAY", "SMART_HOME_MCP_BRIDGE", "SMART_HOME_APP_BRIDGE",
               "SMART_HOME_VOICE_TTS", "SMART_HOME_VOICE_ASR", "SMART_HOME_KWS"):
    if f"CONFIG_{option}=y" in configuration.splitlines():
        raise SystemExit(f"This test covers the standard profile; CONFIG_{option} requires its own integration test")
with tempfile.TemporaryDirectory(prefix="smart-home-standard-") as temporary:
    work = Path(temporary)
    shutil.copytree(app / "res/skills", work / "data/res/skills")
    data = work / "data/smart_home"
    data.mkdir()
    (data / "settings.json").write_text('{"version":1,"active_backend_id":"deepseek"}')
    (data / "secrets.json").write_text('{"version":1,"model_api_keys":{"deepseek":"mock-test-key"}}')
    profile = "#pragma once\n#define FAR\n#define CONFIG_SMART_HOME_MILOCO_BRIDGE 1\n"
    if "CONFIG_SMART_HOME_USER_MEMORY=y" in configuration.splitlines():
        profile += "#define CONFIG_SMART_HOME_USER_MEMORY 1\n"
    if persistent:
        profile += "#define CONFIG_SMART_HOME_SESSION_PERSIST 1\n"
    for name, value in re.findall(r'^CONFIG_((?:AGENT_|SMART_HOME_MODEL_|SMART_HOME_SESSION_)[A-Z0-9_]+)=(\d+)$', configuration, re.M):
        profile += f"#define CONFIG_{name} {value}\n"
    profile += f'#define CONFIG_SMART_HOME_MODEL_SECRETS_PATH "{data}/secrets.json"\n'
    profile += f'#define CONFIG_SMART_HOME_DEMO_DATA_ROOT "{work}/data"\n'
    for name in ("BACKENDS", "SETTINGS", "STATE"):
        profile += f'#define SMART_HOME_CONFIG_{name}_PATH "{data}/{name.lower()}.json"\n'
    (work / "nuttx").mkdir()
    (work / "nuttx/config.h").write_text(profile)
    sources = sorted(package.glob("src/*/*.c"))
    sources += sorted(package.glob("providers/model/openai/src/*.c"))
    sources += [package / "codecs/json/reader.c", package / "codecs/json/writer.c",
                package / "providers/storage/ram/src/session_ram.c", package / "ports/runtime/src/runtime.c", cjson / "cJSON.c"]
    sources += sorted(package.glob("providers/storage/jsonl/src/*.c"))
    sources += sorted(package.glob("providers/storage/files/src/*.c"))
    sources += [package / "ports/posix/storage/src/file_store.c", package / "ports/openvela/storage/src/file_store.c"]
    for directory in ("agent", "config", "device", "skills", "tools"):
        sources += [p for p in (app / "src" / directory).glob("*.c") if p.name != "smart_home_agent_run_service.c"]
    if "CONFIG_SMART_HOME_USER_MEMORY=y" in configuration.splitlines():
        sources += [app / "src/memory/smart_home_user_memory.c"]
    sources += [app / "src/miloco/smart_home_miloco_tools.c", app / "tests/standard_v2_test.c"]
    includes = [work, app / "src", package / "include", package / "src", package / "codecs/json",
                package / "codecs/json/vendor/jsmn", cjson, package / "providers/model/openai/include",
                package / "providers/storage/ram/include", *sorted(package.glob("ports/*/include")),
                package / "providers/storage/jsonl/include", package / "providers/storage/files/include",
                package / "ports/posix/storage/include", package / "ports/openvela/storage/include"]
    executable = work / "standard"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
               "-include", str(work / "nuttx/config.h"),
               '-DAGENT_BUILD_CONFIG_HEADER="nuttx/config.h"', "-Wall", "-Wextra", "-Werror", "-g", "-pthread",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
    command += [item for path in includes for item in ["-I", str(path)]]
    subprocess.run([*command, *map(str, sources), "-lm", "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30,
                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1"))
    if persistent:
        subprocess.run([str(executable), "--resume"], check=True, timeout=30,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1"))
