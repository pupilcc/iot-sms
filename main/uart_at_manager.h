#ifndef UART_AT_MANAGER_H
#define UART_AT_MANAGER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// 定义短信数据结构
typedef struct {
    char sender[32];    // 短信发送者号码
    char content[2048]; // 短信内容 (支持长SMS,最大约4-5段拼接)
} sms_message_t;

// 全局变量，用于存储SIM卡运营商和本机号码
extern char g_sim_operator[32];     // 例如："中国移动", "中国联通"
extern char g_sim_phone_number[20]; // 例如："+8613800000000"

/**
 * @brief Initializes the UART driver for AT communication with 4G Cat.1 modem.
 *
 * @param sms_queue A FreeRTOS queue to send parsed SMS messages to.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t uart_at_init(QueueHandle_t sms_queue);

/**
 * @brief FreeRTOS task to handle AT communication, listen for SMS, and send to queue.
 * @param pvParameters Should be NULL.
 */
void uart_at_task(void *pvParameters);

#endif // UART_AT_MANAGER_H
