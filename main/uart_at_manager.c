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
#include "mqtt_manager.h"
#include "log_redaction.h"
#include "sms_pdu.h"

// Configuration from Kconfig
#define UART_PORT_NUM      CONFIG_APP_UART_PORT_NUM
#define UART_TXD           CONFIG_APP_UART_TXD
#define UART_RXD           CONFIG_APP_UART_RXD
#define UART_BAUD_RATE     CONFIG_APP_UART_BAUD_RATE

#define BUF_SIZE (4096)  // Increased from 1024 to handle long SMS and burst URCs
#define AT_RESPONSE_MAX_LEN 1024  // Increased from 512 for longer responses
#define AT_COMMAND_TIMEOUT_MS 10000 // 10 seconds for AT commands, increased for robustness
#define AT_PROBE_MAX_RETRIES 3
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

// 长短信重组:按UDH中的(参考号,总段数,序号)归组,支持乱序到达
#define SMS_CONCAT_TIMEOUT_MS 30000 // 30秒内剩余分段仍未到达则冲刷已收到的部分
#define SMS_CONCAT_MAX_PARTS  10    // 最大支持10段SMS拼接

typedef struct {
    bool       active;
    char       sender[32];
    uint16_t   ref;                 // 拼接参考号
    uint8_t    total;               // 总段数(已按SMS_CONCAT_MAX_PARTS截断)
    uint32_t   received_mask;       // bit(i)表示seq=i+1已收到
    char       parts[SMS_CONCAT_MAX_PARTS][SMS_PDU_TEXT_MAX];
    TickType_t last_part_time;
} sms_concat_buffer_t;

static sms_concat_buffer_t s_concat = {0};

// PDU模式下+CMT正文是单行hex,长度上界由TPDU大小决定,远小于文本模式
#define SMS_PDU_HEX_MAX 512

// 直投短信的确认要挡在收信路径上,超时时间取得比普通命令短
#define AT_CNMA_TIMEOUT_MS 3000
// +CMTI待读取索引的暂存深度,以及启动时清空模组存储的最大轮数
#define SMS_PENDING_READ_MAX 8
#define SMS_DRAIN_MAX_ROUNDS 10

// URC解析在持有s_uart_rx_mutex的上下文里跑,不能就地发AT命令(at_send_command会
// 重入同一把非递归互斥锁)。这里只记下待办,回到uart_at_task主循环再执行。
static bool s_cmt_ack_pending = false;
static int  s_pending_read_index[SMS_PENDING_READ_MAX];
static int  s_pending_read_count = 0;

// 模组是否要求TE确认直投短信。仅在回读确认模组处于phase 1时才关掉:漏发确认会让
// 网络停投,代价远大于多发一条,所以拿不准时一律发。
static bool s_cnma_required = true;

// Forward declarations
static int handle_urc(char *urc_line_buffer);
static bool has_complete_sms_urc_locked(void);
static esp_err_t at_send_command(const char *cmd, char *response_buffer, size_t buffer_size, TickType_t timeout_ticks);
static esp_err_t configure_modem_for_sms(char *response_buffer, size_t buffer_size,
                                         bool *modem_responding);
static void process_pending_sms_urcs(void);
static void wait_for_recovery_retry(TickType_t delay_ticks);
static esp_err_t extract_and_dispatch_pdu(const char *response, const char *header);
static void ack_direct_sms_if_pending(void);
static void read_pending_stored_sms(void);
static void drain_stored_sms(void);

