/****************************************************************************
 * chips/esp32p4/common/espressif/esp_hosted_wlan.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_ESPRESSIF_HOSTED_WLAN

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/mm/iob.h>
#include <nuttx/kmalloc.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/spinlock.h>

#include <arch/chip/esp_hosted_transport.h>
#include <arch/chip/esp_hosted_wlan.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESP_HOSTED_WLAN_RX_QUOTA 4
#define ESP_HOSTED_WLAN_TX_QUOTA 1

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct esp_hosted_wlan_s
{
  struct netdev_lowerhalf_s dev;
  FAR struct esp_hosted_transport_s *transport;
  spinlock_t rx_lock;
  netpkt_queue_t rx_queue;
  bool initialized;
  bool ifup;
  bool link_up;
  uint8_t tx_buffer[CONFIG_NET_ETH_PKTSIZE];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int esp_hosted_wlan_ifup(FAR struct netdev_lowerhalf_s *dev);
static int esp_hosted_wlan_ifdown(FAR struct netdev_lowerhalf_s *dev);
static int esp_hosted_wlan_transmit(FAR struct netdev_lowerhalf_s *dev,
                                    FAR netpkt_t *pkt);
static FAR netpkt_t *esp_hosted_wlan_receive(
  FAR struct netdev_lowerhalf_s *dev);
static void esp_hosted_wlan_reclaim(FAR struct netdev_lowerhalf_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp_hosted_wlan_s g_esp_hosted_wlan;

static const struct netdev_ops_s g_esp_hosted_wlan_ops =
{
  .ifup = esp_hosted_wlan_ifup,
  .ifdown = esp_hosted_wlan_ifdown,
  .transmit = esp_hosted_wlan_transmit,
  .receive = esp_hosted_wlan_receive,
  .reclaim = esp_hosted_wlan_reclaim,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void esp_hosted_wlan_free_rx_queue(
  FAR struct esp_hosted_wlan_s *priv)
{
  FAR netpkt_t *pkt;
  irqstate_t flags;

  for (;;)
    {
      flags = spin_lock_irqsave(&priv->rx_lock);
      pkt = netpkt_remove_queue(&priv->rx_queue);
      spin_unlock_irqrestore(&priv->rx_lock, flags);

      if (pkt == NULL)
        {
          break;
        }

      netpkt_free(&priv->dev, pkt, NETPKT_RX);
    }
}

static void esp_hosted_wlan_log_kheap(FAR const char *stage)
{
  struct mallinfo info = kmm_mallinfo();

  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 WLAN kheap: stage=%s total=%d used=%d"
         " free=%d largest=%d\n",
         stage, info.arena, info.uordblks, info.fordblks, info.mxordblk);
}

static int esp_hosted_wlan_ifup(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct esp_hosted_wlan_s *priv =
    (FAR struct esp_hosted_wlan_s *)dev;
  irqstate_t flags;

  esp_hosted_wlan_log_kheap("ifup-before");

  flags = spin_lock_irqsave(&priv->rx_lock);
  priv->ifup = false;
  spin_unlock_irqrestore(&priv->rx_lock, flags);

  /* The receive callback runs from the ESP-Hosted worker.  Stop it from
   * adding packets before releasing the queue.
   */

  esp_hosted_wlan_free_rx_queue(priv);

  flags = spin_lock_irqsave(&priv->rx_lock);
  priv->ifup = true;
  spin_unlock_irqrestore(&priv->rx_lock, flags);
  esp_hosted_wlan_log_kheap("ifup-after");
  return OK;
}

static int esp_hosted_wlan_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct esp_hosted_wlan_s *priv =
    (FAR struct esp_hosted_wlan_s *)dev;
  irqstate_t flags;

  esp_hosted_wlan_log_kheap("ifdown-before");

  flags = spin_lock_irqsave(&priv->rx_lock);
  priv->ifup = false;
  spin_unlock_irqrestore(&priv->rx_lock, flags);

  esp_hosted_wlan_free_rx_queue(priv);
  esp_hosted_wlan_log_kheap("ifdown-after");
  return OK;
}

static int esp_hosted_wlan_transmit(FAR struct netdev_lowerhalf_s *dev,
                                    FAR netpkt_t *pkt)
{
  FAR struct esp_hosted_wlan_s *priv =
    (FAR struct esp_hosted_wlan_s *)dev;
  unsigned int length;
  irqstate_t flags;
  bool link_up;
  int ret;

  flags = spin_lock_irqsave(&priv->rx_lock);
  link_up = priv->ifup && priv->link_up;
  spin_unlock_irqrestore(&priv->rx_lock, flags);
  if (!link_up)
    {
      return -ENETDOWN;
    }

  length = netpkt_getdatalen(dev, pkt);
  if (length == 0 || length > sizeof(priv->tx_buffer))
    {
      return -EMSGSIZE;
    }

  ret = netpkt_copyout(dev, priv->tx_buffer, pkt, length, 0);
  if (ret < 0)
    {
      return ret;
    }

  ret = esp_hosted_transport_send_wlan(priv->transport, priv->tx_buffer,
                                        length);
  if (ret < 0)
    {
      return ret;
    }

  netpkt_free(dev, pkt, NETPKT_TX);
  netdev_lower_txdone(dev);
  return OK;
}

static FAR netpkt_t *esp_hosted_wlan_receive(
  FAR struct netdev_lowerhalf_s *dev)
{
  FAR struct esp_hosted_wlan_s *priv =
    (FAR struct esp_hosted_wlan_s *)dev;
  FAR netpkt_t *pkt;
  irqstate_t flags;

  flags = spin_lock_irqsave(&priv->rx_lock);
  pkt = netpkt_remove_queue(&priv->rx_queue);
  spin_unlock_irqrestore(&priv->rx_lock, flags);
  return pkt;
}

