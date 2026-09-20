/****************************************************************************
 * smart_home/src/net/smart_home_network.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <net/if.h>
#include <unistd.h>
#include <errno.h>

#include <arpa/inet.h>
#include <syslog.h>

#include "netutils/netlib.h"

#include <cagent/types.h>

#include "smart_home_network.h"
#include "../config/smart_home_secrets.h"
#ifndef CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED
#include "smart_home_wifi.h"
#else
#include <arch/board/board.h>
#include <arch/chip/esp_hosted_wlan.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SMART_HOME_ETH_IFNAME     "eth0"
#define SMART_HOME_WIFI_IFNAME    "wlan0"
#define SMART_HOME_DNS_HOSTNAME   "api.deepseek.com"

/* ESP-Hosted association timing: WifiConnect returning OK only means the
 * C6 accepted the request.  The association completes asynchronously and
 * flips the wlan0 carrier (IFF_RUNNING), typically within a few seconds. */

#define SMART_HOME_NETWORK_LINK_POLL_MS    200
#define SMART_HOME_NETWORK_ASSOC_TIMEOUT_MS 15000
#define SMART_HOME_NETWORK_LINK_GRACE_MS   2000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool smart_home_network_is_wifi_platform(void)
{
#if defined(CONFIG_ARCH_CHIP_ESP32S3) || defined(CONFIG_ESP32S3_WIFI) || \
    (defined(CONFIG_ARCH_CHIP_ESP32P4) && \
     defined(CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED))
  return true;
#else
  return false;
#endif
}

static bool smart_home_network_is_simulator_platform(void)
{
#if defined(CONFIG_ARCH_CHIP_GOLDFISH_ARM64) || \
    defined(CONFIG_ARCH_CHIP_GOLDFISH_ARM) || \
    defined(CONFIG_ARCH_CHIP_GOLDFISH_X86_64)
  return true;
#else
  return false;
#endif
}

static bool smart_home_network_has_ip(FAR const char *ifname)
{
  struct in_addr addr;

  if (ifname == NULL)
    {
      return false;
    }

  memset(&addr, 0, sizeof(addr));
  netlib_get_ipv4addr(ifname, &addr);
  return addr.s_addr != 0;
}

static bool smart_home_network_link_up(FAR const char *ifname)
{
  struct ifreq ifr;
  int sockfd;
  bool up = false;

  if (ifname == NULL)
    {
      return false;
    }

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    {
      return false;
    }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
  if (ioctl(sockfd, SIOCGIFFLAGS, &ifr) == 0)
    {
      up = (ifr.ifr_flags & IFF_RUNNING) != 0;
    }

  close(sockfd);
  return up;
}

static int smart_home_network_wait_link(FAR const char *ifname,
                                        int timeout_ms)
{
  while (timeout_ms > 0)
    {
      if (smart_home_network_link_up(ifname))
        {
          return OK;
        }

      usleep(SMART_HOME_NETWORK_LINK_POLL_MS * 1000);
      timeout_ms -= SMART_HOME_NETWORK_LINK_POLL_MS;
    }

  return smart_home_network_link_up(ifname) ? OK : -ETIMEDOUT;
}

static void smart_home_network_log_link_state(FAR const char *phase,
                                              FAR const char *ifname,
                                              FAR const smart_home_network_status_t *status,
                                              int result)
{
  struct in_addr ipaddr;
  struct in_addr gateway;
  char iptext[INET_ADDRSTRLEN] = "none";
  char gatewaytext[INET_ADDRSTRLEN] = "none";

  memset(&ipaddr, 0, sizeof(ipaddr));
  memset(&gateway, 0, sizeof(gateway));
  if (ifname != NULL && netlib_get_ipv4addr(ifname, &ipaddr) == 0 &&
      ipaddr.s_addr != 0)
    {
      (void)inet_ntop(AF_INET, &ipaddr, iptext, sizeof(iptext));
    }

  if (ifname != NULL && netlib_get_dripv4addr(ifname, &gateway) == 0 &&
      gateway.s_addr != 0)
    {
      (void)inet_ntop(AF_INET, &gateway, gatewaytext, sizeof(gatewaytext));
    }

  syslog(LOG_INFO,
         "Network %s: if=%s ip=%s gateway=%s result=%d init=%d ip_status=%d "
         "dns=%d online=%d\n",
         phase, ifname ? ifname : "none", iptext, gatewaytext, result,
         status ? status->init_status : SMART_HOME_NETWORK_STATUS_UNKNOWN,
         status ? status->ip_status : SMART_HOME_NETWORK_STATUS_UNKNOWN,
         status ? status->dns_status : SMART_HOME_NETWORK_STATUS_UNKNOWN,
         status && status->online ? 1 : 0);
  fprintf(stderr,
          "[smart_home_net] %s if=%s ip=%s gateway=%s result=%d init=%d "
          "ip_status=%d dns=%d online=%d\n",
          phase, ifname ? ifname : "none", iptext, gatewaytext, result,
          status ? status->init_status : SMART_HOME_NETWORK_STATUS_UNKNOWN,
          status ? status->ip_status : SMART_HOME_NETWORK_STATUS_UNKNOWN,
          status ? status->dns_status : SMART_HOME_NETWORK_STATUS_UNKNOWN,
          status && status->online ? 1 : 0);
}

