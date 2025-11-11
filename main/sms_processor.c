#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "sms_processor.h"
#include "uart_at_manager.h" // For sms_message_t
#include "mqtt_manager.h"     // For mqtt_manager_publish_sms

static const char *TAG = "sms_processor";

void sms_processor_task(void *pvParameters) {
    QueueHandle_t sms_queue = (QueueHandle_t)pvParameters;
    if (sms_queue == NULL) {
        ESP_LOGE(TAG, "SMS queue is NULL, cannot start processor task.");
        vTaskDelete(NULL);
    }

    sms_message_t received_sms;

    while (1) {
        // Wait for an SMS message to arrive in the queue
        if (xQueueReceive(sms_queue, &received_sms, portMAX_DELAY) == pdPASS) {
            ESP_LOGI(TAG, "SMS Processor received SMS: Sender='%s', Content='%s'",
                     received_sms.sender, received_sms.content);

            // Attempt to publish the SMS via MQTT
            if (mqtt_manager_is_connected()) {
                if (mqtt_manager_publish_sms(&received_sms) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to publish SMS via MQTT.");
                }
            } else {
                ESP_LOGW(TAG, "MQTT not connected, SMS will not be published now. Buffering/retrying logic can be added here.");
                // In a real application, you might want to buffer messages and retry later
                // or store them in NVS. For this example, we just log a warning.
            }
        }
    }
    vTaskDelete(NULL);
}
