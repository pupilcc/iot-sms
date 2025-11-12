#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h" // For SemaphoreHandle_t
#include "freertos/event_groups.h" // For EventGroupHandle_t
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "uart_at_manager.h"

// Configuration from Kconfig
#define UART_PORT_NUM      CONFIG_APP_UART_PORT_NUM
#define UART_TXD           CONFIG_APP_UART_TXD
#define UART_RXD           CONFIG_APP_UART_RXD
#define UART_BAUD_RATE     CONFIG_APP_UART_BAUD_RATE

#define BUF_SIZE (1024)
#define AT_RESPONSE_MAX_LEN 512
#define AT_COMMAND_TIMEOUT_MS 10000 // 10 seconds for AT commands, increased for robustness
static const char *TAG = "uart_at_manager";
static QueueHandle_t s_sms_queue = NULL;
static QueueHandle_t s_uart_event_queue = NULL; // Declare event queue handle

// Global buffer for collecting UART responses
static char s_uart_rx_buffer[BUF_SIZE];
static int s_uart_rx_buffer_idx = 0;
static SemaphoreHandle_t s_uart_rx_mutex; // Mutex to protect rx_buffer access
static TaskHandle_t s_uart_event_task_handle = NULL; // Task handle for cleanup
static TaskHandle_t s_uart_at_task_handle = NULL; // Task handle for AT manager task

// Event group to signal AT command response
static EventGroupHandle_t s_at_response_event_group;
#define AT_RESPONSE_OK_BIT   BIT0
#define AT_RESPONSE_ERROR_BIT BIT1
#define AT_RESPONSE_URC_BIT   BIT2 // For unsolicited result codes like +CMT

// 全局变量定义和初始化
char g_sim_operator[32] = {0};

// Forward declarations
static int handle_urc(char *urc_line_buffer);
static esp_err_t at_send_command(const char *cmd, char *response_buffer, size_t buffer_size, TickType_t timeout_ticks);
static esp_err_t parse_cmt_text_mode_response(const char *response, sms_message_t *sms_msg);
static void decode_ucs2_hex_to_utf8(const char *ucs2_hex_str, char *utf8_buf, size_t utf8_buf_len);

// 新增的获取SIM卡信息的辅助函数
static esp_err_t get_sim_imsi(char *imsi_buffer, size_t buffer_size);
static esp_err_t get_sim_operator_name(char *operator_buffer, size_t buffer_size);

// IMSI到运营商的映射表 (参考Lua脚本)
typedef struct {
    const char *mcc_mnc_prefix;
    const char *operator_name_zh;
} sim_operator_map_t;

static const sim_operator_map_t s_operator_map[] = {
    {"46000", "中国移动"},
    {"46002", "中国移动"},
    {"46007", "中国移动"},
    {"46008", "中国移动"},
    {"46001", "中国联通"},
    {"46006", "中国联通"},
    {"46009", "中国联通"},
    {"46010", "中国联通"},
    {"46003", "中国电信"},
    {"46005", "中国电信"},
    {"46011", "中国电信"},
    {"46012", "中国电信"},
    {"46015", "中国广电"},
    // 可以根据需要添加其他运营商
    {"23410", "Giffgaff"},
    {"53005", "Skinny"},
};
static const size_t s_operator_map_size = sizeof(s_operator_map) / sizeof(s_operator_map[0]);