static int smart_home_network_verify_dns(void)
{
  struct addrinfo hints;
  struct addrinfo *result = NULL;
  int ret;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  ret = getaddrinfo(SMART_HOME_DNS_HOSTNAME, NULL, &hints, &result);
  if (ret != 0)
    {
      syslog(LOG_WARNING, "Network: DNS verify failed for %s: %d\n",
             SMART_HOME_DNS_HOSTNAME, ret);
      return SMART_HOME_NETWORK_ERR_DNS;
    }

  freeaddrinfo(result);
  syslog(LOG_INFO, "Network: DNS verify OK\n");
  return SMART_HOME_NETWORK_OK;
}

static int smart_home_network_init_ethernet(
  FAR smart_home_network_status_t *status)
{
  int ret;

  ret = netlib_ifup(status->ifname);
  status->init_status = ret < 0 ? SMART_HOME_NETWORK_ERR_IFUP :
                                  SMART_HOME_NETWORK_OK;
  if (ret < 0)
    {
      syslog(LOG_WARNING, "Network: ifup %s failed: %d\n",
             status->ifname, ret);
    }

  if (!smart_home_network_has_ip(status->ifname))
    {
      ret = netlib_obtain_ipv4addr(status->ifname);
      status->ip_status = ret < 0 ? SMART_HOME_NETWORK_ERR_DHCP :
                                    SMART_HOME_NETWORK_OK;
      if (ret < 0)
        {
          syslog(LOG_WARNING, "Network: DHCP on %s failed: %d\n",
                 status->ifname, ret);
        }
    }
  else
    {
      status->ip_status = SMART_HOME_NETWORK_OK;
    }

  return smart_home_network_probe(status);
}

