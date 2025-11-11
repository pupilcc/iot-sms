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
#include "mqtt_manager.h"
#include "sms_processor.h"

static const char *TAG = "app_main";

// Global queue handle for SMS messages
QueueHandle_t g_sms_queue;

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    // Set log levels for debugging (optional)
    esp_log_level_set("*", ESP_LOG_DEBUG);
    esp_log_level_set("wifi_manager", ESP_LOG_INFO);
    esp_log_level_set("uart_at_manager", ESP_LOG_DEBUG); // Set to VERBOSE for detailed AT command logs
    esp_log_level_set("mqtt_manager", ESP_LOG_INFO);
    esp_log_level_set("sms_processor", ESP_LOG_INFO);

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

    // 3. Initialize UART AT manager and create its task
    ESP_LOGI(TAG, "Initializing UART AT manager...");
    ESP_ERROR_CHECK(uart_at_init(g_sms_queue));
    xTaskCreate(uart_at_task, "uart_at_task", 4096, NULL, 6, NULL); // Increased stack size for parsing

    // 4. Start MQTT client
    ESP_LOGI(TAG, "Starting MQTT client...");
    mqtt_manager_start();

    // 5. Create SMS processor task
    ESP_LOGI(TAG, "Creating SMS processor task...");
    xTaskCreate(sms_processor_task, "sms_processor_task", 3072, (void*)g_sms_queue, 4, NULL);

    ESP_LOGI(TAG, "Application setup complete. Waiting for SMS...");
}
