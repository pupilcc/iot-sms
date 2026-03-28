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

/**
 * @brief Check if Wi-Fi is currently connected.
 *
 * @return true if connected, false otherwise.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Check if Wi-Fi is connected and has a valid IP address.
 *
 * @return true if connected and has IP, false otherwise.
 */
bool wifi_manager_has_ip(void);

#endif // WIFI_MANAGER_H