static int smart_home_network_init_wifi(FAR smart_home_network_status_t *status)
{
  int ret;

  /* The P4 board initializes and associates its C6 companion through
   * ESP-Hosted RPC during board bring-up.  It does not expose the WAPI
   * wireless-extension control interface used by the ESP32-S3 path below.
   * Reuse that association here and only bring up DHCP when it has not yet
   * acquired an address.
   */

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED
    {
      ret = netlib_ifup(status->ifname);
      status->init_status = ret < 0 ? SMART_HOME_NETWORK_ERR_IFUP :
                                      SMART_HOME_NETWORK_OK;
      if (ret < 0)
        {
          syslog(LOG_WARNING, "Network: ifup %s failed: %d\n",
                 status->ifname, ret);
        }

      /* A DHCP request issued while the carrier is still down fails
       * immediately, so give an in-flight association a short grace
       * window before requesting a lease. */
      if (!smart_home_network_has_ip(status->ifname) &&
          !smart_home_network_link_up(status->ifname))
        {
          smart_home_network_wait_link(status->ifname,
                                       SMART_HOME_NETWORK_LINK_GRACE_MS);
        }

      if (!smart_home_network_has_ip(status->ifname))
        {
          esp_hosted_wlan_dhcp_diagnostics_begin();
          ret = netlib_obtain_ipv4addr(status->ifname);
          esp_hosted_wlan_dhcp_diagnostics_log(ret < 0 ? "failed" :
                                                "complete");
          status->ip_status = ret < 0 ? SMART_HOME_NETWORK_ERR_DHCP :
                                        SMART_HOME_NETWORK_OK;
          if (ret < 0)
            {
              syslog(LOG_WARNING, "Network: Hosted C6 DHCP on %s failed: %d\n",
                     status->ifname, ret);
            }
        }
      else
        {
          status->ip_status = SMART_HOME_NETWORK_OK;
        }

      return smart_home_network_probe(status);
    }
#else
  /*
   * A smart_home restart must not disrupt an already usable Wi-Fi session.
   * In particular, reassociating and restarting DHCP can briefly invalidate
   * sockets owned by the UI, model, or MCP workers.  Reuse the current
   * address when DNS proves that it is usable; reconnect only as recovery.
   */

  if (smart_home_wifi_is_connected())
    {
      syslog(LOG_INFO,
             "Network: WiFi already has an IP; probing existing connection\n");

      status->init_status = SMART_HOME_NETWORK_OK;
      ret = smart_home_network_probe(status);
      if (ret == SMART_HOME_NETWORK_OK)
        {
          syslog(LOG_INFO, "Network: reusing existing WiFi connection\n");
          return ret;
        }

      syslog(LOG_WARNING,
             "Network: existing WiFi probe failed: %d; reconnecting\n", ret);
    }
  else
    {
      syslog(LOG_INFO, "Network: WiFi has no IP; connecting\n");
    }

  status->init_status = smart_home_wifi_connect();
  if (status->init_status < 0)
    {
      syslog(LOG_WARNING, "Network: WiFi auto-connect failed: %d\n",
             status->init_status);
    }

  return smart_home_network_probe(status);
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void smart_home_network_status_init(FAR smart_home_network_status_t *status)
{
  if (status == NULL)
    {
      return;
    }

  memset(status, 0, sizeof(*status));
  status->platform = SMART_HOME_NETWORK_PLATFORM_UNKNOWN;
  status->backend = SMART_HOME_NETWORK_BACKEND_UNKNOWN;
  status->ifname = NULL;
  status->init_status = SMART_HOME_NETWORK_STATUS_UNKNOWN;
  status->ip_status = SMART_HOME_NETWORK_STATUS_UNKNOWN;
  status->dns_status = SMART_HOME_NETWORK_STATUS_UNKNOWN;
  status->online = false;
}

int smart_home_network_init(FAR smart_home_network_status_t *status)
{
  int ret;

  if (status == NULL)
    {
      return SMART_HOME_NETWORK_ERR_NO_IP;
    }

  smart_home_network_status_init(status);

  if (smart_home_network_is_wifi_platform())
    {
      status->platform = SMART_HOME_NETWORK_PLATFORM_DEVICE_WIFI;
      status->backend = SMART_HOME_NETWORK_BACKEND_WIFI;
      status->ifname = SMART_HOME_WIFI_IFNAME;
      smart_home_network_log_link_state("init-begin", status->ifname,
                                        status, SMART_HOME_NETWORK_OK);
      ret = smart_home_network_init_wifi(status);
      smart_home_network_log_link_state("init-end", status->ifname,
                                        status, ret);
      return ret;
    }

  if (smart_home_network_is_simulator_platform())
    {
      status->platform = SMART_HOME_NETWORK_PLATFORM_SIMULATOR;
    }
  else
    {
      status->platform = SMART_HOME_NETWORK_PLATFORM_ETHERNET;
    }

  status->backend = SMART_HOME_NETWORK_BACKEND_ETHERNET;
  status->ifname = SMART_HOME_ETH_IFNAME;
  smart_home_network_log_link_state("init-begin", status->ifname,
                                    status, SMART_HOME_NETWORK_OK);
  ret = smart_home_network_init_ethernet(status);
  smart_home_network_log_link_state("init-end", status->ifname,
                                    status, ret);
  return ret;
}

int smart_home_network_connect_credentials(
  FAR smart_home_network_status_t *status, FAR const char *ssid,
  FAR const char *password)
{
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED
  int ret;

  if (status == NULL || ssid == NULL || password == NULL || ssid[0] == '\0')
    {
      return SMART_HOME_NETWORK_ERR_IFUP;
    }

  ret = board_esp_hosted_wifi_connect(ssid, password);
  if (ret < 0)
    {
      smart_home_network_status_init(status);
      status->platform = SMART_HOME_NETWORK_PLATFORM_DEVICE_WIFI;
      status->backend = SMART_HOME_NETWORK_BACKEND_WIFI;
      status->ifname = SMART_HOME_WIFI_IFNAME;
      status->init_status = SMART_HOME_NETWORK_ERR_IFUP;
      return ret;
    }

  /* WifiConnect returning OK only means the C6 accepted the request.  Wait
   * for the asynchronous association to flip the wlan0 carrier before
   * smart_home_network_init() starts DHCP; otherwise the lease request
   * races the association and fails instantly. */
  ret = smart_home_network_wait_link(SMART_HOME_WIFI_IFNAME,
                                     SMART_HOME_NETWORK_ASSOC_TIMEOUT_MS);
  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "Network: association on %s did not complete in %d ms\n",
             SMART_HOME_WIFI_IFNAME, SMART_HOME_NETWORK_ASSOC_TIMEOUT_MS);
    }

  ret = smart_home_network_init(status);
  /* A valid DHCP lease is sufficient to retain the user-selected AP even if
   * the internet/DNS probe is currently unavailable. */
  if (status->ip_status == SMART_HOME_NETWORK_OK)
    {
      int save_ret = smart_home_secrets_set_wifi_credentials(ssid, password);
      if (save_ret != AGENT_OK)
        {
          return save_ret;
        }
    }
  return ret;
