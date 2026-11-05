#!/usr/bin/env python3
"""Real Core + real preference files, with deterministic model and I/O faults."""
from pathlib import Path
import os
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
app = repo / 'demos/smart_home'
core = repo / 'packages/cAGENT'
cjson = repo.parent / 'apps/netutils/cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='smart-home-user-memory-') as temporary:
    work = Path(temporary)
    (work / 'nuttx').mkdir()
    (work / 'nuttx/config.h').write_text('#pragma once\n#define FAR\n'
        '#define CONFIG_SMART_HOME_USER_MEMORY 1\n'
        '#define CONFIG_SMART_HOME_USER_MEMORY_MAX_ENTRIES 4\n'
        '#define CONFIG_AGENT_SCRATCH_BYTES 32768\n'
        '#define CONFIG_AGENT_CORE_WORKSPACE_BYTES 65536\n')
    sources = sorted(core.glob('src/*/*.c')) + [
        core / 'codecs/json/reader.c', core / 'codecs/json/writer.c',
        core / 'ports/runtime/src/runtime.c', cjson / 'cJSON.c']
    sources += [app / 'src' / name for name in [
        'memory/smart_home_user_memory.c', 'tools/smart_home_user_memory_tools.c',
        'config/smart_home_config_store.c', 'config/smart_home_backends.c',
        'device/smart_home_device.c']]
    sources += [app / 'tests/user_memory/test_user_memory.c']
    includes = [work, app / 'src', core / 'include', core / 'src',
        core / 'codecs/json', core / 'codecs/json/vendor/jsmn', core / 'ports/runtime/include', cjson]
    command = [os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
        '-DFAR=', '-Wall', '-Wextra', '-Werror', '-g', '-pthread',
        '-fno-omit-frame-pointer', '-fsanitize=address,undefined', '-no-pie',
        '-DAGENT_BUILD_CONFIG_HEADER="nuttx/config.h"', '-Wl,--wrap=rename', '-Wl,--wrap=fsync']
    command += [part for path in includes for part in ['-I', str(path)]]
    binary = work / 'user-memory'
    subprocess.run([*command, *map(str, sources), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(work)], check=True, timeout=30,
        env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))