// UART event handler to collect data
static void uart_event_task(void *pvParameters) {
    uart_event_t event;
    uint8_t *dtmp = (uint8_t *) malloc(BUF_SIZE);
    if (!dtmp) {
        ESP_LOGE(TAG, "Failed to allocate memory for UART event task buffer");
        vTaskDelete(NULL);
    }

    while (1) {
        // Use xQueueReceive to read from the event queue
        if (xQueueReceive(s_uart_event_queue, &event, portMAX_DELAY) == pdPASS) {
            switch (event.type) {
                case UART_DATA:
                    xSemaphoreTake(s_uart_rx_mutex, portMAX_DELAY);
                    int read_len = uart_read_bytes(UART_PORT_NUM, dtmp, event.size, portMAX_DELAY);
                    if (read_len > 0) {
                        if (s_uart_rx_buffer_idx + read_len < BUF_SIZE) {
                            memcpy(s_uart_rx_buffer + s_uart_rx_buffer_idx, dtmp, read_len);
                            s_uart_rx_buffer_idx += read_len;
                            s_uart_rx_buffer[s_uart_rx_buffer_idx] = '\0'; // Null-terminate
                        } else {
                            ESP_LOGW(TAG, "UART RX buffer overflow, discarding old data.");
                            // Shift data to make space, or just discard
                            int overflow_len = (s_uart_rx_buffer_idx + read_len) - BUF_SIZE + 1;
                            if (overflow_len > 0) { // Ensure overflow_len is positive
                                memmove(s_uart_rx_buffer, s_uart_rx_buffer + overflow_len, BUF_SIZE - 1 - overflow_len);
                                s_uart_rx_buffer_idx -= overflow_len;
                            }
                            memcpy(s_uart_rx_buffer + s_uart_rx_buffer_idx, dtmp, read_len);
                            s_uart_rx_buffer_idx += read_len;
                            s_uart_rx_buffer[s_uart_rx_buffer_idx] = '\0';
                        }
                        ESP_LOGD(TAG, "Current RX buffer: %s", s_uart_rx_buffer); // Log current buffer content for debugging

                        // Check for URCs or command responses in the buffer
                        char *ok_pos = strstr(s_uart_rx_buffer, "OK\r\n");
                        char *error_pos = strstr(s_uart_rx_buffer, "ERROR\r\n");
                        char *prompt_pos = strstr(s_uart_rx_buffer, "> "); // For CMGS prompt

                        if (ok_pos) {
                            xEventGroupSetBits(s_at_response_event_group, AT_RESPONSE_OK_BIT);
                        } else if (error_pos || prompt_pos) {
                            xEventGroupSetBits(s_at_response_event_group, AT_RESPONSE_ERROR_BIT);
                        }
                        
                        // Check for +CMT URC
                        char *cmt_pos = strstr(s_uart_rx_buffer, "+CMT:");
                        if (cmt_pos) {
                            // Look for the end of the +CMT URC block: two \r\n sequences after +CMT: header
                            // +CMT: "sender",,"timestamp"\r\n<content>\r\n
                            char *first_crlf = strstr(cmt_pos, "\r\n");
                            if (first_crlf) {
                                char *second_crlf = strstr(first_crlf + 2, "\r\n"); // Look for second \r\n after the first
                                if (second_crlf) {
                                    // Found a complete +CMT URC block
                                    xEventGroupSetBits(s_at_response_event_group, AT_RESPONSE_URC_BIT);
                                }
                            }
                        }
                    }
                    xSemaphoreGive(s_uart_rx_mutex);
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART FIFO overflow");
                    uart_flush_input(UART_PORT_NUM);
                    xQueueReset(s_uart_event_queue); // Use the stored queue handle
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART RX buffer full");
                    uart_flush_input(UART_PORT_NUM);
                    xQueueReset(s_uart_event_queue); // Use the stored queue handle
                    break;
                case UART_BREAK:
                case UART_PARITY_ERR:
                case UART_FRAME_ERR:
                case UART_DATA_BREAK:
                case UART_PATTERN_DET:
                case UART_WAKEUP: // Added to handle the switch error
                case UART_EVENT_MAX:
                    ESP_LOGW(TAG, "UART event type: %d", event.type);
                    break;
                default: // Catch any other unhandled event types
                    ESP_LOGW(TAG, "Unhandled UART event type: %d", event.type);
                    break;
            }
        }
    }
    free(dtmp);
    vTaskDelete(NULL);
}


/**
 * @brief Sends an AT command and waits for a response.
 *
 * @param cmd The AT command string to send.
 * @param response_buffer Buffer to store the response.
 * @param buffer_size Size of the response_buffer.
 * @param timeout_ticks Timeout in FreeRTOS ticks.
 * @return ESP_OK if "OK" is found in response, ESP_FAIL otherwise.
 */
