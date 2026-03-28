#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sms_storage.h"

static const char *TAG = "sms_storage";
static const char *NVS_NAMESPACE = "sms_failed";
static const char *NVS_KEY_COUNT = "count";
static const char *NVS_KEY_PREFIX = "sms_";

// Maximum number of SMS messages to store in NVS
#define MAX_STORED_SMS 20

esp_err_t sms_storage_init(void)
{
    // NVS is already initialized in main.c, we just verify it here
    ESP_LOGI(TAG, "SMS storage initialized (using NVS namespace: %s)", NVS_NAMESPACE);
    return ESP_OK;
}

esp_err_t sms_storage_save(const sms_message_t *sms)
{
    if (sms == NULL) {
        ESP_LOGE(TAG, "Cannot save NULL SMS");
        return ESP_FAIL;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Get current count
    uint32_t count = 0;
    err = nvs_get_u32(nvs_handle, NVS_KEY_COUNT, &count);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to read SMS count from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    // Check if storage is full
    if (count >= MAX_STORED_SMS) {
        ESP_LOGW(TAG, "SMS storage full (%d messages), cannot save new SMS", MAX_STORED_SMS);
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    // Create key for this SMS (sms_0, sms_1, sms_2, ...)
    char key[16];
    snprintf(key, sizeof(key), "%s%lu", NVS_KEY_PREFIX, (unsigned long)count);

    // Save SMS as blob
    err = nvs_set_blob(nvs_handle, key, sms, sizeof(sms_message_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save SMS to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    // Update count
    count++;
    err = nvs_set_u32(nvs_handle, NVS_KEY_COUNT, count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update SMS count in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Saved SMS to NVS (key=%s, total=%lu): Sender='%s'", key, (unsigned long)count, sms->sender);
    return ESP_OK;
}

esp_err_t sms_storage_get_next(sms_message_t *sms)
{
    if (sms == NULL) {
        ESP_LOGE(TAG, "Cannot retrieve SMS into NULL buffer");
        return ESP_FAIL;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS (namespace might not exist yet)
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace doesn't exist yet, no SMS stored
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Get current count
    uint32_t count = 0;
    err = nvs_get_u32(nvs_handle, NVS_KEY_COUNT, &count);
    if (err == ESP_ERR_NVS_NOT_FOUND || count == 0) {
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SMS count from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    // Get oldest SMS (sms_0)
    char key[16];
    snprintf(key, sizeof(key), "%s0", NVS_KEY_PREFIX);

    size_t required_size = sizeof(sms_message_t);
    err = nvs_get_blob(nvs_handle, key, sms, &required_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to retrieve SMS from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Retrieved SMS from NVS (key=%s): Sender='%s'", key, sms->sender);
    return ESP_OK;
}

esp_err_t sms_storage_delete_oldest(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Get current count
    uint32_t count = 0;
    err = nvs_get_u32(nvs_handle, NVS_KEY_COUNT, &count);
    if (err == ESP_ERR_NVS_NOT_FOUND || count == 0) {
        nvs_close(nvs_handle);
        return ESP_OK; // Nothing to delete
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SMS count from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    // Shift all SMS messages down by one (sms_1 -> sms_0, sms_2 -> sms_1, ...)
    for (uint32_t i = 0; i < count - 1; i++) {
        char src_key[16], dst_key[16];
        snprintf(src_key, sizeof(src_key), "%s%lu", NVS_KEY_PREFIX, (unsigned long)(i + 1));
        snprintf(dst_key, sizeof(dst_key), "%s%lu", NVS_KEY_PREFIX, (unsigned long)i);

        sms_message_t temp_sms;
        size_t required_size = sizeof(sms_message_t);

        // Read from source
        err = nvs_get_blob(nvs_handle, src_key, &temp_sms, &required_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read SMS during shift (key=%s): %s", src_key, esp_err_to_name(err));
            nvs_close(nvs_handle);
            return ESP_FAIL;
        }

        // Write to destination
        err = nvs_set_blob(nvs_handle, dst_key, &temp_sms, sizeof(sms_message_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write SMS during shift (key=%s): %s", dst_key, esp_err_to_name(err));
            nvs_close(nvs_handle);
            return ESP_FAIL;
        }
    }

    // Delete the last SMS entry
    char last_key[16];
    snprintf(last_key, sizeof(last_key), "%s%lu", NVS_KEY_PREFIX, (unsigned long)(count - 1));
    err = nvs_erase_key(nvs_handle, last_key);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to erase last SMS key (key=%s): %s", last_key, esp_err_to_name(err));
    }

    // Update count
    count--;
    err = nvs_set_u32(nvs_handle, NVS_KEY_COUNT, count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update SMS count in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Deleted oldest SMS from NVS, remaining count=%lu", (unsigned long)count);
    return ESP_OK;
}

int sms_storage_get_count(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS (namespace might not exist yet, which is okay)
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace doesn't exist yet, no SMS stored
        return 0;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return -1;
    }

    // Get current count
    uint32_t count = 0;
    err = nvs_get_u32(nvs_handle, NVS_KEY_COUNT, &count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return 0;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SMS count from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return -1;
    }

    nvs_close(nvs_handle);
    return (int)count;
}

esp_err_t sms_storage_clear_all(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // Open NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Erase all keys in this namespace
    err = nvs_erase_all(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase all SMS from NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return ESP_FAIL;
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Cleared all SMS from NVS storage");
    return ESP_OK;
}
