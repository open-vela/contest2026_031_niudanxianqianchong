# ESP32-P4X Function EV Board

Use this reference only for an OpenVela build targeting
`esp32p4-function-ev-board`. Inspect the active `nuttx/.config` and generated
flash plan before issuing a write command.

## Environment and basic sequence

At the OpenVela repository root, activate the project environment first:

```bash
source myenv/bin/activate
```

For command runners that start a new shell per call, keep activation and the
command together:

```bash
source myenv/bin/activate && ./build.sh <config-path> -j2
```

The common board configuration location is:

```text
contest2026_031_niudanxianqianchong/board/esp32p4/
esp32p4-function-ev-board/configs/<configuration>
```

After a build, confirm the image and active configuration before flashing:

```bash
test -f nuttx/nuttx.bin
rg '^(CONFIG_ARCH_CHIP_ESP32P4|CONFIG_ESPRESSIF_SIMPLE_BOOT|CONFIG_ESPRESSIF_FLASH_)' nuttx/.config
```

## Why an ESP32-P4 Simple Boot image uses `0x2000`

For this board's ESP32-P4 Simple Boot path, the ROM loads the application image
from Flash offset `0x2000`. A valid `nuttx.bin` placed at `0x0` therefore can
still fail at boot: the ROM reads bytes from the middle of that image at
`0x2000` and reports `invalid header`.

Use `0x2000` only when all of the following are true:

- the built target is ESP32-P4;
- the active configuration uses Simple Boot;
- the generated flash command or verified board build rule places `nuttx.bin`
  at `0x2000`.

For that verified case, the firmware write form is:

```bash
source myenv/bin/activate && \
esptool --chip esp32p4 --port <port> --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m 0x2000 nuttx/nuttx.bin
```

Replace `<port>` only after checking it with:

```bash
source myenv/bin/activate && esptool --chip esp32p4 --port <port> chip_id
```

Some local OpenVela trees have historically generated a Simple Boot `make
flash` command with `0x0`. Inspect the actual generated command; do not run it
if it conflicts with the verified P4 ROM location above.

## USB console and UART console

The USB Serial/JTAG device is commonly enumerated as `/dev/ttyACM0`, but that
name is not stable across reconnects. Inspect `/dev/serial/by-id/` and
`/dev/ttyACM*` after flash/reset.

If the final config enables `CONFIG_ESPRESSIF_USBSERIAL=y`, connect after the
device has re-enumerated:

```bash
picocom -b 115200 /dev/ttyACM0
```

If USB serial is disabled and `CONFIG_UART0_SERIAL_CONSOLE=y`, USB ROM output
does not establish that NuttX has a console on the same USB device. Connect a
3.3 V USB-TTL adapter to the configured UART0 pins and use the baud rate from
`CONFIG_UART0_BAUD`.

Exit picocom before opening the same port with esptool, normally with
`Ctrl-A`, then `Ctrl-X`.

## P4X Smart Home LittleFS

The currently documented `smart_home` configuration uses:

```text
CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET=0x800000
Flash size: 16 MiB
LittleFS resource partition: 1 MiB
Mount point: /data
```

Generate the image only after building or checking the matching configuration:

```bash
source myenv/bin/activate && \
contest2026_031_niudanxianqianchong/scripts/make_p4x_littlefs_data_image.sh
```

The documented output is `out/p4x_littlefs_data/data_lfs.bin`. For the active
configuration above, its separate write form is:

```bash
source myenv/bin/activate && \
esptool --chip esp32p4 --port <port> --baud 921600 \
  write-flash -fs 16MB -fm dio -ff 80m \
  0x800000 out/p4x_littlefs_data/data_lfs.bin
```

`0x800000` is not a universal P4X address: it is the Smart Home MTD offset.
Older project states used `0xe00000`, and other configurations may have no
LittleFS partition. Read `CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET` and the final
size setting before every resource write. Verify that the image fits the
partition and that its range does not overlap the application image.

After a resource update, validate on the board:

```text
nsh> ls /data
nsh> ls /data/res
```

The mount and expected resources must exist before attributing an application
startup failure to the UI or application code.
