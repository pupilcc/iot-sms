#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include "esp_err.h"
#include "uart_at_manager.h" // For sms_message_t

/**
 * @brief Initializes and starts the MQTT client.
 *        This function does not block.
 */
void mqtt_manager_start(void);

/**
 * @brief Publishes an SMS message to the configured MQTT topic.
 *
 * @param sms Pointer to the sms_message_t structure containing sender and content.
 * @return ESP_OK if message was successfully queued for publishing, ESP_FAIL otherwise.
 */
esp_err_t mqtt_manager_publish_sms(const sms_message_t *sms);

/**
 * @brief Checks if the MQTT client is currently connected to the broker.
 * @return true if connected, false otherwise.
 */
bool mqtt_manager_is_connected(void);

/**
 * @brief Publishes a device ready message to the 'esp32/drive' topic.
 *
 * @param operator_name The SIM operator name (e.g., "中国移动", "中国电信").
 * @return ESP_OK if message was successfully queued for publishing, ESP_FAIL otherwise.
 */
esp_err_t mqtt_manager_publish_device_ready(const char *operator_name);

#endif // MQTT_MANAGER_H