static void esp_hosted_wlan_reclaim(FAR struct netdev_lowerhalf_s *dev)
{
  (void)dev;
}

static int esp_hosted_wlan_rx(FAR void *arg, FAR const uint8_t *data,
                              size_t length)
{
  FAR struct esp_hosted_wlan_s *priv = arg;
  FAR netpkt_t *pkt;
  irqstate_t flags;
  int ret;

  if (length < 14 || length > CONFIG_NET_ETH_PKTSIZE)
    {
      syslog(LOG_WARNING,
             "WARNING: ESP-Hosted C6 RX: invalid station frame bytes=%u\n",
             (unsigned int)length);
      NETDEV_RXDROPPED(&priv->dev.netdev);
      return OK;
    }

  flags = spin_lock_irqsave(&priv->rx_lock);
  if (!priv->ifup || !priv->link_up)
    {
      spin_unlock_irqrestore(&priv->rx_lock, flags);
      NETDEV_RXDROPPED(&priv->dev.netdev);
      return OK;
    }

  spin_unlock_irqrestore(&priv->rx_lock, flags);
  pkt = netpkt_alloc(&priv->dev, NETPKT_RX);
  if (pkt == NULL)
    {
      syslog(LOG_WARNING, "WARNING: ESP-Hosted C6 RX: no packet buffer\n");
      NETDEV_RXDROPPED(&priv->dev.netdev);
      return OK;
    }

  ret = netpkt_copyin(&priv->dev, pkt, data, length, 0);
  if (ret < 0)
    {
      netpkt_free(&priv->dev, pkt, NETPKT_RX);
      NETDEV_RXDROPPED(&priv->dev.netdev);
      return OK;
    }

  flags = spin_lock_irqsave(&priv->rx_lock);
  if (!priv->ifup || !priv->link_up)
    {
      spin_unlock_irqrestore(&priv->rx_lock, flags);
      netpkt_free(&priv->dev, pkt, NETPKT_RX);
      return OK;
    }

  ret = netpkt_tryadd_queue(pkt, &priv->rx_queue);
  spin_unlock_irqrestore(&priv->rx_lock, flags);
  if (ret < 0)
    {
      netpkt_free(&priv->dev, pkt, NETPKT_RX);
      NETDEV_RXDROPPED(&priv->dev.netdev);
      return OK;
    }

  netdev_lower_rxready(&priv->dev);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_hosted_wlan_initialize(FAR struct esp_hosted_transport_s *transport)
{
  FAR struct esp_hosted_wlan_s *priv = &g_esp_hosted_wlan;
  uint8_t mac[6];
  int remote_result;
  int ret;

  if (transport == NULL)
    {
      return -EINVAL;
    }

  if (priv->initialized)
    {
      return -EALREADY;
    }

  ret = esp_hosted_transport_get_sta_mac(transport, mac, &remote_result);
  if (ret < 0)
    {
      return ret;
    }

  if (remote_result != OK)
    {
      return -EIO;
    }

  memset(priv, 0, sizeof(*priv));
  priv->transport = transport;
  priv->dev.ops = &g_esp_hosted_wlan_ops;
  priv->dev.rxtype = NETDEV_RX_WORK;
  priv->dev.quota[NETPKT_RX] = ESP_HOSTED_WLAN_RX_QUOTA;
  priv->dev.quota[NETPKT_TX] = ESP_HOSTED_WLAN_TX_QUOTA;
  memcpy(priv->dev.netdev.d_mac.ether.ether_addr_octet, mac, sizeof(mac));
  spin_lock_init(&priv->rx_lock);

  ret = netdev_lower_register(&priv->dev, NET_LL_IEEE80211);
  if (ret < 0)
    {
      return ret;
    }

  netdev_lower_carrier_off(&priv->dev);
  ret = esp_hosted_transport_register_wlan_rx(transport, esp_hosted_wlan_rx,
                                               priv);
  if (ret < 0)
    {
      netdev_lower_unregister(&priv->dev);
      return ret;
    }

  priv->initialized = true;
  esp_hosted_wlan_log_kheap("registered");
  syslog(LOG_INFO,
         "INFO: ESP-Hosted C6 WLAN registered: wlan0"
         " mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return OK;
}

void esp_hosted_wlan_set_link(bool up)
{
  FAR struct esp_hosted_wlan_s *priv = &g_esp_hosted_wlan;
  irqstate_t flags;

  if (!priv->initialized)
    {
      return;
    }

  flags = spin_lock_irqsave(&priv->rx_lock);
  priv->link_up = up;
  spin_unlock_irqrestore(&priv->rx_lock, flags);

  if (up)
    {
      netdev_lower_carrier_on(&priv->dev);
    }
  else
    {
      netdev_lower_carrier_off(&priv->dev);
      esp_hosted_wlan_free_rx_queue(priv);
    }
}

int esp_hosted_wlan_deinitialize(void)
{
  FAR struct esp_hosted_wlan_s *priv = &g_esp_hosted_wlan;
  int ret;

  if (!priv->initialized)
    {
      return OK;
    }

  ret = esp_hosted_transport_register_wlan_rx(priv->transport, NULL, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = netdev_lower_unregister(&priv->dev);
  if (ret < 0)
    {
      return ret;
    }

  esp_hosted_wlan_free_rx_queue(priv);
  memset(priv, 0, sizeof(*priv));
  return OK;
}

#endif /* CONFIG_ESPRESSIF_HOSTED_WLAN */
