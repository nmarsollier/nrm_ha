/* USB CDC — usb_cdc_tx.c — frame transmission to the host. */
#include "usb_cdc.h"
#include "usb_cdc_internal.h"

#include "tinyusb_cdc_acm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Give up queueing a frame if the host stops draining the CDC ring for this
 * long.  A command response is small and sent rarely, so this only trips when
 * the host has gone away. */
#define USB_CDC_TX_TIMEOUT_MS 1000

bool usb_cdc_send(const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) {
        return false;
    }

    if (usb_cdc_tx_mutex == NULL) {
        return false;
    }

    if (xSemaphoreTake(usb_cdc_tx_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }

    /* Queue the whole frame, advancing past whatever the CDC TX ring accepts.
     * When the ring is full, yield so the TinyUSB task drains it to the host.
     * A partially-queued frame must never be left behind: the next frame would
     * be read as the tail of this one and desynchronise the length framing. */
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(USB_CDC_TX_TIMEOUT_MS);
    size_t off = 0;
    bool ok = true;
    while (off < len) {
        size_t queued = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data + off, len - off);
        off += queued;
        if (off == len) {
            break;
        }
        if (queued == 0 && xTaskGetTickCount() >= deadline) {
            ok = false;
            break;
        }
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);

    xSemaphoreGive(usb_cdc_tx_mutex);
    return ok;
}
