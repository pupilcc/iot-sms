#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "mqtt_manager.h"
#include "uart_at_manager.h" // 包含此头文件以访问全局变量 g_sim_operator 和 g_sim_phone_number

static const char *TAG = "mqtt_manager";

// Configuration from Kconfig
#define MQTT_BROKER_URI CONFIG_APP_MQTT_BROKER_URI
#define MQTT_TOPIC_SMS  CONFIG_APP_MQTT_TOPIC_SMS

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        s_mqtt_connected = true;
        // Optional: Subscribe to a command topic if needed
        // msg_id = esp_mqtt_client_subscribe(client, "/esp/commands", 0);
        // ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        s_mqtt_connected = false;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        // Handle incoming MQTT commands if subscribed
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        s_mqtt_connected = false;
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

void mqtt_manager_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        // Add other MQTT configurations if needed, e.g., client_id, username, password, LWT
        // .credentials.client_id = "esp32c3_sms_gateway",
        // .credentials.username = "your_username",
        // .credentials.password = "your_password",
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
    ESP_LOGI(TAG, "MQTT client started, connecting to %s", MQTT_BROKER_URI);
}

esp_err_t mqtt_manager_publish_sms(const sms_message_t *sms) {
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, cannot publish SMS.");
        return ESP_FAIL;
    }
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "MQTT client not initialized.");
        return ESP_FAIL;
    }

    // 获取当前时间戳
    time_t now;
    struct tm timeinfo;
    char timestamp[32];

    time(&now);
    localtime_r(&now, &timeinfo);

    // 格式化时间为 ISO 8601 格式: YYYY-MM-DDTHH:MM:SSZ
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

    // 计算新的JSON payload所需的缓冲区大小
    // 格式示例: {"sender":"%s","content":"%s","operator":"%s","timestamp":"%s"}
    // 最大长度估算: sender (32), content (256), operator (32), timestamp (32)
    // 加上固定的JSON字符 (引号, 逗号, 冒号, 大括号) 和 null 终止符
    // 大致: 32 + 256 + 32 + 32 + (固定JSON开销 ~ 80) = ~432 字节
    // 使用一个足够大的缓冲区，例如 512 字节。
    char payload[512];

    // 检查运营商和本机号码是否可用，如果为空则使用"UNKNOWN"
    const char *operator_str = (strlen(g_sim_operator) > 0) ? g_sim_operator : "UNKNOWN";

    // 构建包含运营商、时间戳的JSON payload
    // 示例 JSON 格式: {"sender": "+8613800000000", "content": "Hello World", "operator": "中国移动", "timestamp": "2025-11-12T10:30:00Z"}
    snprintf(payload, sizeof(payload),
             "{\"sender\":\"%s\",\"content\":\"%s\",\"operator\":\"%s\",\"timestamp\":\"%s\"}",
             sms->sender, sms->content, operator_str, timestamp);

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_SMS, payload, 0, 1, 0);
    if (msg_id == -1) {
        ESP_LOGE(TAG, "Failed to publish SMS message to topic %s", MQTT_TOPIC_SMS);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Published SMS (msg_id=%d) to topic %s: %s", msg_id, MQTT_TOPIC_SMS, payload);
    return ESP_OK;
}

bool mqtt_manager_is_connected(void) {
    return s_mqtt_connected;
}

esp_err_t mqtt_manager_publish_device_ready(const char *operator_name) {
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "MQTT client not initialized.");
        return ESP_FAIL;
    }

    // Wait for MQTT connection with timeout
    const int max_wait_ms = 30000; // Wait up to 30 seconds for MQTT connection
    const int wait_interval_ms = 1000;
    int waited_ms = 0;

    while (!s_mqtt_connected && waited_ms < max_wait_ms) {
        ESP_LOGI(TAG, "Waiting for MQTT connection before sending device ready message... (%d/%d ms)",
                 waited_ms, max_wait_ms);
        vTaskDelay(pdMS_TO_TICKS(wait_interval_ms));
        waited_ms += wait_interval_ms;
    }

    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected after waiting, cannot publish device ready message.");
        return ESP_FAIL;
    }

    // Get current timestamp
    time_t now;
    struct tm timeinfo;
    char timestamp[32];

    time(&now);
    localtime_r(&now, &timeinfo);

    // Format time as ISO 8601: YYYY-MM-DDTHH:MM:SSZ
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

    // Determine operator string
    const char *operator_str = (operator_name && strlen(operator_name) > 0) ? operator_name : "未知运营商";

    // Build human-readable message
    char message[64];
    snprintf(message, sizeof(message), "%s设备已就绪", operator_str);

    // Build JSON payload
    // Format: {"status":"ready","operator":"中国电信","timestamp":"2025-11-13T10:30:00Z","message":"中国电信设备已就绪"}
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"status\":\"ready\",\"operator\":\"%s\",\"timestamp\":\"%s\",\"message\":\"%s\"}",
             operator_str, timestamp, message);

    // Publish to 'esp32/device' topic
    const char *device_ready_topic = "esp32/device";
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, device_ready_topic, payload, 0, 1, 0);
    if (msg_id == -1) {
        ESP_LOGE(TAG, "Failed to publish device ready message to topic %s", device_ready_topic);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Published device ready (msg_id=%d) to topic %s: %s", msg_id, device_ready_topic, payload);
    return ESP_OK;
}
