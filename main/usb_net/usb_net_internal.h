#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "tinyusb.h"

/* ── IP / DHCP configuration ───────────────────────────────── */

#define USB_NET_IP_OCTET1   192
#define USB_NET_IP_OCTET2   168
#define USB_NET_IP_OCTET3   7
#define USB_NET_IP_OCTET4   1

#define USB_NET_NETMASK_O1  255
#define USB_NET_NETMASK_O2  255
#define USB_NET_NETMASK_O3  255
#define USB_NET_NETMASK_O4  0

#define USB_NET_DHCP_START_O4      2
#define USB_NET_DHCP_END_O4       10
#define USB_NET_DHCP_LEASE_MINUTES 1440
#define USB_NET_MTU               1500

/* ── TX queue / dedicated sender task ────────────────────────
 *
 * usb_net_transmit() runs on lwIP's single TCP/IP thread.  A blocking
 * tinyusb_net_send_sync() there stalls every other connection.  Frames are
 * instead copied into a bounded pool and sent by a dedicated task (see
 * usb_net_tx.c), so lwIP keeps processing while USB is busy.
 *
 * When the pool is momentarily full (a large response such as the ~82 KiB
 * embedded web page bursts through the queue faster than USB Full Speed can
 * drain it) the enqueue *blocks* up to USB_NET_TX_ENQUEUE_TIMEOUT_MS instead
 * of dropping the frame.  A drop forces a TCP retransmission and a ~1 s stall;
 * blocking for the time it takes the sender to free one slot (≈150 µs) is the
 * flow-control that keeps the queue lossless without holding lwIP hostage.
 */
#define USB_NET_TX_POOL_SIZE      16      /* pre-allocated TX slots */
#define USB_NET_TX_BUFFER_SIZE    1536    /* 14 (Ethernet) + 1500 (MTU) + margin */
#define USB_NET_TX_TIMEOUT_MS     100     /* per-frame send timeout */
#define USB_NET_TX_ENQUEUE_TIMEOUT_MS 100 /* max wait for a free TX slot */

void       usb_net_tx_init(void);
esp_err_t  usb_net_tx_enqueue(const void *buffer, size_t len);

/* ── lwIP / driver helpers ─────────────────────────────────── */

esp_err_t  usb_net_lwip_input(void *netif_handle, void *buffer, size_t len, void *l2_buff);
esp_err_t  usb_net_transmit(void *driver_handle, void *buffer, size_t len);
void       usb_net_free_rx_buffer(void *driver_handle, void *buffer);

/* ── TinyUSB NCM callbacks (defined in usb_net_init.c) ─────── */

esp_err_t  usb_net_tinyusb_recv_cb(void *buffer, uint16_t len, void *ctx);
void       usb_net_tinyusb_init_cb(void *ctx);
void       usb_net_tinyusb_free_tx(void *buffer, void *ctx);

/* ── USB descriptors (defined in usb_net_descriptors.c) ────── */

const tusb_desc_device_t *usb_net_device_descriptor(void);
