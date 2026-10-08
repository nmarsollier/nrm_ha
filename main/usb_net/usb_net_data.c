/* USB Net — usb_net_data.c — NCM data path.
 *
 * usb_net_transmit() is lwIP's TX hook: it copies each frame into the bounded
 * TX queue (usb_net_tx.c) and returns immediately, so the single TCP/IP thread
 * never blocks on the USB send while other connections wait.
 */
#include "usb_net_internal.h"

#include "lwip/netif.h"
#include "lwip/pbuf.h"

esp_err_t usb_net_lwip_input(void *netif_handle, void *buffer, size_t len, void *l2_buff)
{
    struct netif *netif = (struct netif *)netif_handle;
    if (!netif || !buffer) return ESP_ERR_INVALID_ARG;

    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
    if (!p) return ESP_ERR_NO_MEM;
    if (pbuf_take(p, buffer, len) != ERR_OK) { pbuf_free(p); return ESP_FAIL; }
    if (netif->input(p, netif) != ERR_OK) { pbuf_free(p); return ESP_FAIL; }
    return ESP_OK;
}

esp_err_t usb_net_transmit(void *driver_handle, void *buffer, size_t len)
{
    return usb_net_tx_enqueue(buffer, len);
}

void usb_net_free_rx_buffer(void *driver_handle, void *buffer)
{
    /* NCM mode manages RX buffers internally via TinyUSB — nothing
     * to free here.  This callback satisfies the esp_netif driver
     * interface contract. */
}