// 长短信重组相关函数声明
static bool is_concat_timeout(void);
static void reset_concat_buffer(void);
static void assemble_concat_buffer(sms_message_t *sms);
static void flush_concat_buffer_to_queue(const char *reason);
static esp_err_t process_sms_part(const sms_pdu_t *part, sms_message_t *complete_sms);

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
    {"45403", "Haha"},
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
                        // 检查是否会溢出
                        if (s_uart_rx_buffer_idx + read_len >= BUF_SIZE) {
                            ESP_LOGW(TAG, "UART RX buffer near overflow! Current: %d, Incoming: %d, Max: %d",
                                     s_uart_rx_buffer_idx, read_len, BUF_SIZE);

                            // 尝试查找并保留+CMT消息,删除其他内容
                            char *cmt_start = strstr(s_uart_rx_buffer, "+CMT:");
                            if (cmt_start) {
                                // 找到+CMT消息,保留它及之后的内容
                                int keep_offset = cmt_start - s_uart_rx_buffer;
                                ESP_LOGI(TAG, "Preserving +CMT message, discarding %d bytes before it", keep_offset);
                                memmove(s_uart_rx_buffer, cmt_start, s_uart_rx_buffer_idx - keep_offset);
                                s_uart_rx_buffer_idx -= keep_offset;
                            } else {
                                // 没有+CMT消息,清空缓冲区以接收新数据
                                ESP_LOGW(TAG, "No +CMT in buffer, clearing to make space");
                                s_uart_rx_buffer_idx = 0;
                            }
                        }

                        // 再次检查空间是否足够
                        if (s_uart_rx_buffer_idx + read_len < BUF_SIZE) {
                            memcpy(s_uart_rx_buffer + s_uart_rx_buffer_idx, dtmp, read_len);
                            s_uart_rx_buffer_idx += read_len;
                            s_uart_rx_buffer[s_uart_rx_buffer_idx] = '\0'; // Null-terminate
                        } else {
                            // 仍然放不下,只能截断
                            int can_copy = BUF_SIZE - s_uart_rx_buffer_idx - 1;
                            if (can_copy > 0) {
                                ESP_LOGW(TAG, "Buffer still full, copying only %d of %d bytes", can_copy, read_len);
                                memcpy(s_uart_rx_buffer + s_uart_rx_buffer_idx, dtmp, can_copy);
                                s_uart_rx_buffer_idx += can_copy;
                                s_uart_rx_buffer[s_uart_rx_buffer_idx] = '\0';
                            }
                        }
                        ESP_LOGD(TAG, "UART RX buffer updated (len=%d)", s_uart_rx_buffer_idx);

                        // 清理非SMS相关的URC消息,避免缓冲区堆积
                        // 保留AT命令响应、扩展错误响应和+CMT消息
                        char *buffer_ptr = s_uart_rx_buffer;
                        while (*buffer_ptr) {
                            // 检查是否是需要忽略的URC (不是+CMT的其他URC)
                            if (*buffer_ptr == '+' || *buffer_ptr == '^') {
                                // +CME ERROR/+CMS ERROR是AT命令的终止响应，不是普通URC
                                // +CMTI是模组把短信存进存储后的通知,必须保留:丢掉它
                                // 等于丢掉整条短信,而且存储会一直涨到写满
                                bool should_preserve_line = strncmp(buffer_ptr, "+CMT:", 5) == 0 ||
                                                            strncmp(buffer_ptr, "+CMTI:", 6) == 0 ||
                                                            strncmp(buffer_ptr, "+CME ERROR:", 11) == 0 ||
                                                            strncmp(buffer_ptr, "+CMS ERROR:", 11) == 0;
                                if (should_preserve_line) {
                                    // 跳过这一行以保留它
                                    char *line_end = strstr(buffer_ptr, "\r\n");
                                    if (line_end) {
                                        buffer_ptr = line_end + 2;
                                    } else {
                                        break; // 不完整的行,保留
                                    }
                                } else {
                                    // 这是其他URC (如+CGEV, ^MODE, +NITZ等),需要删除
                                    char *line_end = strstr(buffer_ptr, "\r\n");
                                    if (line_end) {
                                        int line_len = line_end + 2 - buffer_ptr;
                                        ESP_LOGD(TAG, "Removing non-SMS URC (len=%d)", line_len);
                                        // 移除这一行
                                        memmove(buffer_ptr, line_end + 2, strlen(line_end + 2) + 1);
                                        s_uart_rx_buffer_idx -= line_len;
                                        // 不移动buffer_ptr,因为内容已经前移
                                    } else {
                                        break; // 不完整的行,保留
                                    }
                                }
                            } else {
                                // 不是URC,跳到下一行
                                char *line_end = strstr(buffer_ptr, "\r\n");
                                if (line_end) {
                                    buffer_ptr = line_end + 2;
                                } else {
                                    break;
                                }
                            }
                        }

                        // Check for URCs or command responses in the buffer
                        char *ok_pos = strstr(s_uart_rx_buffer, "OK\r\n");
                        char *error_pos = strstr(s_uart_rx_buffer, "ERROR\r\n");
                        char *cme_error_pos = strstr(s_uart_rx_buffer, "+CME ERROR:");
                        char *cms_error_pos = strstr(s_uart_rx_buffer, "+CMS ERROR:");
                        char *prompt_pos = strstr(s_uart_rx_buffer, "> "); // For CMGS prompt

                        if (ok_pos) {
                            xEventGroupSetBits(s_at_response_event_group, AT_RESPONSE_OK_BIT);
                        } else if (error_pos || cme_error_pos || cms_error_pos || prompt_pos) {
                            xEventGroupSetBits(s_at_response_event_group, AT_RESPONSE_ERROR_BIT);
                        }

                        // Check for a complete incoming-SMS URC (+CMT block or +CMTI line)
                        if (has_complete_sms_urc_locked()) {
                            xEventGroupSetBits(s_at_response_event_group, AT_RESPONSE_URC_BIT);
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


// +CMT占两行(头行 + PDU行), +CMTI只有一行
static bool is_complete_urc_at(const char *pos, bool two_lines) {
    const char *first_crlf = strstr(pos, "\r\n");
    if (first_crlf == NULL) {
        return false;
    }
    if (!two_lines) {
        return true;
    }
    return strstr(first_crlf + 2, "\r\n") != NULL;
}

static bool has_complete_sms_urc_locked(void) {
    const char *cmt_pos = strstr(s_uart_rx_buffer, "+CMT:");
    if (cmt_pos != NULL && is_complete_urc_at(cmt_pos, true)) {
        return true;
    }

    const char *cmti_pos = strstr(s_uart_rx_buffer, "+CMTI:");
    return cmti_pos != NULL && is_complete_urc_at(cmti_pos, false);
}

static void process_pending_sms_urcs_locked(void) {
    int processed_len;

    do {
        processed_len = handle_urc(s_uart_rx_buffer);
        if (processed_len > 0) {
            memmove(s_uart_rx_buffer,
                    s_uart_rx_buffer + processed_len,
                    s_uart_rx_buffer_idx - processed_len + 1);
            s_uart_rx_buffer_idx -= processed_len;
            ESP_LOGD(TAG, "Buffer after URC processing (len=%d)", s_uart_rx_buffer_idx);
        }
    } while (processed_len > 0 && s_uart_rx_buffer_idx > 0);

    if (has_complete_sms_urc_locked()) {
        xEventGroupSetBits(s_at_response_event_group, AT_RESPONSE_URC_BIT);
    } else {
        xEventGroupClearBits(s_at_response_event_group, AT_RESPONSE_URC_BIT);
    }
}

static void process_pending_sms_urcs(void) {
    xSemaphoreTake(s_uart_rx_mutex, portMAX_DELAY);
    process_pending_sms_urcs_locked();
    xSemaphoreGive(s_uart_rx_mutex);
}

static void clear_command_data_preserving_sms_locked(void) {
    // 两种收信URC都可能只到达了一半,取靠前的那个作为保留起点
    char *cmt_start = strstr(s_uart_rx_buffer, "+CMT:");
    char *cmti_start = strstr(s_uart_rx_buffer, "+CMTI:");

    if (cmt_start == NULL || (cmti_start != NULL && cmti_start < cmt_start)) {
        cmt_start = cmti_start;
    }

    if (cmt_start != NULL) {
        int preserved_len = s_uart_rx_buffer_idx - (int)(cmt_start - s_uart_rx_buffer);
        memmove(s_uart_rx_buffer, cmt_start, preserved_len);
        s_uart_rx_buffer_idx = preserved_len;
        s_uart_rx_buffer[s_uart_rx_buffer_idx] = '\0';
        ESP_LOGD(TAG, "Preserved %d bytes of an incomplete SMS URC", s_uart_rx_buffer_idx);
    } else {
        s_uart_rx_buffer_idx = 0;
        s_uart_rx_buffer[0] = '\0';
    }
}

static void wait_for_recovery_retry(TickType_t delay_ticks) {
    TickType_t start_time = xTaskGetTickCount();

    while (xTaskGetTickCount() - start_time < delay_ticks) {
        TickType_t elapsed = xTaskGetTickCount() - start_time;
        TickType_t remaining = delay_ticks - elapsed;
        EventBits_t bits = xEventGroupWaitBits(s_at_response_event_group,
                                               AT_RESPONSE_URC_BIT,
                                               pdTRUE,
                                               pdFALSE,
                                               remaining);
        if (bits & AT_RESPONSE_URC_BIT) {
            process_pending_sms_urcs();
        } else {
            break;
        }
    }
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
    ESP_LOGD(TAG, "Sending AT command");

    xSemaphoreTake(s_uart_rx_mutex, portMAX_DELAY);
    // Process complete SMS reports first. If a report is still arriving, keep
    // it in the buffer instead of discarding it for the command transaction.
    process_pending_sms_urcs_locked();
    clear_command_data_preserving_sms_locked();
    xSemaphoreGive(s_uart_rx_mutex);

    xEventGroupClearBits(s_at_response_event_group, AT_RESPONSE_OK_BIT | AT_RESPONSE_ERROR_BIT);

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
    process_pending_sms_urcs_locked();
    clear_command_data_preserving_sms_locked();
    xSemaphoreGive(s_uart_rx_mutex);

    // 命令字符串本身不含敏感数据(敏感内容只出现在响应里),带上它才能定位是哪条超时
    if (uxBits & AT_RESPONSE_OK_BIT) {
        ESP_LOGD(TAG, "'%s' succeeded (response_len=%u)",
                 cmd, (unsigned)strlen(response_buffer));
        return ESP_OK;
    } else if (uxBits & AT_RESPONSE_ERROR_BIT) {
        ESP_LOGW(TAG, "'%s' failed (response_len=%u)",
                 cmd, (unsigned)strlen(response_buffer));
        return ESP_FAIL;
    } else {
        if (response_buffer[0] != '\0') {
            ESP_LOGE(TAG, "'%s' timed out (partial_response_len=%u)",
                     cmd, (unsigned)strlen(response_buffer));
        } else {
            ESP_LOGE(TAG, "'%s' timed out (no response received)", cmd);
        }
        return ESP_FAIL;
    }
}

/**
 * @brief 把一条完整的SMS投递到处理队列
 */
static void queue_complete_sms(sms_message_t *sms) {
    if (s_sms_queue == NULL) {
        return;
    }
    if (xQueueSend(s_sms_queue, sms, portMAX_DELAY) != pdPASS) {
        ESP_LOGE(TAG, "Failed to send complete SMS to queue.");
    } else {
        ESP_LOGI(TAG, "Complete SMS sent to processing queue.");
    }
}

/**
 * @brief 从<header>所在行的下一行取出PDU,解码、重组,集齐后直接投递到队列。
 *
 * 三种来源共用:直投的 +CMT: ,23\r\n0791...、读取存储的 +CMGR: 1,,23\r\n0791...、
 * 以及列举存储的 +CMGL: 1,1,,23\r\n0791...。头行的<alpha>/<length>一律不使用,
 * 发件人与正文都从PDU自身解出,不依赖模组的文本模式转换。
 *
 * @param response 含有该头行的响应文本
 * @param header   头行前缀,如"+CMT:"
 * @return ESP_OK 完整SMS已投递,
 *         ESP_ERR_INVALID_STATE 这是一个分段,已存储待续,
 *         ESP_FAIL 解析或解码失败
 */
static esp_err_t extract_and_dispatch_pdu(const char *response, const char *header) {
    // 均只在uart_at_task单线程内使用,static避免在深调用链上压栈
    static char s_pdu_hex[SMS_PDU_HEX_MAX];
    static sms_pdu_t s_part;
    static sms_message_t s_sms;

    if (!response || !header) return ESP_FAIL;

    const char *line = strstr(response, header);
    if (!line) {
        ESP_LOGW(TAG, "%s prefix not found in response.", header);
        return ESP_FAIL;
    }

    // PDU在头行之后的一行
    const char *pdu_start = strstr(line, "\r\n");
    if (!pdu_start) {
        ESP_LOGW(TAG, "Failed to locate PDU line in CMT response.");
        return ESP_FAIL;
    }
    pdu_start += 2;

    const char *pdu_end = strstr(pdu_start, "\r\n");
    size_t pdu_hex_len = pdu_end ? (size_t)(pdu_end - pdu_start) : strlen(pdu_start);
    while (pdu_hex_len > 0 && (pdu_start[pdu_hex_len - 1] == '\r' ||
                               pdu_start[pdu_hex_len - 1] == '\n' ||
                               pdu_start[pdu_hex_len - 1] == ' ')) {
        pdu_hex_len--;
    }
    if (pdu_hex_len == 0) {
        ESP_LOGW(TAG, "SMS PDU line is empty");
        return ESP_FAIL;
    }
    if (pdu_hex_len >= sizeof(s_pdu_hex)) {
        ESP_LOGE(TAG, "SMS PDU line too long (hex_len=%u), dropping", (unsigned)pdu_hex_len);
        return ESP_FAIL;
    }

    memcpy(s_pdu_hex, pdu_start, pdu_hex_len);
    s_pdu_hex[pdu_hex_len] = '\0';
    // DEBUG级:默认编译期即被裁掉,避免短信原文经remote_log外泄
    ESP_LOGD(TAG, "Raw PDU: %s", s_pdu_hex);

    if (sms_pdu_decode(s_pdu_hex, &s_part) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode SMS PDU (hex_len=%u)", (unsigned)pdu_hex_len);
        return ESP_FAIL;
    }

    char masked_sender[LOG_MASKED_PHONE_SIZE];
    log_mask_phone(s_part.sender, masked_sender, sizeof(masked_sender));
    if (s_part.concat) {
        ESP_LOGI(TAG, "SMS part %u/%u (ref=%u) decoded from '%s', text_len=%u",
                 s_part.seq, s_part.total, s_part.ref, masked_sender,
                 (unsigned)strlen(s_part.text));
    } else {
        ESP_LOGI(TAG, "SMS decoded from '%s', text_len=%u",
                 masked_sender, (unsigned)strlen(s_part.text));
    }

    esp_err_t result = process_sms_part(&s_part, &s_sms);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Complete SMS assembled: Sender='%s', content_len=%u",
                 log_mask_phone(s_sms.sender, masked_sender, sizeof(masked_sender)),
                 (unsigned)strlen(s_sms.content));
        queue_complete_sms(&s_sms);
    } else if (result == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SMS part stored, waiting for more parts...");
    } else {
        ESP_LOGE(TAG, "Failed to process SMS part");
    }
    return result;
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
                ESP_LOGI(TAG, "IMSI read successfully (%s)", LOG_REDACTED_VALUE);
                return ESP_OK;
            }
        }
    }
    ESP_LOGE(TAG, "Failed to parse IMSI from response (response_len=%u)",
             (unsigned)strlen(response));
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
        ESP_LOGW(TAG, "IMSI too short to determine operator (len=%u)",
                 (unsigned)strlen(imsi));
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

