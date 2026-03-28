#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "sdkconfig.h"

#include "sntp_manager.h"

static const char *TAG = "sntp_manager";

// SNTP servers (using multiple servers for redundancy)
#define SNTP_SERVER_1 "pool.ntp.org"
#define SNTP_SERVER_2 "ntp.aliyun.com"
#define SNTP_SERVER_3 "time.cloudflare.com"

// Maximum time to wait for SNTP sync (in seconds)
#define SNTP_SYNC_TIMEOUT_SEC 10

// Timezone from configuration
#define TIMEZONE CONFIG_APP_SNTP_TIMEZONE

// Callback function that is called when time is synchronized
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized from SNTP server");
    time_t now = tv->tv_sec;
    struct tm timeinfo;
    char strftime_buf[64];

    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s", strftime_buf);
}

esp_err_t sntp_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing SNTP for time synchronization...");

    // Stop SNTP if it's already running (important for device restarts)
    if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "SNTP already running, stopping it first...");
        esp_sntp_stop();
    }

    // Set timezone from configuration
    // Examples: "UTC0" for UTC, "CST-8" for China (UTC+8), "EST5EDT,M3.2.0/2,M11.1.0" for US Eastern
    ESP_LOGI(TAG, "Setting timezone to: %s", TIMEZONE);
    setenv("TZ", TIMEZONE, 1);
    tzset();

    // Initialize SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);

    // Set multiple SNTP servers for redundancy
    esp_sntp_setservername(0, SNTP_SERVER_1);
    esp_sntp_setservername(1, SNTP_SERVER_2);
    esp_sntp_setservername(2, SNTP_SERVER_3);

    // Set time sync notification callback
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    // Set sync mode to immediate
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);

    // Initialize SNTP service
    esp_sntp_init();

    ESP_LOGI(TAG, "Waiting for system time to be synchronized...");

    // Wait for time to be set (with timeout)
    // We check if time is set to a reasonable value (after 2020-01-01)
    // This is more reliable than checking sync status
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = SNTP_SYNC_TIMEOUT_SEC * 2; // Check every 500ms
    const time_t MIN_VALID_TIME = 1577836800; // 2020-01-01 00:00:00 UTC

    while (++retry < retry_count) {
        time(&now);
        if (now > MIN_VALID_TIME) {
            // Time has been set to a reasonable value
            break;
        }
        ESP_LOGD(TAG, "Waiting for time sync... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    time(&now);
    if (now > MIN_VALID_TIME) {
        char strftime_buf[64];
        localtime_r(&now, &timeinfo);
        strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

        ESP_LOGI(TAG, "System time synchronized successfully: %s", strftime_buf);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed to synchronize time within timeout period");
        ESP_LOGW(TAG, "MQTT messages will use system uptime instead of real time");
        return ESP_FAIL;
    }
}
