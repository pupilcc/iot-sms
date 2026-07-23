#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "esp_netif.h"

// Include our custom modules
#include "wifi_manager.h"
#include "uart_at_manager.h"
#include "uart_dtu_manager.h"
#include "mqtt_manager.h"
#include "sms_processor.h"
#include "sntp_manager.h"
#include "remote_log.h"

static const char *TAG = "app_main";

// Global queue handle for SMS messages
QueueHandle_t g_sms_queue;

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    // Set log levels for debugging (optional)
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("wifi_manager", ESP_LOG_INFO);
    esp_log_level_set("uart_at_manager", ESP_LOG_INFO); // Set to VERBOSE for detailed AT command logs
    esp_log_level_set("uart_dtu_manager", ESP_LOG_DEBUG);
    esp_log_level_set("mqtt_manager", ESP_LOG_INFO);
    esp_log_level_set("sms_processor", ESP_LOG_INFO);
    esp_log_level_set("remote_log", ESP_LOG_INFO);

    // Install remote log hook early so boot logs are buffered and
    // flushed to MQTT once the connection is up
    if (remote_log_early_init() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to init remote log, continuing without it.");
    }

    // Initialize NVS (Non-Volatile Storage) for Wi-Fi credentials
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize TCP/IP stack and default event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 1. Initialize and connect Wi-Fi
    ESP_LOGI(TAG, "Initializing Wi-Fi...");
    if (wifi_manager_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi. Aborting.");
        // Consider a retry mechanism or deep sleep here
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "Wi-Fi connected successfully.");

    // 2. Create SMS message queue
    g_sms_queue = xQueueCreate(10, sizeof(sms_message_t)); // Queue can hold 10 SMS messages
    if (g_sms_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create SMS queue. Aborting.");
        while(1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    // 3. Initialize UART manager (AT or DTU firmware) and create its task
#if CONFIG_APP_MODEM_FIRMWARE_DTU
    ESP_LOGI(TAG, "Initializing UART DTU manager...");
    ESP_ERROR_CHECK(uart_dtu_init(g_sms_queue));
    xTaskCreate(uart_dtu_task, "uart_dtu_task", 8192, NULL, 6, NULL);
#else
    ESP_LOGI(TAG, "Initializing UART AT manager...");
    ESP_ERROR_CHECK(uart_at_init(g_sms_queue));
    xTaskCreate(uart_at_task, "uart_at_task", 8192, NULL, 6, NULL); // 8KB stack for SMS fragment processing (sms_message_t is 2KB)
#endif

    // 4. Start MQTT client
    ESP_LOGI(TAG, "Starting MQTT client...");
    mqtt_manager_start();

    // Start log forwarder / metrics task (prio 3, below sms_processor)
    if (remote_log_start(g_sms_queue) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start remote log task, continuing without it.");
    }

    // 5. Create SMS processor task
    ESP_LOGI(TAG, "Creating SMS processor task...");
    xTaskCreate(sms_processor_task, "sms_processor_task", 10240, (void*)g_sms_queue, 4, NULL); // 10KB stack (sms_message_t=2KB + mqtt_payload=2.5KB + network stack)

    ESP_LOGI(TAG, "All critical components initialized.");

    // 6. Initialize SNTP and synchronize time (non-critical, done last)
    // Give network stack a moment to fully stabilize after Wi-Fi connection
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Synchronizing time via SNTP...");
    if (sntp_manager_init() != ESP_OK) {
        ESP_LOGW(TAG, "SNTP time synchronization failed. Continuing with system time.");
    }

    ESP_LOGI(TAG, "Application setup complete. Waiting for SMS...");
}