static esp_err_t at_send_command(const char *cmd, char *response_buffer, size_t buffer_size, TickType_t timeout_ticks) {
    ESP_LOGD(TAG, "Sending AT command: %s", cmd);

    xSemaphoreTake(s_uart_rx_mutex, portMAX_DELAY);
    s_uart_rx_buffer_idx = 0; // Clear buffer for new command
    s_uart_rx_buffer[0] = '\0';
    xSemaphoreGive(s_uart_rx_mutex);

    xEventGroupClearBits(s_at_response_event_group, AT_RESPONSE_OK_BIT | AT_RESPONSE_ERROR_BIT | AT_RESPONSE_URC_BIT); // Clear URC bit too

    uart_write_bytes(UART_PORT_NUM, cmd, strlen(cmd));
    uart_write_bytes(UART_PORT_NUM, "\r\n", 2); // AT commands usually end with CR+LF

    EventBits_t uxBits = xEventGroupWaitBits(s_at_response_event_group,
                                             AT_RESPONSE_OK_BIT | AT_RESPONSE_ERROR_BIT,
                                             pdTRUE, // Clear bits on exit
                                             pdFALSE, // Don't wait for all bits
                                             timeout_ticks);

    xSemaphoreTake(s_uart_rx_mutex, portMAX_DELAY);
    strncpy(response_buffer, s_uart_rx_buffer, buffer_size - 1);
    response_buffer[buffer_size - 1] = '\0';
    s_uart_rx_buffer_idx = 0; // Clear buffer after copying
    s_uart_rx_buffer[0] = '\0';
    xSemaphoreGive(s_uart_rx_mutex);

    if (uxBits & AT_RESPONSE_OK_BIT) {
        ESP_LOGD(TAG, "AT command response (OK): %s", response_buffer);
        return ESP_OK;
    } else if (uxBits & AT_RESPONSE_ERROR_BIT) {
        ESP_LOGW(TAG, "AT command response (ERROR/PROMPT): %s", response_buffer);
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "AT command timeout for: %s", cmd);
        return ESP_FAIL;
    }
}

/**
 * @brief Parses a +CMT response (Text Mode) to extract sender and content.
 * Example: +CMT: "+8613800000000","","23/08/15,10:30:00+32"\r\nHello World\r\n
 *
 * @param response The full AT response string.
 * @param sms_msg Pointer to sms_message_t to fill.
 * @return ESP_OK on success, ESP_FAIL if parsing fails.
 */
static esp_err_t parse_cmt_text_mode_response(const char *response, sms_message_t *sms_msg) {
    const char *cmt_prefix = "+CMT:";
    const char *sender_hex_start = NULL;
    const char *sender_hex_end = NULL;
    const char *content_hex_start = NULL;

    if (!response || !sms_msg) return ESP_FAIL;

    const char *line = strstr(response, cmt_prefix);
    if (!line) {
        ESP_LOGW(TAG, "CMT prefix not found in response.");
        return ESP_FAIL;
    }

    // Find sender number (between quotes after +CMT:)
    sender_hex_start = strchr(line, '"'); // First quote
    if (sender_hex_start) {
        sender_hex_start++; // Move past the quote
        sender_hex_end = strchr(sender_hex_start, '"'); // Sender number end
        if (sender_hex_end) {
            // Extract the UCS2 hex string for sender
            char temp_sender_hex[sizeof(sms_msg->sender) * 2 + 1]; // Max possible hex length
            int len = sender_hex_end - sender_hex_start;
            if (len > 0 && len < sizeof(temp_sender_hex)) {
                strncpy(temp_sender_hex, sender_hex_start, len);
                temp_sender_hex[len] = '\0';
                
                // Decode UCS2 hex to UTF-8
                decode_ucs2_hex_to_utf8(temp_sender_hex, sms_msg->sender, sizeof(sms_msg->sender));
                ESP_LOGD(TAG, "Decoded Sender: %s", sms_msg->sender);
            } else {
                ESP_LOGW(TAG, "Sender hex string too long or empty. Len: %d", len);
                strncpy(sms_msg->sender, "UNKNOWN", sizeof(sms_msg->sender));
            }
        }
    }

    // Find content (after the last line of +CMT: header and before next \r\n or end)
    content_hex_start = strstr(line, "\r\n"); // End of +CMT: header line
    if (content_hex_start) {
        content_hex_start += 2; // Move past \r\n
        const char *next_line_end = strstr(content_hex_start, "\r\n");
        int content_hex_len;
        if (next_line_end) {
            content_hex_len = next_line_end - content_hex_start;
        } else {
            content_hex_len = strlen(content_hex_start); // Take till end if no next line
        }

        // Trim trailing \r\n if present
        while (content_hex_len > 0 && (content_hex_start[content_hex_len - 1] == '\r' || content_hex_start[content_hex_len - 1] == '\n')) {
            content_hex_len--;
        }

        ESP_LOGD(TAG, "Content hex length: %d", content_hex_len);
        ESP_LOGD(TAG, "Content hex string (first 100 chars): %.100s", content_hex_start);

        if (content_hex_len > 0 && content_hex_len < (sizeof(sms_msg->content) * 2 + 1)) { // Max possible hex length
            char temp_content_hex[sizeof(sms_msg->content) * 2 + 1];
            strncpy(temp_content_hex, content_hex_start, content_hex_len);
            temp_content_hex[content_hex_len] = '\0';

            ESP_LOGD(TAG, "Calling decode_ucs2_hex_to_utf8 with hex string length: %d", content_hex_len);
            // Decode UCS2 hex to UTF-8
            decode_ucs2_hex_to_utf8(temp_content_hex, sms_msg->content, sizeof(sms_msg->content));
            ESP_LOGI(TAG, "Parsed SMS: Sender='%s', Content='%s'", sms_msg->sender, sms_msg->content);
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "SMS content hex string too long or empty. Len: %d", content_hex_len);
            strncpy(sms_msg->content, "EMPTY_OR_TOO_LONG", sizeof(sms_msg->content));
            return ESP_FAIL;
        }
    }

    ESP_LOGW(TAG, "Failed to parse SMS content from CMT response.");
    return ESP_FAIL;
}


