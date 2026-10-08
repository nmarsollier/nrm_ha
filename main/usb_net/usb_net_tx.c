/* USB Net — usb_net_tx.c — bounded TX queue + dedicated sender task.
 *
 * lwIP calls usb_net_transmit() from its single TCP/IP thread.  Calling
 * tinyusb_net_send_sync() there blocks that thread (up to USB_NET_TX_TIMEOUT_MS
 * per frame) while the TinyUSB task runs the deferred send — which stalls every
 * other TCP connection (N.I.N.A., the browser) behind the current frame.
 *
 * Instead the frame is copied out of the lwIP-owned buffer into a pre-allocated
 * slot and queued; a dedicated task drains the queue and performs the blocking
 * send.  lwIP is never held waiting on USB, so the other connections keep
 * flowing while a frame is on its way out.
 */
#include "usb_net_internal.h"

#include <string.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "tinyusb_net.h"

static const char *TAG = "USB_NET_TX";

#define USB_NET_TX_TASK_STACK_WORDS 4096
/* Above both httpd servers (10) so the sender drains the queue promptly even
 * when they are mid-handler; below lwIP (18) and TinyUSB (20) so it can never
 * starve the network stack.  The motion task owns core 1 at priority 23 and is
 * untouched. */
#define USB_NET_TX_TASK_PRIORITY    12
#define USB_NET_TX_TASK_CORE        0

/* One pooled TX slot: the copied frame plus its length. */
typedef struct {
    uint16_t len;
    uint8_t data[USB_NET_TX_BUFFER_SIZE];
} usb_net_tx_buf_t;

static usb_net_tx_buf_t s_pool[USB_NET_TX_POOL_SIZE];
static QueueHandle_t s_free_q;   /* idle slots (usb_net_tx_buf_t *) */
static QueueHandle_t s_work_q;   /* slots ready to send (usb_net_tx_buf_t *) */

/*
 * Copy the lwIP-owned frame into a pooled slot and enqueue it.  Returns
 * ESP_OK once the frame is safely queued; the caller (lwIP) may then free its
 * buffer.  When the pool is exhausted it blocks up to
 * USB_NET_TX_ENQUEUE_TIMEOUT_MS for a slot to free instead of dropping: a drop
 * would force a TCP retransmission and a multi-second stall.  ESP_ERR_NO_MEM is
 * returned only if no slot frees within the timeout.
 */
esp_err_t usb_net_tx_enqueue(const void *buffer, size_t len) {
    if (buffer == NULL || len == 0 || len > USB_NET_TX_BUFFER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_free_q == NULL || s_work_q == NULL) {
        /* TX task never started (init failure) — refuse rather than crash. */
        return ESP_ERR_NO_MEM;
    }

    usb_net_tx_buf_t *tx = NULL;
    if (xQueueReceive(s_free_q, &tx, pdMS_TO_TICKS(USB_NET_TX_ENQUEUE_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(tx->data, buffer, len);
    tx->len = (uint16_t) len;

    if (xQueueSend(s_work_q, &tx, 0) != pdTRUE) {
        /* Same depth as the free queue, so this cannot normally happen; still
         * return the slot rather than leak it. */
        xQueueSend(s_free_q, &tx, 0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Dedicated sender: drain the queue and do the blocking TinyUSB send. */
static void usb_net_tx_task(void *arg) {
    (void) arg;

    while (true) {
        usb_net_tx_buf_t *tx = NULL;
        if (xQueueReceive(s_work_q, &tx, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t r = tinyusb_net_send_sync(tx->data, tx->len, NULL,
                                            pdMS_TO_TICKS(USB_NET_TX_TIMEOUT_MS));
        if (r == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "send timeout (%u bytes)", tx->len);
        } else if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
            /* ESP_ERR_INVALID_STATE = USB not mounted (cable out) — expected. */
            ESP_LOGW(TAG, "send error: %s", esp_err_to_name(r));
        }

        xQueueSend(s_free_q, &tx, 0);
    }
}

void usb_net_tx_init(void) {
    s_free_q = xQueueCreate(USB_NET_TX_POOL_SIZE, sizeof(usb_net_tx_buf_t *));
    s_work_q = xQueueCreate(USB_NET_TX_POOL_SIZE, sizeof(usb_net_tx_buf_t *));
    if (s_free_q == NULL || s_work_q == NULL) {
        ESP_LOGE(TAG, "failed to create TX queues");
        return;
    }

    for (int i = 0; i < USB_NET_TX_POOL_SIZE; i++) {
        usb_net_tx_buf_t *tx = &s_pool[i];
        xQueueSend(s_free_q, &tx, 0);
    }

    if (xTaskCreatePinnedToCore(usb_net_tx_task, "usb_net_tx",
                                USB_NET_TX_TASK_STACK_WORDS, NULL,
                                USB_NET_TX_TASK_PRIORITY, NULL,
                                USB_NET_TX_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "failed to create TX task");
        return;
    }
}
