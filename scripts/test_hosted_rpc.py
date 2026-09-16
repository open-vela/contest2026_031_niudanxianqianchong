#!/usr/bin/env python3
"""Host regression checks for the actual C RPC encoder and callback removal.

Compiles selected production functions against a small transport/OS fixture.
No device, credentials, downloaded packages or cross compiler are required.
The independent wire decoder checks against ESP-Hosted 2.9.1 field numbers.
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "chips/esp32p4/common/espressif/esp_hosted_transport.c"


def function(source, name):
    match = re.search(
        r"^(?:static )?(?:int|void|uint16_t) " + name
        + r"\([^;]*?^\{.*?^\}", source, re.M | re.S
    )
    if match is None:
        raise AssertionError(f"Production function not found: {name}")
    return match[0]


FIXTURE = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#define CONFIG_ESPRESSIF_HOSTED_WLAN 1
#define FAR
#define OK 0
#define MSEC2TICK(x) (x)
typedef int (*esp_hosted_transport_wlan_rx_t)(void *, const uint8_t *, size_t);
struct esp_hosted_transport_s {
  bool initialized, data_path_open, rx_active, checksum_enabled, rpc_pending;
  int rpc_lock, rpc_sem, rpc_result;
  uint32_t rpc_uid, rpc_response_id;
  esp_hosted_transport_wlan_rx_t wlan_rx;
  void *wlan_rx_arg;
};
static struct esp_hosted_transport_s g_transport;
static uint8_t captured[256];
static size_t captured_length;
static int send_result, wait_result, remote_result;
static uint32_t expected_response;
static bool link_up;
static unsigned int link_changes;
static int nxmutex_lock(int *lock) { assert(!*lock); *lock = 1; return 0; }
static void nxmutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static int nxsem_trywait(int *sem) { (void)sem; return -EAGAIN; }
static int nxsem_tickwait_uninterruptible(int *sem, int ticks) {
  (void)sem; (void)ticks;
  assert(g_transport.rpc_response_id == expected_response);
  if (!wait_result) {
    g_transport.rpc_pending = false;
    g_transport.rpc_result = remote_result;
  }
  return wait_result;
}
static uint16_t esp_hosted_transport_next_tx_sequence(
    struct esp_hosted_transport_s *t) { (void)t; return 0; }
static int esp_hosted_transport_send_packet(
    struct esp_hosted_transport_s *t, uint8_t *packet, size_t size) {
  (void)t;
  assert(size <= sizeof(captured));
  memcpy(captured, packet, size);
  captured_length = size;
  return send_result;
}
static int callback(void *a, const uint8_t *d, size_t n) {
  (void)a; (void)d; (void)n; return 0;
}
static void esp_hosted_wlan_set_link(bool up) {
  link_up = up;
  link_changes++;
}
static void dump(void) {
  for (size_t i = 0; i < captured_length; i++) printf("%02x", captured[i]);
  puts("");
}
'''

MAIN = r'''
int main(void) {
  int result = -1;
  uint8_t empty_message[2] = {0xff, 0xff};
  size_t offset = 0;
  assert(esp_hosted_transport_append_empty_message(empty_message, 2,
         &offset, 9) == 0);
  assert(offset == 2 && empty_message[0] == 0x4a && empty_message[1] == 0);
  offset = 0;
  assert(esp_hosted_transport_append_empty_message(empty_message, 0,
         &offset, 9) == -EMSGSIZE);
  assert(offset == 0);
  assert(esp_hosted_transport_append_empty_message(empty_message, 1,
         &offset, 9) == -EMSGSIZE);
  assert(offset == 1);
  g_transport.initialized = true;
  g_transport.data_path_open = true;
  g_transport.rx_active = true;
  expected_response = 569;
  assert(esp_hosted_transport_set_wifi_storage_ram(&g_transport, &result) == 0);
  assert(result == 0); dump();
  expected_response = 516;
  assert(esp_hosted_transport_set_wifi_mode(&g_transport, 1, &result) == 0);
  dump();
  expected_response = 569;
  remote_result = 42;
  assert(esp_hosted_transport_set_wifi_storage_ram(&g_transport, &result) == 0);
  assert(result == 42);
  wait_result = -ETIMEDOUT;
  assert(esp_hosted_transport_set_wifi_storage_ram(&g_transport, &result)
         == -ETIMEDOUT);
  assert(!g_transport.rpc_pending && !g_transport.rpc_lock);
  send_result = -EIO;
  assert(esp_hosted_transport_set_wifi_storage_ram(&g_transport, &result) == -EIO);
  assert(!g_transport.rpc_pending && !g_transport.rpc_lock);
  send_result = 0;
  assert(esp_hosted_transport_send_sta_config(&g_transport,
         "test-network", "dummy-pass", 6) == 0); dump();
  assert(esp_hosted_transport_send_sta_config(&g_transport, "open", "", 7) == 0);
  dump();
  char ssid[34], password[66];
  memset(ssid, 's', 32); ssid[32] = 0;
  memset(password, 'p', 64); password[64] = 0;
  assert(esp_hosted_transport_send_sta_config(&g_transport, ssid, password, 8)
         == 0); dump();
  ssid[32] = 's'; ssid[33] = 0;
  assert(esp_hosted_transport_send_sta_config(&g_transport, ssid, password, 8)
         == -EINVAL);
  ssid[32] = 0; password[64] = 'p'; password[65] = 0;
  assert(esp_hosted_transport_send_sta_config(&g_transport, ssid, password, 8)
         == -EINVAL);
  g_transport.wlan_rx = callback;
  g_transport.wlan_rx_arg = &result;
  g_transport.rx_active = false;
  assert(esp_hosted_transport_register_wlan_rx(&g_transport, NULL, NULL) == 0);
  assert(!g_transport.wlan_rx && !g_transport.wlan_rx_arg);
  assert(esp_hosted_transport_register_wlan_rx(&g_transport, callback, NULL)
         == -EPIPE);
  {
    const uint8_t connected[] = {0x10, 0x00};
    const uint8_t disconnected[] = {0x10, 0x00};
    const uint8_t failed[] = {0x08, 0x01};
    const uint8_t malformed[] = {0x0a, 0x00};
    assert(esp_hosted_transport_handle_sta_link_event(connected,
           sizeof(connected), true) == 0);
    assert(link_up && link_changes == 1);
    assert(esp_hosted_transport_handle_sta_link_event(disconnected,
           sizeof(disconnected), false) == 0);
    assert(!link_up && link_changes == 2);
    assert(esp_hosted_transport_handle_sta_link_event(failed,
           sizeof(failed), true) == 0);
    assert(!link_up && link_changes == 2);
    assert(esp_hosted_transport_handle_sta_link_event(malformed,
           sizeof(malformed), true) == -EPROTO);
  }
  g_transport.initialized = false;
  assert(esp_hosted_transport_register_wlan_rx(&g_transport, NULL, NULL)
         == -EPIPE);
  return 0;
}
'''


def fields(data):
    offset = 0

    def varint():
        nonlocal offset
        value = shift = 0
        while True:
            byte = data[offset]
            offset += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7
            assert shift < 70

    result = {}
    while offset < len(data):
        key = varint()
        if key & 7 == 0:
            value = varint()
        else:
            assert key & 7 == 2
            length = varint()
            assert offset + length <= len(data)
            value = data[offset:offset + length]
            offset += length
        assert key >> 3 not in result
        result[key >> 3] = value
    return result


def rpc(hex_packet):
    packet = bytes.fromhex(hex_packet)
    assert packet[0] == 3
    assert int.from_bytes(packet[4:6], "little") == 12
    assert int.from_bytes(packet[2:4], "little") == len(packet) - 12
    assert packet[12:21] == b"\x01\x06\x00RPCRsp"
    assert packet[21] == 2
    assert int.from_bytes(packet[22:24], "little") == len(packet) - 24
    return fields(packet[24:])


def check_legacy_decoder(sta_messages, decoder_dir):
    """Optionally check official generated code with the local protobuf-c runtime."""
    runtime = ROOT.parent / "external/protobuf-c/protobuf-c"
    decoder = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_hosted_rpc.pb-c.h"
int main(int argc, char **argv) {
  for (int arg = 1; arg < argc; arg++) {
    uint8_t bytes[128];
    size_t size = strlen(argv[arg]) / 2;
    assert(size >= 4 && size <= sizeof(bytes));
    for (size_t i = 0; i < size; i++) {
      unsigned int value;
      assert(sscanf(argv[arg] + 2 * i, "%2x", &value) == 1);
      bytes[i] = value;
    }
    assert(memcmp(bytes + size - 4, "\x4a\0\x52\0", 4) == 0);
    WifiStaConfig *sta = wifi_sta_config__unpack(NULL, size, bytes);
    assert(sta && sta->threshold && sta->pmf_cfg);
    assert(sta->threshold->rssi == 0 && sta->threshold->authmode == 0);
    assert(!sta->pmf_cfg->capable && !sta->pmf_cfg->required);
    wifi_sta_config__free_unpacked(sta, NULL);
    /* Negative control: the old packet omitted both final messages. */
    sta = wifi_sta_config__unpack(NULL, size - 4, bytes);
    assert(sta && !sta->threshold && !sta->pmf_cfg);
    wifi_sta_config__free_unpacked(sta, NULL);
  }
  return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="hosted-decoder-test-") as directory:
        path = Path(directory)
        (path / "decode.c").write_text(decoder)
        subprocess.run([
            "cc", "-std=c11", "-fsanitize=undefined", "-I", str(runtime),
            "-I", str(decoder_dir), str(path / "decode.c"),
            str(decoder_dir / "esp_hosted_rpc.pb-c.c"),
            str(runtime / "protobuf-c/protobuf-c.c"),
            "-o", str(path / "decode"),
        ], check=True)
        subprocess.run([str(path / "decode"),
                        *(message.hex() for message in sta_messages)], check=True)
    print("PASS: official protobuf-c decoder yields non-NULL default objects; "
          "omitting the messages reproduces NULL pointers")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--decoder-dir", type=Path,
                        help="Optional directory containing official "
                             "esp_hosted_rpc.pb-c.c and .h for cross-checking")
    args = parser.parse_args()
    source = SOURCE.read_text()
    names = [
        "put_le16", "checksum", "append_varint", "append_field", "append_bytes",
        "append_empty_message",
        "send_scalar_request", "send_sta_config", "scalar_rpc",
        "set_wifi_mode", "set_wifi_storage_ram", "register_wlan_rx",
        "get_varint", "skip_field", "handle_sta_link_event",
    ]
    constants = "\n".join(re.findall(
        r"^#define ESP_HOSTED_TRANSPORT_\w+[^\n]*", source, re.M))
    functions = "\n".join(function(source, "esp_hosted_transport_" + name)
                          for name in names)
    with tempfile.TemporaryDirectory(prefix="hosted-rpc-test-") as directory:
        path = Path(directory)
        (path / "test.c").write_text(FIXTURE + constants + "\n" + functions + MAIN)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-o", str(path / "test"),
                        str(path / "test.c")], check=True)
        packets = subprocess.check_output([str(path / "test")], text=True).splitlines()
    assert len(packets) == 5
    assert len(bytes.fromhex(packets[2])) == 68  # 12-byte SSID, 10-byte password
    storage, mode, normal, empty, maximum = map(rpc, packets)
    assert storage == {1: 1, 2: 313, 3: 1, 313: b"\x08\x01"}
    assert mode == {1: 1, 2: 260, 3: 2, 260: b"\x08\x01"}
    sta_messages = []
    for message, ssid, password in [
        (normal, b"test-network", b"dummy-pass"),
        (empty, b"open", b""), (maximum, b"s" * 32, b"p" * 64),
    ]:
        assert message[1] == 1 and message[2] == 284
        request = fields(message[284])
        assert request.get(1, 0) == 0  # WIFI_IF_STA
        sta_message = fields(request[2])[2]
        sta_messages.append(sta_message)
        sta = fields(sta_message)
        assert sta[1] == ssid and sta.get(2, b"") == password
        # An absent submessage decodes to NULL in protobuf-c.  Legacy C6
        # handlers dereference both pointers without testing for NULL.
        # Empty, present messages instead allocate default-valued structs.
        assert sta.get(9) == b"", "STA threshold submessage is missing"
        assert sta.get(10) == b"", "STA pmf_cfg submessage is missing"
        assert fields(sta[9]) == {} and fields(sta[10]) == {}
    assert len(sta_messages[-1]) == 104
    if args.decoder_dir:
        check_legacy_decoder(sta_messages, args.decoder_dir)
    print("PASS: storage/mode wire format, RPC errors/timeouts, STA credential "
          "boundaries, required nested messages and encoder bounds, "
          "callback removal after RX fault, STA carrier event handling")


if __name__ == "__main__":
    main()
