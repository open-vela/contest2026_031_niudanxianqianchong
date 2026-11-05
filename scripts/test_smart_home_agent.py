#!/usr/bin/env python3
"""Host verification of the V2 Agent layer; platform and business services are mocks."""

from pathlib import Path
import argparse
import os
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--p4", action="store_true", help="Compile P4 objects using the existing NuttX configuration")
    arguments = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    package = repo / "packages/cAGENT"
    app = repo / "demos/smart_home"
    cjson = repo.parent / "apps/netutils/cjson/cJSON"
    includes = [
        package / "include", package / "src", package / "codecs/json",
        package / "codecs/json/vendor/jsmn",
        package / "providers/model/openai/include", package / "providers/storage/ram/include",
        package / "ports/runtime/include", package / "ports/transport/include",
        package / "ports/tls/include", app / "src", cjson,
        repo / "packages/cagent_addons/include",
        *sorted(package.glob("providers/storage/*/include")),
        package / "ports/posix/storage/include", package / "ports/openvela/storage/include",
    ]
    sources = sorted(package.glob("src/*/*.c"))
    sources += [package / "codecs/json/reader.c", package / "codecs/json/writer.c"]
    sources += sorted(package.glob("providers/model/openai/src/*.c"))
    sources += [package / "providers/storage/ram/src/session_ram.c"]
    application = [
        app / "src/agent/smart_home_agent.c",
        app / "src/tools/smart_home_device_inventory.c",
        app / "src/agent/smart_home_agent_backend.c",
        app / "src/agent/smart_home_session.c",
        app / "src/config/smart_home_backends.c",
    ]
    if arguments.p4:
        configuration = (repo.parent / "nuttx/.config").read_text()
        if "CONFIG_ARCH_CHIP_ESP32P4=y" not in configuration.splitlines():
            raise SystemExit("P4 object check requires an existing ESP32-P4 NuttX configuration")
        compiler = repo.parent / "prebuilts/gcc/linux-x86_64/riscv-none-elf/bin/riscv-none-elf-gcc"
        sources += sorted(package.glob("ports/*/src/*.c")) + application
        sources += sorted(package.glob("providers/storage/files/src/*.c"))
        sources += sorted(package.glob("providers/storage/jsonl/src/*.c"))
        sources += [package / "ports/posix/storage/src/file_store.c",
                    package / "ports/openvela/storage/src/file_store.c"]
        sources += [app / "src/tools/smart_home_tools.c",
                    app / "src/memory/smart_home_user_memory.c",
                    app / "src/tools/smart_home_user_memory_tools.c",
                    app / "src/miloco/smart_home_miloco_tools.c",
                    app / "src/device/smart_home_device.c",
                    app / "src/device/smart_home_device_service.c",
                    app / "src/skills/smart_home_skill_loader.c",
                    app / "src/skills/smart_home_skills.c",
                    app / "src/skills/smart_home_scene_catalog.c",
                    app / "src/ui/smart_home_ui.c",
                    app / "src/ui/lvgl/smart_home_lvgl.c",
                    app / "src/ui/lvgl/smart_home_lvgl_agent.c",
                    app / "src/ui/lvgl/smart_home_lvgl_chat.c",
                    app / "src/ui/lvgl/smart_home_lvgl_history.c",
                    app / "src/ui/lvgl/smart_home_lvgl_settings_v9.c",
                    app / "src/ui/lvgl/smart_home_lvgl_wake_settings.c",
                    app / "src/app/smart_home_main.c",
                    app / "src/smart_home_memory.c"]
        native_includes = [repo.parent / "nuttx/include", repo.parent / "apps/include",
                           repo.parent / "apps/crypto/mbedtls/mbedtls/include",
                           repo.parent / "apps/graphics/lvgl",
                           repo.parent / "apps/graphics/lvgl/lvgl", *includes]
        with tempfile.TemporaryDirectory(prefix="smart-home-agent-p4-") as temporary:
            for index, source in enumerate(sources):
                command = [str(compiler), "-std=c99", "-Wall", "-Wextra", "-Werror",
                           "-march=rv32imafc", "-mabi=ilp32", "-D__NuttX__",
                           '-DAGENT_BUILD_CONFIG_HEADER="nuttx/config.h"']
                command += [item for path in native_includes for item in ["-I", str(path)]]
                command += ["-c", str(source), "-o", str(Path(temporary) / f"{index}.o")]
                subprocess.run(command, check=True)
        print(f"PASS: {len(sources)} P4 objects; full firmware link and hardware remain unverified")
        return
    sources += [package / "ports/runtime/src/runtime.c", *application,
                app / "tests/agent_v2_test.c", cjson / "cJSON.c"]
    with tempfile.TemporaryDirectory(prefix="smart-home-agent-v2-") as temporary:
        work = Path(temporary)
        (work / "nuttx").mkdir()
        (work / "nuttx/config.h").write_text("#pragma once\n#define FAR\n")
        flags = [
            "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DFAR=", "-g",
            '-DAGENT_BUILD_CONFIG_HEADER="nuttx/config.h"',
            "-Wall", "-Wextra", "-Werror", "-pthread", "-fno-omit-frame-pointer",
            "-fsanitize=address,undefined", "-no-pie", "-include", str(work / "nuttx/config.h"),
        ]
        include_flags = [item for path in [work, *includes] for item in ["-I", str(path)]]
        for miloco, user_memory in ((False, False), (True, False), (True, True)):
            profile = "#pragma once\n#define FAR\n"
            if miloco:
                profile += ("#define CONFIG_AGENT_MAX_TOOLS 16\n"
                            "#define CONFIG_AGENT_SCRATCH_BYTES 20480\n"
                            "#define CONFIG_AGENT_CORE_WORKSPACE_BYTES 65536\n")
            (work / "nuttx/config.h").write_text(profile)
            if user_memory:
                profile += ('#define CONFIG_SMART_HOME_USER_MEMORY 1\n'
                            '#define CONFIG_AGENT_MAX_CONTEXT_BYTES 8192\n'
                            f'#define CONFIG_SMART_HOME_DEMO_DATA_ROOT "{work}/data"\n')
                (work / "nuttx/config.h").write_text(profile)
            executable = work / ("agent-memory" if user_memory else "agent-miloco" if miloco else "agent")
            definitions = ["-DCONFIG_SMART_HOME_MILOCO_BRIDGE=1"] if miloco else []
            feature_sources = [app / "src/memory/smart_home_user_memory.c",
                               app / "src/tools/smart_home_user_memory_tools.c"] if user_memory else []
            command = [os.environ.get("CC", "cc"), *flags, *definitions, *include_flags,
                       *map(str, sources + feature_sources), "-lm", "-o", str(executable)]
            subprocess.run(command, check=True)
            environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                               UBSAN_OPTIONS="halt_on_error=1")
            subprocess.run([str(executable)], check=True, env=environment, timeout=30)
    print("PASS: Agent layer with real V2 Core/provider and mock HTTP/business services")


if __name__ == "__main__":
    main()
