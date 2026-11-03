#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Board verification harness for ESP32-P4X SMP SmartHome.

Runs the checks that are automatable over the USB Serial/JTAG console:
  * boot to NSH with CPU1 ready
  * ICMP connectivity (DHCP implied by a successful ping)
  * /dev/video0 registration
  * /data/secrets.json presence
  * SmartHome UI startup pinned to CPU0

Physical touch, display quality and the miloco config round-trip still need a
human.  The script prints MANUAL items and saves the console log.

Usage:
  scripts/verify_smp_smart_home.py [--port /dev/ttyACM0] [--image nuttx/nuttx.bin]
                                   [--no-flash] [--ping-target 223.5.5.5]
"""

import argparse
import glob
import os
import subprocess
import sys
import time

import serial

BOOT_TIMEOUT = 25
CMD_TIMEOUT = 20


def find_port(explicit):
    if explicit:
        return explicit
    ports = sorted(glob.glob('/dev/ttyACM*'))
    if not ports:
        raise SystemExit('no /dev/ttyACM* device found')
    return ports[0]


def open_port(port):
    for _ in range(20):
        try:
            return serial.Serial(port, 115200, timeout=0.3)
        except Exception:
            time.sleep(0.5)
    raise SystemExit(f'cannot open {port}')


def read_for(ser, seconds, stop=None):
    buf = b''
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            data = ser.read(4096)
        except Exception:
            time.sleep(0.1)
            continue
        if data:
            buf += data
            if stop and stop in buf[-200:]:
                break
    return buf.decode('utf-8', 'replace')


def send(ser, command, wait=CMD_TIMEOUT):
    ser.write((command + '\r').encode())
    return read_for(ser, wait, stop=b'nsh>')


def reset_board(ser):
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)


def reopen_and_boot(old_port):
    ser = None
    for _ in range(30):
        try:
            port = find_port(None)
            ser = serial.Serial(port, 115200, timeout=0.3)
            break
        except Exception:
            time.sleep(0.5)
    if ser is None:
        raise SystemExit('board did not re-enumerate after reboot')
    reset_board(ser)
    log = read_for(ser, BOOT_TIMEOUT, stop=b'nsh>')
    return ser, log


def check_boot(text):
    return 'CPU1 ready' in text and 'NuttShell (NSH)' in text


def check_ping(text):
    lowered = text.lower()
    return ('0% packet loss' in lowered or
            '3 received' in lowered or
            ' 3 packets transmitted, 3 received' in lowered)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port')
    ap.add_argument('--image', default='nuttx/nuttx.bin')
    ap.add_argument('--no-flash', action='store_true')
    ap.add_argument('--ping-target', default='223.5.5.5')
    ap.add_argument('--log', default='out/verify_smp_smart_home.log')
    args = ap.parse_args()

    root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
    port = find_port(args.port)

    if not args.no_flash:
        image = os.path.join(root, args.image)
        if not os.path.exists(image):
            raise SystemExit(f'missing image: {image}')
        print(f'[flash] {image} -> {port}')
        subprocess.run([
            'esptool', '--chip', 'esp32p4', '--port', port, '--baud', '921600',
            '--after', 'hard-reset', 'write-flash', '-fs', '16MB', '-fm', 'dio',
            '-ff', '80m', '0x2000', image
        ], check=True)

    ser = open_port(port)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)

    log = read_for(ser, BOOT_TIMEOUT, stop=b'nsh>')
    results = {}
    results['boot'] = check_boot(log)
    results['video0'] = False
    results['secrets'] = False
    results['ping'] = False
    results['camera'] = False
    results['miloco_write'] = False
    results['miloco_roundtrip'] = False
    results['ui_cpu0'] = False

    if results['boot']:
        out = send(ser, 'ls /dev/video0')
        results['video0'] = '/dev/video0' in out

        out = send(ser, 'ls -l /data/secrets.json')
        results['secrets'] = 'secrets.json' in out and 'No such' not in out

        out = send(ser, f'ping -c 3 {args.ping_target}', wait=25)
        results['ping'] = check_ping(out)
        log += out

        cam = send(ser, 'smart_home --selftest-camera', wait=20)
        log += cam
        results['camera'] = 'camera PASS' in cam

        # miloco persistence: write a marker, reboot, read it back through the
        # same secrets API used by the UI.
        write = send(ser, 'smart_home --selftest-miloco-write')
        log += write
        results['miloco_write'] = 'miloco write PASS' in write

        ser.write(b'taskset 1 reboot\r')
        time.sleep(2)
        try:
            ser.close()
        except Exception:
            pass
        ser, boot2 = reopen_and_boot(port)
        log += boot2

        read = send(ser, 'smart_home --selftest-miloco-read')
        log += read
        results['miloco_roundtrip'] = 'miloco read PASS' in read

        send(ser, 'taskset 1 smart_home &', wait=3)
        ui = read_for(ser, 20)
        log += ui
        results['ui_cpu0'] = ('[smart_home_cpu]' in ui and 'cpu=0' in ui)

        ps = send(ser, 'ps')
        log += ps
        for line in ps.splitlines():
            if 'smart_home' in line and '0x00000001' in line:
                results['ui_cpu0'] = results['ui_cpu0'] or True

    ser.close()

    os.makedirs(os.path.dirname(os.path.join(root, args.log)), exist_ok=True)
    with open(os.path.join(root, args.log), 'w', encoding='utf-8') as f:
        f.write(log)

    print('\n=== results ===')
    for key, ok in results.items():
        print(f'{key:10s} {"PASS" if ok else "FAIL"}')
    print('\nMANUAL: touch response and display quality')
    print(f'log: {os.path.join(root, args.log)}')

    return 0 if all(results.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
