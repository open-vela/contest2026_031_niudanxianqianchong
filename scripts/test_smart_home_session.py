#!/usr/bin/env python3
"""Verify Session persistence with real files and injected write/sync failures."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
package = repo / "packages/cAGENT"
app = repo / "demos/smart_home"
with tempfile.TemporaryDirectory(prefix="smart-home-session-") as temporary:
    work = Path(temporary)
    (work / "nuttx").mkdir()
    (work / "data").mkdir()
    (work / "nuttx/config.h").write_text(
        "#pragma once\n#define CONFIG_SMART_HOME_SESSION_PERSIST 1\n"
        f'#define CONFIG_SMART_HOME_DEMO_DATA_ROOT "{work}/data"\n'
        "#define CONFIG_SMART_HOME_SESSION_FILE_BYTES 65536\n"
        "#define CONFIG_SMART_HOME_SESSION_LINE_BYTES 32768\n")
    sources = sorted(package.glob("src/*/*.c"))
    sources += [package / "codecs/json/reader.c", package / "codecs/json/writer.c"]
    sources += sorted(package.glob("providers/storage/jsonl/src/*.c"))
    sources += [package / "providers/storage/files/src/file_store.c",
                package / "ports/posix/storage/src/file_store.c",
                package / "ports/openvela/storage/src/file_store.c",
                app / "src/agent/smart_home_session.c", app / "tests/session_v2_test.c"]
    includes = [work, app / "src", package / "include", package / "src", package / "codecs/json",
                package / "codecs/json/vendor/jsmn", package / "providers/storage/files/include",
                package / "providers/storage/jsonl/include", package / "ports/posix/storage/include",
                package / "ports/openvela/storage/include"]
    command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
               '-DAGENT_BUILD_CONFIG_HEADER="nuttx/config.h"', "-include", str(work / "nuttx/config.h"),
               "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-no-pie",
               "-Wl,--wrap=write", "-Wl,--wrap=fsync"]
    command += [item for path in includes for item in ["-I", str(path)]]
    executable = work / "session"
    subprocess.run([*command, *map(str, sources), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30,
                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                            UBSAN_OPTIONS="halt_on_error=1"))