esp_err_t uart_at_init(QueueHandle_t sms_queue) {
    s_sms_queue = sms_queue;

    // Delete existing AT task if running (important for device restarts)
    if (s_uart_at_task_handle != NULL) {
        ESP_LOGI(TAG, "UART AT task already running, deleting it first...");
        vTaskDelete(s_uart_at_task_handle);
        s_uart_at_task_handle = NULL;
        vTaskDelay(pdMS_TO_TICKS(100)); // Give time for task cleanup
    }

    // Delete existing UART event task if running (important for device restarts)
    if (s_uart_event_task_handle != NULL) {
        ESP_LOGI(TAG, "UART event task already running, deleting it first...");
        vTaskDelete(s_uart_event_task_handle);
        s_uart_event_task_handle = NULL;
        vTaskDelay(pdMS_TO_TICKS(100)); // Give time for task cleanup
    }

    // Clean up existing UART driver if already installed (important for device restarts)
    if (uart_is_driver_installed(UART_PORT_NUM)) {
        ESP_LOGI(TAG, "UART driver already installed on port %d, uninstalling first...", UART_PORT_NUM);
        uart_driver_delete(UART_PORT_NUM);
        vTaskDelay(pdMS_TO_TICKS(100)); // Give time for cleanup
    }

    // Create or reset synchronization primitives
    if (s_uart_rx_mutex == NULL) {
        s_uart_rx_mutex = xSemaphoreCreateMutex();
    }
    if (s_at_response_event_group == NULL) {
        s_at_response_event_group = xEventGroupCreate();
    } else {
        // Clear all bits if event group already exists
        xEventGroupClearBits(s_at_response_event_group, 0xFFFFFF);
    }

    // Clear UART RX buffer
    s_uart_rx_buffer_idx = 0;
    s_uart_rx_buffer[0] = '\0';

    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;
#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    // Install UART driver with event queue
    // The last parameter is a pointer to the event queue handle
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &s_uart_event_queue, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TXD, UART_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Create a task to handle UART events and save the handle
    xTaskCreate(uart_event_task, "uart_event_task", 3072, NULL, 10, &s_uart_event_task_handle);

    ESP_LOGI(TAG, "UART AT manager initialized on port %d, TX:%d, RX:%d, Baud:%d",
             UART_PORT_NUM, UART_TXD, UART_RXD, UART_BAUD_RATE);

    return ESP_OK;
}