#else
  (void)status;
  (void)ssid;
  (void)password;
  return SMART_HOME_NETWORK_ERR_IFUP;
#endif
}

int smart_home_network_prepare_setup(FAR smart_home_network_status_t *status)
{
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED
  int ret;

  if (status == NULL)
    {
      return SMART_HOME_NETWORK_ERR_IFUP;
    }

  smart_home_network_status_init(status);
  status->platform = SMART_HOME_NETWORK_PLATFORM_DEVICE_WIFI;
  status->backend = SMART_HOME_NETWORK_BACKEND_WIFI;
  status->ifname = SMART_HOME_WIFI_IFNAME;
  ret = netlib_ifup(status->ifname);
  status->init_status = ret < 0 ? SMART_HOME_NETWORK_ERR_IFUP :
                                  SMART_HOME_NETWORK_OK;
  if (ret < 0)
    {
      smart_home_network_log_link_state("setup-ready", status->ifname,
                                        status, ret);
      return ret;
    }

  /* Board Kconfig credentials may already have supplied an address during
   * early bring-up.  Reuse it, but never wait for DHCP before first-time UI
   * setup when there is no address yet. */
  if (smart_home_network_has_ip(status->ifname))
    {
      ret = smart_home_network_probe(status);
    }
  else
    {
      status->ip_status = SMART_HOME_NETWORK_ERR_NO_IP;
      status->dns_status = SMART_HOME_NETWORK_STATUS_UNKNOWN;
      status->online = false;
      ret = SMART_HOME_NETWORK_OK;
    }
  smart_home_network_log_link_state("setup-ready", status->ifname,
                                    status, ret);
  return ret;
#else
  return smart_home_network_init(status);
#endif
}

int smart_home_network_probe(FAR smart_home_network_status_t *status)
{
  int ret;

  if (status == NULL || status->ifname == NULL)
    {
      return SMART_HOME_NETWORK_ERR_NO_IP;
    }

  if (!smart_home_network_has_ip(status->ifname))
    {
      status->ip_status = SMART_HOME_NETWORK_ERR_NO_IP;
      status->dns_status = SMART_HOME_NETWORK_STATUS_UNKNOWN;
      status->online = false;
      return SMART_HOME_NETWORK_ERR_NO_IP;
    }

  status->ip_status = SMART_HOME_NETWORK_OK;

  ret = smart_home_network_verify_dns();
  status->dns_status = ret;
  status->online = ret == SMART_HOME_NETWORK_OK;
  if (status->online) {
      /* WiFi→DNS 全通：kick 天气 worker 立即拉取天气+时间。 */
      extern smart_home_miloco_t *g_weather_miloco_service;
      extern void smart_home_miloco_kick_weather(smart_home_miloco_t *);
      smart_home_miloco_kick_weather(g_weather_miloco_service);
  }
  return status->online ? SMART_HOME_NETWORK_OK : ret;
}

const char *smart_home_network_platform_name(
  smart_home_network_platform_t platform)
{
  switch (platform)
    {
    case SMART_HOME_NETWORK_PLATFORM_SIMULATOR:
      return "Simulator";
    case SMART_HOME_NETWORK_PLATFORM_DEVICE_WIFI:
#if defined(CONFIG_ARCH_CHIP_ESP32P4) && \
    defined(CONFIG_ESP32P4_FUNCTION_EV_BOARD_ESP_HOSTED)
      return "ESP32-P4 + C6 Wi-Fi";
#else
      return "ESP32-S3 Wi-Fi";
#endif
    case SMART_HOME_NETWORK_PLATFORM_ETHERNET:
      return "Ethernet";
    default:
      return "Unknown";
    }
}
