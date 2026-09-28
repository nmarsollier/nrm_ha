/* Sntp - sntp_start.c
 *
 * Purpose: synchronise the system clock via SNTP over the USB network.
 */
#include "sntp.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SNTP_START";

/*
 * NTP sources, most probable first:
 * - the USB host itself (serves NTP locally without internet), then
 * - public pools (reachable when the host shares its internet connection).
 * The ESP-IDF SNTP client cycles through them if one is unreachable.
 */
#define SNTP_SERVER_1 "192.168.7.2"
#define SNTP_SERVER_2 "pool.ntp.org"
#define SNTP_SERVER_3 "time.google.com"
#define SNTP_SERVER_4 "time.cloudflare.com"

/*
 * The USB host is the only outbound gateway.  It receives 192.168.7.2 from
 * the mount's DHCP server (the pool starts at .2 and there is a single host).
 * Pointing DNS at it lets SNTP resolve the public pools when the host shares
 * internet (ICS); if it does not, the direct-IP server above still works when
 * the host runs a local NTP service.
 */
static void configure_dns(void) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("USB_NET");
    if (netif == NULL) {
        ESP_LOGW(TAG, "USB_NET interface not found — DNS not configured");
        return;
    }
    esp_netif_dns_info_t dns = {
        .ip = ESP_IP4ADDR_INIT(192, 168, 7, 2),
    };
    if (esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set DNS server");
    }
}

static void sntp_task(void *arg) {
    (void) arg;

    /* Let the USB link (enumeration, DHCP) settle before the first query. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    configure_dns();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        4, ESP_SNTP_SERVER_LIST(SNTP_SERVER_1, SNTP_SERVER_2, SNTP_SERVER_3, SNTP_SERVER_4));

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s — time sync unavailable", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_sntp_set_sync_interval(3600000); /* re-sync every hour (ms) */

    /* Poll up to 30 s for the first sync.  The lwIP SNTP thread keeps
     * running afterwards for the periodic re-sync. */
    for (int retry = 0; retry < 300; retry++) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            time_t now;
            time(&now);
            struct tm timeinfo;
            gmtime_r(&now, &timeinfo);
            ESP_LOGI(TAG, "SNTP sync completed — UTC: %04d-%02d-%02dT%02d:%02d:%02dZ",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                     timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    vTaskDelete(NULL);
}

void sntp_start(void) {
    xTaskCreatePinnedToCore(sntp_task, "sntp_sync", 3072, NULL, 1, NULL,
                            0); /* CPU 0 — keep CPU 1 isolated for the motion task */
}
