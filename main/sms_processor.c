#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "sms_processor.h"
#include "uart_at_manager.h" // For sms_message_t
#include "mqtt_manager.h"     // For mqtt_manager_publish_sms
#include "sms_storage.h"      // For NVS persistence

static const char *TAG = "sms_processor";

// Retry state for non-blocking retry mechanism
typedef struct {
    sms_message_t sms;
    int retry_count;
    TickType_t next_retry_time;
    bool is_active;
} sms_retry_state_t;

static sms_retry_state_t s_retry_state = {0};

void sms_processor_task(void *pvParameters) {
    QueueHandle_t sms_queue = (QueueHandle_t)pvParameters;
    if (sms_queue == NULL) {
        ESP_LOGE(TAG, "SMS queue is NULL, cannot start processor task.");
        vTaskDelete(NULL);
    }

    // Initialize SMS storage
    sms_storage_init();

    // Check for stored SMS from previous session and recover them
    int stored_count = sms_storage_get_count();
    if (stored_count > 0) {
        ESP_LOGI(TAG, "Found %d stored SMS from previous session, will retry sending", stored_count);
        // Recovered SMS will be processed after MQTT connects
    }

    sms_message_t received_sms;
    const int max_retry_attempts = 3;      // Maximum retry attempts per SMS (reduced for non-blocking)
    const int retry_delay_ms = 10000;      // Wait 10 seconds between retries
    const TickType_t queue_timeout = pdMS_TO_TICKS(1000); // Check queue every 1 second

    while (1) {
        // First, try to send any stored SMS from NVS if MQTT is connected
        if (mqtt_manager_is_connected()) {
            sms_message_t stored_sms;
            while (sms_storage_get_next(&stored_sms) == ESP_OK) {
                ESP_LOGI(TAG, "Retrying stored SMS from NVS: Sender='%s'", stored_sms.sender);
                if (mqtt_manager_publish_sms(&stored_sms) == ESP_OK) {
                    ESP_LOGI(TAG, "Successfully sent stored SMS, removing from NVS");
                    sms_storage_delete_oldest();
                } else {
                    ESP_LOGW(TAG, "Failed to send stored SMS, will retry later");
                    break; // Stop trying stored SMS for now, try again next iteration
                }
            }
        }

        // Check if there's a retry in progress
        if (s_retry_state.is_active) {
            TickType_t current_time = xTaskGetTickCount();
            if (current_time >= s_retry_state.next_retry_time) {
                // Time to retry
                if (mqtt_manager_is_connected()) {
                    if (mqtt_manager_publish_sms(&s_retry_state.sms) == ESP_OK) {
                        ESP_LOGI(TAG, "Retry successful for SMS from '%s'", s_retry_state.sms.sender);
                        s_retry_state.is_active = false;
                    } else {
                        s_retry_state.retry_count++;
                        if (s_retry_state.retry_count >= max_retry_attempts) {
                            ESP_LOGE(TAG, "Failed to publish SMS after %d attempts, saving to NVS",
                                     max_retry_attempts);
                            if (sms_storage_save(&s_retry_state.sms) != ESP_OK) {
                                ESP_LOGE(TAG, "Failed to save SMS to NVS, message from '%s' is lost",
                                         s_retry_state.sms.sender);
                            }
                            s_retry_state.is_active = false;
                        } else {
                            // Schedule next retry
                            s_retry_state.next_retry_time = current_time + pdMS_TO_TICKS(retry_delay_ms);
                            ESP_LOGW(TAG, "Retry %d/%d failed, will retry in %d seconds",
                                     s_retry_state.retry_count, max_retry_attempts, retry_delay_ms / 1000);
                        }
                    }
                } else {
                    s_retry_state.retry_count++;
                    if (s_retry_state.retry_count >= max_retry_attempts) {
                        ESP_LOGE(TAG, "MQTT disconnected after %d attempts, saving SMS to NVS",
                                 max_retry_attempts);
                        if (sms_storage_save(&s_retry_state.sms) != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to save SMS to NVS, message from '%s' is lost",
                                     s_retry_state.sms.sender);
                        }
                        s_retry_state.is_active = false;
                    } else {
                        // Schedule next retry
                        s_retry_state.next_retry_time = current_time + pdMS_TO_TICKS(retry_delay_ms);
                        ESP_LOGW(TAG, "MQTT not connected (attempt %d/%d), will retry in %d seconds",
                                 s_retry_state.retry_count, max_retry_attempts, retry_delay_ms / 1000);
                    }
                }
            }
        }

        // Use timeout-based receive to allow processing retries and stored SMS
        if (xQueueReceive(sms_queue, &received_sms, queue_timeout) == pdPASS) {
            ESP_LOGI(TAG, "SMS Processor received new SMS: Sender='%s', Content='%s'",
                     received_sms.sender, received_sms.content);

            // If there's already a retry in progress, save this new SMS to NVS
            if (s_retry_state.is_active) {
                ESP_LOGW(TAG, "Retry in progress, saving new SMS to NVS for later processing");
                if (sms_storage_save(&received_sms) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to save new SMS to NVS, message from '%s' is lost",
                             received_sms.sender);
                }
                continue;
            }

            // Try to publish immediately
            if (mqtt_manager_is_connected()) {
                if (mqtt_manager_publish_sms(&received_sms) == ESP_OK) {
                    ESP_LOGI(TAG, "SMS published successfully");
                } else {
                    // Start retry mechanism
                    ESP_LOGW(TAG, "Failed to publish SMS, starting retry mechanism");
                    memcpy(&s_retry_state.sms, &received_sms, sizeof(sms_message_t));
                    s_retry_state.retry_count = 1;
                    s_retry_state.next_retry_time = xTaskGetTickCount() + pdMS_TO_TICKS(retry_delay_ms);
                    s_retry_state.is_active = true;
                }
            } else {
                // MQTT not connected, start retry mechanism
                ESP_LOGW(TAG, "MQTT not connected, starting retry mechanism");
                memcpy(&s_retry_state.sms, &received_sms, sizeof(sms_message_t));
                s_retry_state.retry_count = 1;
                s_retry_state.next_retry_time = xTaskGetTickCount() + pdMS_TO_TICKS(retry_delay_ms);
                s_retry_state.is_active = true;
            }
        }
    }
    vTaskDelete(NULL);
}
