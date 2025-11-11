#ifndef SMS_PROCESSOR_H
#define SMS_PROCESSOR_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief FreeRTOS task to process received SMS messages from the UART queue
 *        and publish them via MQTT.
 * @param pvParameters Should be a QueueHandle_t for the SMS queue.
 */
void sms_processor_task(void *pvParameters);

#endif // SMS_PROCESSOR_H
