#ifndef UART_DTU_MANAGER_H
#define UART_DTU_MANAGER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "uart_at_manager.h" // 复用 sms_message_t 和 g_sim_operator

/**
 * @brief Initializes the UART driver for communication with a modem running
 *        Yinerda DTU transparent firmware (config,xxx serial commands).
 *
 * @param sms_queue A FreeRTOS queue to send parsed SMS messages to.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t uart_dtu_init(QueueHandle_t sms_queue);

/**
 * @brief FreeRTOS task to configure the DTU firmware, listen for SMS reports,
 *        poll cached SMS, and send parsed messages to the queue.
 * @param pvParameters Should be NULL.
 */
void uart_dtu_task(void *pvParameters);

#endif // UART_DTU_MANAGER_H
