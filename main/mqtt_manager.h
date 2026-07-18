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
 * @brief Publishes an arbitrary payload to the given topic without blocking.
 *        Does not log; safe to call from the remote log forwarding path.
 *
 * @param topic MQTT topic to publish to.
 * @param payload Payload bytes.
 * @param len Payload length, or 0 to use strlen(payload).
 * @param qos QoS level (0, 1 or 2).
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected, ESP_FAIL on publish error.
 */
esp_err_t mqtt_manager_publish(const char *topic, const char *payload, int len, int qos);

/**
 * @brief Publishes a device ready message to the 'esp32/drive' topic.
 *
 * @param operator_name The SIM operator name (e.g., "中国移动", "中国电信").
 * @return ESP_OK if message was successfully queued for publishing, ESP_FAIL otherwise.
 */
esp_err_t mqtt_manager_publish_device_ready(const char *operator_name);

#endif // MQTT_MANAGER_H
