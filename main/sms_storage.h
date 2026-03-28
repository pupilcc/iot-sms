#ifndef SMS_STORAGE_H
#define SMS_STORAGE_H

#include "esp_err.h"
#include "uart_at_manager.h" // For sms_message_t

/**
 * @brief Initialize the SMS storage system (NVS).
 *
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t sms_storage_init(void);

/**
 * @brief Save a failed SMS message to NVS for later retry.
 *
 * @param sms Pointer to the SMS message to save.
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t sms_storage_save(const sms_message_t *sms);

/**
 * @brief Retrieve the next failed SMS message from NVS.
 *
 * @param sms Pointer to buffer where the SMS message will be stored.
 * @return ESP_OK if a message was retrieved, ESP_ERR_NOT_FOUND if no messages exist, ESP_FAIL on error.
 */
esp_err_t sms_storage_get_next(sms_message_t *sms);

/**
 * @brief Delete the oldest failed SMS message from NVS after successful send.
 *
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t sms_storage_delete_oldest(void);

/**
 * @brief Get the count of failed SMS messages in NVS.
 *
 * @return Number of stored messages, or -1 on error.
 */
int sms_storage_get_count(void);

/**
 * @brief Clear all stored SMS messages from NVS.
 *
 * @return ESP_OK on success, ESP_FAIL otherwise.
 */
esp_err_t sms_storage_clear_all(void);

#endif // SMS_STORAGE_H