// Helper function to get IMSI
static esp_err_t get_sim_imsi(char *imsi_buffer, size_t buffer_size) {
    char response[AT_RESPONSE_MAX_LEN];
    if (at_send_command("AT+CIMI", response, sizeof(response), pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IMSI.");
        return ESP_FAIL;
    }

    // Parse IMSI from response. It's usually the line before "OK"
    // Example: \r\n460001234567890\r\n\r\nOK\r\n
    char *start = strstr(response, "\r\n");
    if (start) {
        start += 2; // Move past the first \r\n
        char *end = strstr(start, "\r\nOK"); // Find the start of OK
        if (end) {
            int len = end - start;
            if (len > 0 && len < buffer_size) {
                strncpy(imsi_buffer, start, len);
                imsi_buffer[len] = '\0';
                ESP_LOGI(TAG, "IMSI: %s", imsi_buffer);
                return ESP_OK;
            }
        }
    }
    ESP_LOGE(TAG, "Failed to parse IMSI from response: %s", response);
    return ESP_FAIL;
}

// Helper function to get SIM operator name
static esp_err_t get_sim_operator_name(char *operator_buffer, size_t buffer_size) {
    char imsi[20]; // IMSI is typically 15 digits
    if (get_sim_imsi(imsi, sizeof(imsi)) != ESP_OK) {
        strncpy(operator_buffer, "UNKNOWN", buffer_size);
        return ESP_FAIL;
    }

    if (strlen(imsi) < 5) {
        ESP_LOGW(TAG, "IMSI too short to determine operator: %s", imsi);
        strncpy(operator_buffer, "UNKNOWN", buffer_size);
        return ESP_FAIL;
    }

    char mcc_mnc_prefix[6]; // e.g., "46000" + null terminator
    strncpy(mcc_mnc_prefix, imsi, 5);
    mcc_mnc_prefix[5] = '\0';

    for (size_t i = 0; i < s_operator_map_size; i++) {
        if (strcmp(mcc_mnc_prefix, s_operator_map[i].mcc_mnc_prefix) == 0) {
            strncpy(operator_buffer, s_operator_map[i].operator_name_zh, buffer_size - 1);
            operator_buffer[buffer_size - 1] = '\0';
            ESP_LOGI(TAG, "SIM Operator: %s (IMSI prefix: %s)", operator_buffer, mcc_mnc_prefix);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Unknown SIM operator for IMSI prefix: %s", mcc_mnc_prefix);
    strncpy(operator_buffer, "UNKNOWN", buffer_size);
    return ESP_FAIL;
}


void uart_at_task(void *pvParameters) {
    char response_buffer[AT_RESPONSE_MAX_LEN];
    // sms_message_t new_sms; // Removed as it's passed to handle_urc

    // Register this task's handle for cleanup on restart
    s_uart_at_task_handle = xTaskGetCurrentTaskHandle();

    // Initialize 4G Cat.1 modem
    ESP_LOGI(TAG, "Initializing 4G Cat.1 modem...");
    vTaskDelay(pdMS_TO_TICKS(5000)); // Give modem more time to boot (5 seconds)

    // Flush UART buffers to clear any residual data from previous sessions
    uart_flush(UART_PORT_NUM);
    xSemaphoreTake(s_uart_rx_mutex, portMAX_DELAY);
    s_uart_rx_buffer_idx = 0;
    s_uart_rx_buffer[0] = '\0';
    xSemaphoreGive(s_uart_rx_mutex);
    ESP_LOGI(TAG, "UART buffers flushed, ready for AT commands.");

    // Send wake-up sequence to ensure modem is responsive
    // This is important when ESP32 restarts but modem is still running
    ESP_LOGI(TAG, "Sending wake-up sequence to modem...");
    for (int i = 0; i < 3; i++) {
        uart_write_bytes(UART_PORT_NUM, "AT\r\n", 4);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // Clear any responses from wake-up sequence
    vTaskDelay(pdMS_TO_TICKS(500));
    uart_flush(UART_PORT_NUM);
    xSemaphoreTake(s_uart_rx_mutex, portMAX_DELAY);
    s_uart_rx_buffer_idx = 0;
    s_uart_rx_buffer[0] = '\0';
    xSemaphoreGive(s_uart_rx_mutex);
    xEventGroupClearBits(s_at_response_event_group, AT_RESPONSE_OK_BIT | AT_RESPONSE_ERROR_BIT | AT_RESPONSE_URC_BIT);
    ESP_LOGI(TAG, "Wake-up sequence complete, modem should be responsive.");

    // 1. Test AT command (with retry mechanism)
    int retry_count = 0;
    const int max_retries = 3;
    esp_err_t at_result = ESP_FAIL;

    while (retry_count < max_retries && at_result != ESP_OK) {
        if (retry_count > 0) {
            ESP_LOGW(TAG, "Retrying AT command (%d/%d)...", retry_count + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retry
        }
        at_result = at_send_command("AT", response_buffer, sizeof(response_buffer), pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS));
        retry_count++;
    }

    if (at_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to communicate with 4G modem after %d attempts.", max_retries);
        // Consider a retry mechanism or reboot here
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "AT command successful, modem is responding.");
    vTaskDelay(pdMS_TO_TICKS(500));

    // 2. Disable echo (ATE0) - This is important to simplify parsing
    if (at_send_command("ATE0", response_buffer, sizeof(response_buffer), pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable AT command echo. Parsing might be more complex.");
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // 3. Set SMS to text mode (AT+CMGF=1) - Sticking to text mode for now to avoid PDU decoding complexity
    if (at_send_command("AT+CMGF=1", response_buffer, sizeof(response_buffer), pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set SMS to text mode (AT+CMGF=1).");
        vTaskDelete(NULL);
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // 4. Set character set to UCS2 (AT+CSCS="UCS2") - Important for Chinese characters
    if (at_send_command("AT+CSCS=\"UCS2\"", response_buffer, sizeof(response_buffer), pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set character set to UCS2 (AT+CSCS=\"UCS2\"). SMS might be garbled.");
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // 5. Configure new SMS message indications (AT+CNMI=2,2,0,0,0) - Direct URC, no storage
    if (at_send_command("AT+CNMI=2,2,0,0,0", response_buffer, sizeof(response_buffer), pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure new SMS indications (AT+CNMI).");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "4G modem initialized for SMS reception.");
    vTaskDelay(pdMS_TO_TICKS(500));

    // --- 新增：获取SIM卡运营商和本机号码 ---
    // 6. 获取SIM卡运营商
    if (get_sim_operator_name(g_sim_operator, sizeof(g_sim_operator)) != ESP_OK) {
        ESP_LOGW(TAG, "Could not determine SIM operator.");
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "4G modem initialization complete. Operator: %s", g_sim_operator);
    // --- 新增结束 ---

    // Main loop to listen for incoming URCs (like +CMT:)
    while (1) {
        // Wait for a URC to be signaled by the uart_event_task
        EventBits_t uxBits = xEventGroupWaitBits(s_at_response_event_group,
                                                 AT_RESPONSE_URC_BIT,
                                                 pdTRUE, // Clear bit on exit
                                                 pdFALSE, // Don't wait for all bits
                                                 portMAX_DELAY); // Wait indefinitely

        if (uxBits & AT_RESPONSE_URC_BIT) {
            xSemaphoreTake(s_uart_rx_mutex, portMAX_DELAY);
            ESP_LOGI(TAG, "Received URC from 4G modem: %s", s_uart_rx_buffer);

            // Process URCs one by one from the buffer
            int processed_len = 0;
            do {
                processed_len = handle_urc(s_uart_rx_buffer);
                if (processed_len > 0) {
                    // Shift remaining data to the beginning of the buffer
                    memmove(s_uart_rx_buffer, s_uart_rx_buffer + processed_len, s_uart_rx_buffer_idx - processed_len + 1); // +1 for null terminator
                    s_uart_rx_buffer_idx -= processed_len;
                    ESP_LOGD(TAG, "Buffer after URC processing: %s", s_uart_rx_buffer);
                }
            } while (processed_len > 0 && s_uart_rx_buffer_idx > 0); // Keep processing if more URCs are in buffer

            // After processing, check if there are still complete URCs in the buffer
            // This is important if multiple URCs arrived in one go
            char *cmt_pos = strstr(s_uart_rx_buffer, "+CMT:");
            if (cmt_pos) {
                char *first_crlf = strstr(cmt_pos, "\r\n");
                if (first_crlf && strstr(first_crlf + 2, "\r\n")) {
                    xEventGroupSetBits(s_at_response_event_group, AT_RESPONSE_URC_BIT); // Re-signal if another URC is ready
                }
            }
            xSemaphoreGive(s_uart_rx_mutex);
        }
    }
    vTaskDelete(NULL);
}

static int handle_urc(char *urc_line_buffer) { // Now takes a mutable buffer
    sms_message_t new_sms;
    int urc_len = 0;

    // Check for new SMS indication (+CMT:)
    char *cmt_ptr = strstr(urc_line_buffer, "+CMT:");
    if (cmt_ptr) {
        // Find the end of this specific +CMT URC block
        char *first_crlf = strstr(cmt_ptr, "\r\n");
        if (first_crlf) {
            char *second_crlf = strstr(first_crlf + 2, "\r\n");
            if (second_crlf) {
                urc_len = (second_crlf - cmt_ptr) + 2; // Length from start of +CMT to end of second \r\n

                // Temporarily null-terminate the URC block for parsing
                char temp_char = cmt_ptr[urc_len];
                cmt_ptr[urc_len] = '\0';

                ESP_LOGI(TAG, "New SMS received (direct URC).");
                if (parse_cmt_text_mode_response(cmt_ptr, &new_sms) == ESP_OK) {
                    if (s_sms_queue != NULL) {
                        if (xQueueSend(s_sms_queue, &new_sms, portMAX_DELAY) != pdPASS) {
                            ESP_LOGE(TAG, "Failed to send SMS to queue.");
                        }
                    }
                }
                cmt_ptr[urc_len] = temp_char; // Restore original char
                return (cmt_ptr - urc_line_buffer) + urc_len; // Return total length processed from buffer start
            }
        }
    }
    // Add other URC handlers here if needed (e.g., +CGATT, +CPIN)
    // For this example, we only focus on +CMT
    return 0; // No complete URC processed
}

// Helper function to convert a hex char to its integer value
static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1; // Invalid hex char
}

// Function to decode UCS2 hex string to UTF-8 string
// Note: This is a simplified UCS2 to UTF-8 conversion.
// It assumes basic multilingual plane characters.
// For full Unicode support, a more robust library might be needed.
static void decode_ucs2_hex_to_utf8(const char *ucs2_hex_str, char *utf8_buf, size_t utf8_buf_len) {
    size_t ucs2_hex_len = strlen(ucs2_hex_str);
    size_t utf8_idx = 0;

    ESP_LOGD(TAG, "decode_ucs2_hex_to_utf8: Input hex length: %d, Output buffer size: %d", ucs2_hex_len, utf8_buf_len);

    for (size_t i = 0; i + 4 <= ucs2_hex_len && utf8_idx < utf8_buf_len - 4; i += 4) {
        int h1 = hex_char_to_int(ucs2_hex_str[i]);
        int h2 = hex_char_to_int(ucs2_hex_str[i+1]);
        int h3 = hex_char_to_int(ucs2_hex_str[i+2]);
        int h4 = hex_char_to_int(ucs2_hex_str[i+3]);

        if (h1 == -1 || h2 == -1 || h3 == -1 || h4 == -1) {
            ESP_LOGW(TAG, "Invalid UCS2 hex character at position %d: %c%c%c%c",
                     i, ucs2_hex_str[i], ucs2_hex_str[i+1], ucs2_hex_str[i+2], ucs2_hex_str[i+3]);
            break;
        }

        uint16_t ucs2_char = (h1 << 12) | (h2 << 8) | (h3 << 4) | h4;

        // Convert UCS2 to UTF-8
        if (ucs2_char < 0x80) { // 1-byte UTF-8 (ASCII)
            utf8_buf[utf8_idx++] = (char)ucs2_char;
        } else if (ucs2_char < 0x800) { // 2-byte UTF-8
            if (utf8_idx + 1 < utf8_buf_len - 1) {
                utf8_buf[utf8_idx++] = 0xC0 | (ucs2_char >> 6);
                utf8_buf[utf8_idx++] = 0x80 | (ucs2_char & 0x3F);
            } else {
                ESP_LOGW(TAG, "UTF-8 buffer too small for 2-byte char at position %d (utf8_idx=%d)", i, utf8_idx);
                break; // Buffer too small
            }
        } else { // 3-byte UTF-8
            if (utf8_idx + 2 < utf8_buf_len - 1) {
                utf8_buf[utf8_idx++] = 0xE0 | (ucs2_char >> 12);
                utf8_buf[utf8_idx++] = 0x80 | ((ucs2_char >> 6) & 0x3F);
                utf8_buf[utf8_idx++] = 0x80 | (ucs2_char & 0x3F);
            } else {
                ESP_LOGW(TAG, "UTF-8 buffer too small for 3-byte char at position %d (utf8_idx=%d)", i, utf8_idx);
                break; // Buffer too small
            }
        }
    }
    ESP_LOGD(TAG, "decode_ucs2_hex_to_utf8: Decoded %d UTF-8 bytes from %d hex chars", utf8_idx, ucs2_hex_len);
    utf8_buf[utf8_idx] = '\0'; // Null-terminate
}
