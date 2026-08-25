/****************************************************************************
 * smart_home/src/net/smart_home_network_offline.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Offline implementation used by the P4X regular-LVGL bring-up profile.
 * It keeps the UI's network-status contract without linking network stack,
 * Wi-Fi, DHCP, DNS or TLS code into a display/touch validation image.
 ****************************************************************************/

#include <nuttx/config.h>

#include <string.h>

#include "smart_home_network.h"

void smart_home_network_status_init(FAR smart_home_network_status_t *status)
{
  if (status == NULL)
    {
      return;
    }

  memset(status, 0, sizeof(*status));
  status->platform = SMART_HOME_NETWORK_PLATFORM_UNKNOWN;
  status->backend = SMART_HOME_NETWORK_BACKEND_UNKNOWN;
  status->init_status = SMART_HOME_NETWORK_STATUS_NA;
  status->ip_status = SMART_HOME_NETWORK_STATUS_NA;
  status->dns_status = SMART_HOME_NETWORK_STATUS_NA;
}

int smart_home_network_init(FAR smart_home_network_status_t *status)
{
  smart_home_network_status_init(status);
  return SMART_HOME_NETWORK_STATUS_NA;
}

int smart_home_network_probe(FAR smart_home_network_status_t *status)
{
  if (status != NULL)
    {
      status->init_status = SMART_HOME_NETWORK_STATUS_NA;
      status->ip_status = SMART_HOME_NETWORK_STATUS_NA;
      status->dns_status = SMART_HOME_NETWORK_STATUS_NA;
      status->online = false;
    }

  return SMART_HOME_NETWORK_STATUS_NA;
}

const char *smart_home_network_platform_name(
  smart_home_network_platform_t platform)
{
  switch (platform)
    {
      case SMART_HOME_NETWORK_PLATFORM_SIMULATOR:
        return "simulator";

      case SMART_HOME_NETWORK_PLATFORM_DEVICE_WIFI:
        return "wifi";

      case SMART_HOME_NETWORK_PLATFORM_ETHERNET:
        return "ethernet";

      default:
        return "offline";
    }
}
