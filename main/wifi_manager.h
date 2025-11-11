#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"

/**
 * @brief Initializes Wi-Fi in station mode and connects to the configured AP.
 *        This function blocks until Wi-Fi is connected or maximum retries are exhausted.
 *
 * @return ESP_OK if connected successfully, ESP_FAIL otherwise.
 */
esp_err_t wifi_manager_init_sta(void);

#endif // WIFI_MANAGER_H
