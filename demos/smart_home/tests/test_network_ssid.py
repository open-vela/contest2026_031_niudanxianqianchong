#!/usr/bin/env python3
"""Host checks of production SSID tracking; Wi-Fi/DHCP/DNS are test doubles."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/net/smart_home_network.c"


def production_function(name):
    source = SOURCE.read_text()
    match = re.search(r"^(?:int|void) " + name + r"\([^;]*?^\{.*?^\}",
                      source, re.M | re.S)
    assert match, name
    return match[0]


FIXTURE = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#define FAR
#include "smart_home_network.h"
#define CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED 1
#define CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED_STA_SSID "board-network"
#define SMART_HOME_WIFI_IFNAME "wlan0"
#define SMART_HOME_NETWORK_ASSOC_TIMEOUT_MS 15000
#define AGENT_OK 0
static int connect_result, save_result, dns_result;
static bool has_ip = true, link_up = true;
static unsigned saves;
static bool smart_home_network_has_ip(const char *name)
{ (void)name; return has_ip; }
static bool smart_home_network_link_up(const char *name)
{ (void)name; return link_up; }
static int smart_home_network_verify_dns(void) { return dns_result; }
static int smart_home_network_wait_link(const char *name, int ms)
{ (void)name; assert(ms == 15000); return link_up ? 0 : -ETIMEDOUT; }
static int board_esp_hosted_wifi_connect(const char *ssid, const char *password)
{ (void)ssid; (void)password; return connect_result; }
static int smart_home_secrets_set_wifi_credentials(const char *s, const char *p)
{ assert(s && p); saves++; return save_result; }
static int netlib_ifup(const char *name) { (void)name; return 0; }
static void smart_home_network_log_link_state(const char *a, const char *b,
                                             const smart_home_network_status_t *s, int r)
{ (void)a; (void)b; (void)s; (void)r; }
int smart_home_network_init(smart_home_network_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->backend = SMART_HOME_NETWORK_BACKEND_WIFI;
    status->ifname = "wlan0";
    return smart_home_network_probe(status);
}
'''

MAIN = r'''
int main(int argc, char **argv)
{
    smart_home_network_status_t status;
    assert(argc == 2);
    unsigned mode = (unsigned)atoi(argv[1]);
    smart_home_network_status_init(&status);
    if (mode == 0) {
        assert(!smart_home_network_connect_credentials(&status, "家庭 Wi-Fi", "test"));
        assert(!strcmp(status.ssid, "家庭 Wi-Fi") && saves == 1 && status.online);
        dns_result = -23;
        assert(smart_home_network_probe(&status) == -23);
        assert(!status.online && !strcmp(status.ssid, "家庭 Wi-Fi"));
    } else if (mode == 1) {
        dns_result = -23;
        assert(smart_home_network_connect_credentials(&status, "no-internet", "") == -23);
        assert(!strcmp(status.ssid, "no-internet") && saves == 1);
    } else if (mode == 2) {
        connect_result = -EIO;
        assert(smart_home_network_connect_credentials(&status, "failed", "") == -EIO);
        assert(!status.ssid[0] && !saves);
        connect_result = 0; has_ip = false;
        assert(smart_home_network_connect_credentials(&status, "no-ip", "") == -22);
        assert(!status.ssid[0] && !saves);
    } else if (mode == 3) {
        assert(!smart_home_network_connect_credentials(&status, "recover", ""));
        link_up = false;
        assert(smart_home_network_probe(&status) == -22 && !status.online);
        assert(status.ip_status != SMART_HOME_NETWORK_OK);
        link_up = true;
        assert(!smart_home_network_probe(&status));
        assert(status.online && !strcmp(status.ssid, "recover"));
    } else if (mode == 4) {
        assert(!smart_home_network_prepare_setup(&status));
        assert(!strcmp(status.ssid, "board-network"));
        has_ip = false;
        assert(!smart_home_network_prepare_setup(&status));
        assert(!status.ssid[0]);
    } else {
        save_result = -EIO;
        assert(smart_home_network_connect_credentials(&status, "unsaved", "") == -EIO);
        assert(!strcmp(status.ssid, "unsaved") && status.online);
    }
    return 0;
}
'''


class NetworkSSID(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="smart-home-network-ssid-")
        cls.addClassCleanup(cls.tmp.cleanup)
        source = Path(cls.tmp.name) / "network.c"
        source.write_text(FIXTURE + "\n" + "\n".join(production_function(name) for name in (
            "smart_home_network_status_init", "smart_home_network_probe",
            "smart_home_network_connect_credentials", "smart_home_network_prepare_setup")) + MAIN)
        cls.binary = source.with_suffix("")
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                        "-I", str(ROOT / "src/net"), str(source), "-o", str(cls.binary)],
                       check=True, capture_output=True)

    def test_connection_and_dns_refresh_preserve_confirmed_name(self):
        self.run_mode(0)

    def test_lan_without_internet_still_has_connected_name(self):
        self.run_mode(1)

    def test_failed_connections_do_not_publish_draft_name(self):
        self.run_mode(2)

    def test_carrier_loss_and_reconnect_with_existing_lease(self):
        self.run_mode(3)

    def test_existing_board_association_and_unconfigured_startup(self):
        self.run_mode(4)

    def test_credential_save_failure_does_not_hide_live_connection(self):
        self.run_mode(5)

    def run_mode(self, mode):
        subprocess.run([str(self.binary), str(mode)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