static esp_err_t configure_modem_for_sms(char *response_buffer, size_t buffer_size,
                                         bool *modem_responding) {
    esp_err_t at_result = ESP_FAIL;

    *modem_responding = false;

    // Keep the short startup probe, but let the caller decide how to recover.
    for (int retry_count = 0; retry_count < AT_PROBE_MAX_RETRIES; retry_count++) {
        if (retry_count > 0) {
            ESP_LOGW(TAG, "Retrying AT command (%d/%d)...",
                     retry_count + 1, AT_PROBE_MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        at_result = at_send_command("AT", response_buffer, buffer_size,
                                    pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS));
        if (at_result == ESP_OK) {
            break;
        }
    }

    if (at_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to communicate with 4G modem after %d attempts.",
                 AT_PROBE_MAX_RETRIES);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AT command successful, modem is responding.");
    *modem_responding = true;
    vTaskDelay(pdMS_TO_TICKS(500));

    // Non-critical setup commands remain best effort.
    if (at_send_command("ATE0", response_buffer, buffer_size,
                        pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable AT command echo. Parsing might be more complex.");
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    if (at_send_command("AT+CMEE=2", response_buffer, buffer_size,
                        pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable verbose modem errors (AT+CMEE=2). Continuing with default error reporting.");
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // SMS mode and new-message indications are required before declaring ready.
    // PDU模式:短信由本地sms_pdu解码,不经模组的文本模式转换(部分固件转换有缺陷)。
    if (at_send_command("AT+CMGF=0", response_buffer, buffer_size,
                        pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set SMS to PDU mode (AT+CMGF=0).");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // PDU模式下字符集不影响短信正文,但模组可能保留上次的UCS2设置,
    // 那会让AT+CIMI返回hex而识别不出运营商,所以显式设回IRA。
    if (at_send_command("AT+CSCS=\"IRA\"", response_buffer, buffer_size,
                        pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set character set to IRA. Operator detection might fail.");
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // 27.005规定phase 2+下直投的短信必须由TE用AT+CNMA确认。声明phase 1让模组自己
    // 向网络回RP-ACK,省掉这个往返。
    s_cnma_required = true;
    if (at_send_command("AT+CSMS=0", response_buffer, buffer_size,
                        pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to select SMS phase 1 (AT+CSMS=0). Relying on AT+CNMA instead.");
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    // 回读实际生效的phase。AT+CSMS=0的响应只报告支持能力(+CSMS: <mt>,<mo>,<bm>),
    // 当前service要用查询形式才拿得到(+CSMS: <service>,<mt>,<mo>,<bm>)。
    // phase 1下CNMA不适用,而部分固件(Air724UG)对不适用的命令是静默不响应,
    // 照发会让每条短信白等一次超时。
    if (at_send_command("AT+CSMS?", response_buffer, buffer_size,
                        pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) == ESP_OK) {
        const char *csms = strstr(response_buffer, "+CSMS:");
        int service = -1;
        if (csms != NULL && sscanf(csms, "+CSMS: %d", &service) == 1) {
            s_cnma_required = (service != 0);
            ESP_LOGI(TAG, "Modem SMS service phase: %d (direct SMS ack %s)",
                     service, s_cnma_required ? "required" : "not needed");
        } else {
            ESP_LOGW(TAG, "Could not parse AT+CSMS? response; will acknowledge direct SMS.");
        }
    } else {
        ESP_LOGW(TAG, "AT+CSMS? failed; will acknowledge direct SMS.");
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    if (at_send_command("AT+CNMI=2,2,0,0,0", response_buffer, buffer_size,
                        pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure new SMS indications (AT+CNMI).");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "4G modem initialized for SMS reception.");
    vTaskDelay(pdMS_TO_TICKS(500));

    if (get_sim_operator_name(g_sim_operator, sizeof(g_sim_operator)) != ESP_OK) {
        ESP_LOGW(TAG, "Could not determine SIM operator.");
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    return ESP_OK;
}


/**
 * @brief 确认上一条直投短信
 *
 * 27.005: <mt>=2 直投给TE的短信若得不到确认,模组会向网络回RP-ERROR,并把<mt>和<ds>
 * 自动复位为0——此后所有短信都不再送到串口,而是被SMSC扣住重投,表现为"整段时间一条
 * 都收不到,把SIM插回手机才一次性全到"。
 *
 * 只在phase 2+下才发:phase 1的模组要么回+CMS ERROR: 340,要么(如Air724UG)干脆不响应,
 * 后者会让每条短信都白等一次AT_CNMA_TIMEOUT_MS。
 */
static void ack_direct_sms_if_pending(void) {
    static char ack_response[128];

    if (!s_cmt_ack_pending) {
        return;
    }
    s_cmt_ack_pending = false;

    if (!s_cnma_required) {
        return; // phase 1: 模组已自行向网络回RP-ACK
    }

    if (at_send_command("AT+CNMA=0", ack_response, sizeof(ack_response),
                        pdMS_TO_TICKS(AT_CNMA_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGD(TAG, "AT+CNMA=0 rejected (no acknowledgement expected, harmless)");
    } else {
        ESP_LOGD(TAG, "Direct SMS acknowledged to the network");
    }
}

/**
 * @brief 读取+CMTI通知过的存储短信,读完即删
 *
 * 删除不是可选项:存储写满后模组会告诉网络"内存已满",SMSC随即停止投递所有短信。
 */
static void read_pending_stored_sms(void) {
    static char response[AT_RESPONSE_MAX_LEN];
    char cmd[32];

    while (s_pending_read_count > 0) {
        // 读取存储期间也可能来直投短信,别让确认排在整批读完之后
        ack_direct_sms_if_pending();

        int index = s_pending_read_index[0];
        for (int i = 1; i < s_pending_read_count; i++) {
            s_pending_read_index[i - 1] = s_pending_read_index[i];
        }
        s_pending_read_count--;

        snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
        if (at_send_command(cmd, response, sizeof(response),
                            pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read stored SMS at index %d", index);
            continue;
        }

        // 解码失败也要删:否则这一格永远占着,最终把存储堵满
        if (extract_and_dispatch_pdu(response, "+CMGR:") == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to decode stored SMS at index %d, deleting it anyway", index);
        }

        snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
        if (at_send_command(cmd, response, sizeof(response),
                            pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to delete stored SMS at index %d", index);
        }
    }
}

/**
 * @brief 启动时取出并清空模组存储里积压的短信
 *
 * 存储被写满后SMSC会停投,清空是让网络恢复投递的前提。响应可能超过
 * AT_RESPONSE_MAX_LEN被截断,所以分多轮列举:每轮处理完就删除,下一轮重新列举。
 */
static void drain_stored_sms(void) {
    static char response[AT_RESPONSE_MAX_LEN];
    int indices[SMS_PENDING_READ_MAX];
    char cmd[32];
    int drained = 0;

    for (int round = 0; round < SMS_DRAIN_MAX_ROUNDS; round++) {
        // 清空存储可能耗时不短,期间到达的直投短信要及时确认
        ack_direct_sms_if_pending();

        // PDU模式下<stat>=4表示列出全部状态的短信
        if (at_send_command("AT+CMGL=4", response, sizeof(response),
                            pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
            ESP_LOGW(TAG, "AT+CMGL=4 failed, skipping stored SMS drain.");
            return;
        }

        int count = 0;
        const char *p = response;
        while (count < SMS_PENDING_READ_MAX && (p = strstr(p, "+CMGL:")) != NULL) {
            // 头行和PDU行都到齐才算一条完整条目,残缺的留给下一轮
            const char *first_crlf = strstr(p, "\r\n");
            if (first_crlf == NULL) break;
            const char *second_crlf = strstr(first_crlf + 2, "\r\n");
            if (second_crlf == NULL) break;

            int index = -1;
            if (sscanf(p, "+CMGL: %d", &index) != 1) break;

            extract_and_dispatch_pdu(p, "+CMGL:");
            indices[count++] = index;
            p = second_crlf + 2;
        }

        if (count == 0) {
            if (drained > 0) {
                ESP_LOGI(TAG, "Drained %d stored SMS from modem storage", drained);
            } else {
                ESP_LOGI(TAG, "No stored SMS in modem storage");
            }
            return;
        }

        for (int i = 0; i < count; i++) {
            snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", indices[i]);
            if (at_send_command(cmd, response, sizeof(response),
                                pdMS_TO_TICKS(AT_COMMAND_TIMEOUT_MS)) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to delete stored SMS at index %d", indices[i]);
            }
        }
        drained += count;
    }

    ESP_LOGW(TAG, "Stored SMS drain stopped after %d rounds (%d drained)",
             SMS_DRAIN_MAX_ROUNDS, drained);
}

void uart_at_task(void *pvParameters) {
    char response_buffer[AT_RESPONSE_MAX_LEN];
    static const uint32_t recovery_delays_ms[] = {5000, 10000, 20000, 30000};
    size_t recovery_delay_index = 0;

    // Register this task's handle for cleanup on restart
    s_uart_at_task_handle = xTaskGetCurrentTaskHandle();

    // Initialize 4G Cat.1 modem
    ESP_LOGI(TAG, "Initializing 4G Cat.1 modem...");
    vTaskDelay(pdMS_TO_TICKS(120000)); // Give modem more time to boot (120 seconds)

    // A modem that survived an ESP32 restart may already be reporting SMS.
    // Drain those reports before sending wake-up or validation commands.
    process_pending_sms_urcs();

    // Send wake-up sequence to ensure modem is responsive
    // This is important when ESP32 restarts but modem is still running
    ESP_LOGI(TAG, "Sending wake-up sequence to modem...");
    for (int i = 0; i < 3; i++) {
        uart_write_bytes(UART_PORT_NUM, "AT\r\n", 4);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    process_pending_sms_urcs();
    ESP_LOGI(TAG, "Wake-up sequence complete, modem should be responsive.");

    bool modem_responding = false;
    while (configure_modem_for_sms(response_buffer, sizeof(response_buffer),
                                   &modem_responding) != ESP_OK) {
        if (modem_responding) {
            // Communication recovered; restart the backoff for any remaining
            // SMS configuration failure.
            recovery_delay_index = 0;
        }

        uint32_t retry_delay_ms = recovery_delays_ms[recovery_delay_index];
        ESP_LOGW(TAG,
                 "SMS modem is not ready; UART task remains active and will retry in %lu seconds.",
                 (unsigned long)(retry_delay_ms / 1000));
        wait_for_recovery_retry(pdMS_TO_TICKS(retry_delay_ms));

        if (!modem_responding &&
            recovery_delay_index <
            (sizeof(recovery_delays_ms) / sizeof(recovery_delays_ms[0])) - 1) {
            recovery_delay_index++;
        }
    }

    process_pending_sms_urcs();
    ESP_LOGI(TAG, "4G modem initialization complete. Operator: %s", g_sim_operator);

    // 取走上次运行期间被存进模组、从未投递出去的短信,并腾空存储
    drain_stored_sms();

    ESP_LOGI(TAG, "Publishing device ready message to MQTT...");
    if (mqtt_manager_publish_device_ready(g_sim_operator) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish device ready message.");
    }

    // Main loop to listen for incoming URCs (like +CMT:)
    while (1) {
        // Wait for a URC to be signaled by the uart_event_task. The finite
        // timeout also drives the concat buffer timeout check below.
        EventBits_t uxBits = xEventGroupWaitBits(s_at_response_event_group,
                                                 AT_RESPONSE_URC_BIT,
                                                 pdTRUE, // Clear bit on exit
                                                 pdFALSE, // Don't wait for all bits
                                                 pdMS_TO_TICKS(1000));

        if (uxBits & AT_RESPONSE_URC_BIT) {
            process_pending_sms_urcs();
        }

        // 网络等待RP-ACK的时间只有几秒,确认必须紧跟在URC处理之后
        ack_direct_sms_if_pending();
        read_pending_stored_sms();

        // 剩余分段迟迟不到时的兜底:投递已累积的内容,绝不静默丢弃
        if (is_concat_timeout()) {
            flush_concat_buffer_to_queue("part timeout");
        }
    }
}

/**
 * @brief 记下一条待读取的存储短信索引,回到主循环再发AT+CMGR
 */
static void push_pending_read(int index) {
    if (s_pending_read_count >= SMS_PENDING_READ_MAX) {
        ESP_LOGW(TAG, "Pending stored-SMS list is full, dropping index %d", index);
        return;
    }
    s_pending_read_index[s_pending_read_count++] = index;
}

/**
 * @brief 处理直投短信URC: +CMT: ,23\r\n0791...\r\n
 * @return 从缓冲区起始算起已消费的字节数,URC不完整时返回0
 */
static int handle_cmt_urc(char *urc_line_buffer, char *cmt_ptr) {
    char *first_crlf = strstr(cmt_ptr, "\r\n");
    if (!first_crlf) {
        return 0;
    }
    char *second_crlf = strstr(first_crlf + 2, "\r\n");
    if (!second_crlf) {
        return 0;
    }

    int urc_len = (second_crlf - cmt_ptr) + 2; // 从+CMT开头到第二个\r\n结束

    // Temporarily null-terminate the URC block for parsing
    char temp_char = cmt_ptr[urc_len];
    cmt_ptr[urc_len] = '\0';

    ESP_LOGI(TAG, "New SMS received (direct URC).");
    if (extract_and_dispatch_pdu(cmt_ptr, "+CMT:") == ESP_FAIL) {
        ESP_LOGW(TAG, "Failed to parse SMS URC.");
    }
    // 无论解码成败都要确认:未确认的直投短信会被模组回RP-ERROR退给网络
    s_cmt_ack_pending = true;

    cmt_ptr[urc_len] = temp_char; // Restore original char
    return (cmt_ptr - urc_line_buffer) + urc_len;
}

/**
 * @brief 处理存储通知URC: +CMTI: "SM",3
 * @return 从缓冲区起始算起已消费的字节数,URC不完整时返回0
 */
static int handle_cmti_urc(char *urc_line_buffer, char *cmti_ptr) {
    char *crlf = strstr(cmti_ptr, "\r\n");
    if (!crlf) {
        return 0;
    }

    int urc_len = (crlf - cmti_ptr) + 2;
    char temp_char = cmti_ptr[urc_len];
    cmti_ptr[urc_len] = '\0';

    char mem[8] = {0};
    int index = -1;
    if (sscanf(cmti_ptr, "+CMTI: \"%7[^\"]\",%d", mem, &index) == 2 ||
        sscanf(cmti_ptr, "+CMTI: %7[^,],%d", mem, &index) == 2) {
        // <mem>只做记录:CMGR/CMGD走当前选中的存储,两者不一致时能从日志看出来
        ESP_LOGI(TAG, "New SMS stored by modem (mem=%s, index=%d), reading it back.", mem, index);
        push_pending_read(index);
    } else {
        ESP_LOGW(TAG, "Failed to parse +CMTI notification.");
    }

    cmti_ptr[urc_len] = temp_char;
    return (cmti_ptr - urc_line_buffer) + urc_len;
}

static int handle_urc(char *urc_line_buffer) { // Now takes a mutable buffer
    char *cmt_ptr = strstr(urc_line_buffer, "+CMT:");
    char *cmti_ptr = strstr(urc_line_buffer, "+CMTI:");

    // 缓冲区里可能同时躺着两种URC,先处理靠前的那条
    if (cmt_ptr && (!cmti_ptr || cmt_ptr < cmti_ptr)) {
        return handle_cmt_urc(urc_line_buffer, cmt_ptr);
    }
    if (cmti_ptr) {
        return handle_cmti_urc(urc_line_buffer, cmti_ptr);
    }
    // Add other URC handlers here if needed (e.g., +CGATT, +CPIN)
    return 0; // No complete URC processed
}

// ==================== 长短信重组相关函数实现 ====================

/**
 * @brief 检查重组缓冲区是否超时
 * @return true if timeout, false otherwise
 */
static bool is_concat_timeout(void) {
    if (!s_concat.active) {
        return false;
    }

    TickType_t current_time = xTaskGetTickCount();
    TickType_t elapsed = current_time - s_concat.last_part_time;

    // 转换为毫秒并检查是否超时
    if ((elapsed * portTICK_PERIOD_MS) > SMS_CONCAT_TIMEOUT_MS) {
        char masked_sender[LOG_MASKED_PHONE_SIZE];
        ESP_LOGW(TAG, "SMS concat buffer timeout after %d ms, flushing %d of %u part(s) from sender '%s'",
                 (int)(elapsed * portTICK_PERIOD_MS), __builtin_popcount(s_concat.received_mask),
                 s_concat.total,
                 log_mask_phone(s_concat.sender, masked_sender, sizeof(masked_sender)));
        return true;
    }
    return false;
}

/**
 * @brief 重置重组缓冲区
 */
static void reset_concat_buffer(void) {
    memset(&s_concat, 0, sizeof(s_concat));
    s_concat.active = false;
    ESP_LOGD(TAG, "SMS concat buffer reset");
}

/**
 * @brief 把缓冲区中已收到的分段按序号顺序拼成一条消息
 *
 * 缺失的分段直接跳过(超时冲刷时可能不全),内容超出content容量时截断,
 * 并裁掉截断处残缺的UTF-8序列。
 */
static void assemble_concat_buffer(sms_message_t *sms) {
    memset(sms, 0, sizeof(*sms));
    strncpy(sms->sender, s_concat.sender, sizeof(sms->sender) - 1);

    size_t idx = 0;
    for (uint8_t i = 0; i < s_concat.total && i < SMS_CONCAT_MAX_PARTS; i++) {
        if ((s_concat.received_mask & (1u << i)) == 0) {
            continue;
        }
        size_t part_len = strlen(s_concat.parts[i]);
        size_t space = sizeof(sms->content) - 1 - idx;
        if (part_len > space) {
            ESP_LOGW(TAG, "Concatenated SMS exceeds content buffer, truncating");
            part_len = space;
        }
        memcpy(sms->content + idx, s_concat.parts[i], part_len);
        idx += part_len;
        if (idx >= sizeof(sms->content) - 1) {
            break;
        }
    }
    sms->content[idx] = '\0';
    sms_pdu_utf8_trim_tail(sms->content, idx);
}

/**
 * @brief 把缓冲区中已收到的分段组装成消息并投递到SMS队列,然后重置缓冲区
 *
 * 用于剩余分段始终未到达的情况:宁可投递可能不完整的内容,也不静默丢弃。
 *
 * @param reason 冲刷原因,仅用于日志
 */
static void flush_concat_buffer_to_queue(const char *reason) {
    // 重组逻辑只在uart_at_task单线程内执行,static避免在深调用链上再压2KB栈
    static sms_message_t flushed_sms;

    if (!s_concat.active) {
        return;
    }
    if (s_concat.received_mask == 0) {
        ESP_LOGD(TAG, "Concat buffer has no parts to flush (%s)", reason);
        reset_concat_buffer();
        return;
    }

    assemble_concat_buffer(&flushed_sms);
    int part_count = __builtin_popcount(s_concat.received_mask);

    reset_concat_buffer();

    if (s_sms_queue != NULL) {
        if (xQueueSend(s_sms_queue, &flushed_sms, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to send flushed SMS to queue (%s).", reason);
        } else {
            ESP_LOGI(TAG, "Flushed %d pending SMS part(s) to processing queue (%s).",
                     part_count, reason);
        }
    }
}

/**
 * @brief 用一条已解码的短信填充sms_message_t(单段短信直接完成)
 */
static void fill_message_from_part(const sms_pdu_t *part, sms_message_t *sms) {
    memset(sms, 0, sizeof(*sms));
    strncpy(sms->sender, part->sender, sizeof(sms->sender) - 1);
    strncpy(sms->content, part->text, sizeof(sms->content) - 1);
}

/**
 * @brief 处理一段已解码的短信,按UDH拼接信息累积并在集齐时返回完整消息
 *
 * @param part 已解码的分段
 * @param complete_sms 输出参数:完整的SMS消息(仅在返回ESP_OK时有效)
 * @return ESP_OK 如果完整SMS已组装完成
 *         ESP_ERR_INVALID_STATE 如果这是一个分段,需要等待更多分段
 *         ESP_FAIL 如果处理失败
 */
static esp_err_t process_sms_part(const sms_pdu_t *part, sms_message_t *complete_sms) {
    if (!part || !complete_sms) {
        return ESP_FAIL;
    }
    char masked_sender[LOG_MASKED_PHONE_SIZE];
    log_mask_phone(part->sender, masked_sender, sizeof(masked_sender));

    // 检查是否超时,如果超时则先投递已累积的内容
    if (is_concat_timeout()) {
        flush_concat_buffer_to_queue("part timeout");
    }

    // 单段短信,或分段信息不合法时按单段处理
    if (!part->concat || part->total <= 1) {
        ESP_LOGD(TAG, "Processing standalone SMS from '%s'", masked_sender);
        fill_message_from_part(part, complete_sms);
        return ESP_OK;
    }
    if (part->seq < 1 || part->seq > part->total) {
        ESP_LOGW(TAG, "Invalid concat sequence %u/%u from '%s', treating as standalone SMS",
                 part->seq, part->total, masked_sender);
        fill_message_from_part(part, complete_sms);
        return ESP_OK;
    }

    // 换了发件人或换了参考号,说明上一条长短信不会再有后续分段了
    if (s_concat.active &&
        (s_concat.ref != part->ref || strcmp(s_concat.sender, part->sender) != 0)) {
        char masked_previous_sender[LOG_MASKED_PHONE_SIZE];
        ESP_LOGW(TAG, "New message (ref=%u) from '%s' while parts of ref=%u from '%s' are pending. Flushing old parts.",
                 part->ref, masked_sender, s_concat.ref,
                 log_mask_phone(s_concat.sender, masked_previous_sender,
                                sizeof(masked_previous_sender)));
        flush_concat_buffer_to_queue("new message started");
    }

    // 超出上限的分段直接丢弃。必须在建组之前判断,否则会留下一个空分组,
    // 等它超时被冲刷时会投递出一条空短信。
    if (part->seq > SMS_CONCAT_MAX_PARTS) {
        ESP_LOGW(TAG, "Dropping part %u/%u beyond the %d-part limit",
                 part->seq, part->total, SMS_CONCAT_MAX_PARTS);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_concat.active) {
        reset_concat_buffer();
        strncpy(s_concat.sender, part->sender, sizeof(s_concat.sender) - 1);
        s_concat.ref = part->ref;
        s_concat.total = part->total;
        if (part->total > SMS_CONCAT_MAX_PARTS) {
            ESP_LOGW(TAG, "SMS has %u parts, only the first %d can be assembled",
                     part->total, SMS_CONCAT_MAX_PARTS);
            s_concat.total = SMS_CONCAT_MAX_PARTS;
        }
        s_concat.active = true;
        ESP_LOGI(TAG, "Starting SMS assembly from '%s' (ref=%u, %u parts)",
                 masked_sender, s_concat.ref, s_concat.total);
    }

    strncpy(s_concat.parts[part->seq - 1], part->text, SMS_PDU_TEXT_MAX - 1);
    s_concat.parts[part->seq - 1][SMS_PDU_TEXT_MAX - 1] = '\0';
    s_concat.received_mask |= 1u << (part->seq - 1);
    s_concat.last_part_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "Stored SMS part %u/%u from '%s' (%d of %u received)",
             part->seq, s_concat.total, masked_sender,
             __builtin_popcount(s_concat.received_mask), s_concat.total);

    uint32_t complete_mask = (1u << s_concat.total) - 1;
    if ((s_concat.received_mask & complete_mask) == complete_mask) {
        ESP_LOGI(TAG, "All %u parts received, assembling message", s_concat.total);
        assemble_concat_buffer(complete_sms);
        reset_concat_buffer();
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE; // 需要等待更多分段
}
