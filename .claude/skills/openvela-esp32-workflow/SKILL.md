---
name: openvela-esp32-workflow
description: Build, inspect, flash, and verify OpenVela firmware on ESP32 boards, including esptool, serial consoles, and LittleFS resource images. Use for ESP32-P4, ESP32-S3, or ESP32-C6 OpenVela build and board workflows; do not use for generic non-Espressif builds.
metadata:
  short-description: OpenVela ESP32 build and flash
---

# OpenVela ESP32 Workflow

Use this skill for a reproducible OpenVela firmware workflow: build a chosen
defconfig, determine the generated image layout, flash only the requested
images, then verify the selected console and optional LittleFS mount.

## Working directory and Python environment

Find the repository root from `build.sh`. From that directory, activate the
project virtual environment before using Python-dependent tools or project
scripts:

```bash
source myenv/bin/activate
```

In a non-persistent shell, combine activation and the operation in the same
command, for example:

```bash
source myenv/bin/activate && esptool --help
```

If `myenv/bin/activate` is absent, report that fact and locate the repository's
documented environment rather than falling back to a random system `esptool`.

## Workflow

1. Identify the requested board/configuration, serial download port, and
   requested operation: build, firmware flash, resource flash, or console.
   Do not infer a serial device when more than one candidate exists.
2. Activate `myenv`, build the selected configuration with `./build.sh`, and
   confirm successful completion and the actual image artifacts.
3. Inspect the final `nuttx/.config`, generated manifest, and build output to
   derive chip family, boot scheme, flash size, image files, and flash offsets.
   Treat source defaults and old documents as clues, not as the current image
   layout.
4. Before flashing, use `esptool --chip <chip> --port <port> chip_id` and
   display the complete write plan: each image, offset, size, and flash
   parameters. Run a write or erase only when the user has explicitly asked
   to flash that identified target.
5. Release any serial terminal before esptool opens the download port. After
   reset, rediscover the terminal device if USB re-enumeration changed it,
   then connect with `picocom` using the console selected by the final config.
6. Verify the requested result, such as `nsh>`, an application command, or a
   mounted LittleFS directory. Report the exact image layout and validation
   result.

## Build and image checks

- Prefer the configuration path named by the user. For a new build, run
  `./build.sh <config-path> -j<jobs>` from the repository root after activation.
- Do not run `distclean` merely because a build failed. Use it only when the
  user requests it or stale configuration is supported by evidence.
- Check `nuttx/.config`, `nuttx/nuttx.bin`, and generated flash/manifest data
  after a successful build. If `make flash` exists, inspect the command or
  dry-run output before trusting its addresses.
- Keep a build configuration, its resulting `.config`, and its firmware image
  together. Do not flash an image built for a different defconfig.

## Flashing rules

- Use `esptool` from the activated environment. `esptool.py` is acceptable
  only when it is the environment-provided command.
- Never transplant a `write-flash` offset from another ESP32 family, a prior
  boot scheme, or an Internet example. Multi-image bootloader builds and
  Simple Boot builds have different layouts.
- Preserve the configured flash mode, frequency, and size from the generated
  image/build output. Do not add `erase_flash` unless it is explicitly
  requested and its consequences are understood.
- A successful esptool transfer proves that bytes were written. It does not
  prove that the boot address, console route, or application startup is
  correct.

## Console rules

- Inspect the final configuration to distinguish USB Serial/JTAG from UART0.
  ROM output on a USB CDC device and NuttX console output on UART0 can be
  different physical paths.
- Use the configured baud rate. For a `115200` console, a typical command is
  `picocom -b 115200 /dev/ttyACM0` after verifying that this is the correct
  device.
- If no `nsh>` appears but ROM output does, check the console configuration
  before diagnosing the flash image as invalid.

## LittleFS resource updates

When the user requests a LittleFS image or resource flash:

1. Read the final configuration for the enabled storage backend and its MTD
   offset and size. Use the built `.config`, not a guessed board default.
2. Generate the resource image using the repository script for that board.
3. Check that the image size does not exceed the MTD partition and that the
   intended write range lies within the detected/configured flash capacity.
4. Confirm the resource write range does not overlap firmware images in the
   current flash plan.
5. Flash the resource image only when explicitly requested, then boot and
   verify the configured mount point and expected resource files. Do not
   force-format a preloaded resource partition during routine verification.

## ESP32-P4X reference

For `esp32p4-function-ev-board`, read
[references/esp32-p4x.md](references/esp32-p4x.md) before choosing offsets,
LittleFS addresses, or the USB console path. Its values apply only after the
active P4X build configuration confirms the stated boot scheme and storage
options.
