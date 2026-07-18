#ifndef REMOTE_LOG_H
#define REMOTE_LOG_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief 创建日志环形缓冲并安装 vprintf 钩子。
 *        应在 app_main 设置日志级别后、其他模块初始化前调用，
 *        开机日志会先进入缓冲，待 MQTT 连接后统一发出。
 *        本地串口日志输出不受影响。
 */
esp_err_t remote_log_early_init(void);

/**
 * @brief 启动日志转发任务（同时负责定期设备指标上报）。
 *        应在 mqtt_manager_start() 之后调用。
 *
 * @param sms_queue SMS 队列句柄，用于上报队列深度指标。
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未先调用 early_init；ESP_FAIL 任务创建失败。
 */
esp_err_t remote_log_start(QueueHandle_t sms_queue);

#endif // REMOTE_LOG_H
