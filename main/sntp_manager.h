#ifndef SNTP_MANAGER_H
#define SNTP_MANAGER_H

#include "esp_err.h"

/**
 * @brief Initializes SNTP (Simple Network Time Protocol) and synchronizes system time.
 *        This function blocks until time is synchronized or timeout occurs.
 *
 * @return ESP_OK if time synchronized successfully, ESP_FAIL otherwise.
 */
esp_err_t sntp_manager_init(void);

#endif // SNTP_MANAGER_H
