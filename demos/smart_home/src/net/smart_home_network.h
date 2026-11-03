/****************************************************************************
 * smart_home/src/net/smart_home_network.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef SMART_HOME_NETWORK_H
#define SMART_HOME_NETWORK_H

#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SMART_HOME_NETWORK_OK             0
#define SMART_HOME_NETWORK_STATUS_UNKNOWN 1
#define SMART_HOME_NETWORK_STATUS_NA      2
#define SMART_HOME_NETWORK_ERR_IFUP      -20
#define SMART_HOME_NETWORK_ERR_DHCP      -21
#define SMART_HOME_NETWORK_ERR_NO_IP     -22
#define SMART_HOME_NETWORK_ERR_DNS       -23

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef enum {
  SMART_HOME_NETWORK_PLATFORM_UNKNOWN = 0,
  SMART_HOME_NETWORK_PLATFORM_SIMULATOR,
  SMART_HOME_NETWORK_PLATFORM_DEVICE_WIFI,
  SMART_HOME_NETWORK_PLATFORM_ETHERNET
} smart_home_network_platform_t;

typedef enum {
  SMART_HOME_NETWORK_BACKEND_UNKNOWN = 0,
  SMART_HOME_NETWORK_BACKEND_ETHERNET,
  SMART_HOME_NETWORK_BACKEND_WIFI
} smart_home_network_backend_t;

typedef struct {
  smart_home_network_platform_t platform;
  smart_home_network_backend_t backend;
  const char *ifname;
  int init_status;
  int ip_status;
  int dns_status;
  bool online;
  /* Last confirmed Wi-Fi association; display only while connected. */
  char ssid[33];
} smart_home_network_status_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

void smart_home_network_status_init(FAR smart_home_network_status_t *status);
int smart_home_network_init(FAR smart_home_network_status_t *status);
/* Prepare the Wi-Fi status for first-time UI setup without starting DHCP.
 * This prevents an unconfigured AP from delaying the touchscreen UI. */
int smart_home_network_prepare_setup(FAR smart_home_network_status_t *status);
int smart_home_network_probe(FAR smart_home_network_status_t *status);
int smart_home_network_connect_credentials(
  FAR smart_home_network_status_t *status, FAR const char *ssid,
  FAR const char *password);
const char *smart_home_network_platform_name(
  smart_home_network_platform_t platform);

#endif /* SMART_HOME_NETWORK_H */
