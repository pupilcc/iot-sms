#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "uart_dtu_manager.h"
#include "mqtt_manager.h"

// Configuration from Kconfig (与AT版共用同一组UART配置)
#define UART_PORT_NUM      CONFIG_APP_UART_PORT_NUM
#define UART_TXD           CONFIG_APP_UART_TXD
#define UART_RXD           CONFIG_APP_UART_RXD
#define UART_BAUD_RATE     CONFIG_APP_UART_BAUD_RATE

// content[2048]的UTF-8 hex最长约4094字符,加号码和"config,sms,ok,"前缀留余量
#define DTU_LINE_BUF_SIZE        4608
#define DTU_SMS_POLL_INTERVAL_MS 10000 // 轮询读取DTU缓存短信的间隔
#define DTU_RX_CHUNK_TIMEOUT_MS  100   // 单次uart_read_bytes的等待时间
#define DTU_CMD_RESPONSE_TIMEOUT_MS 5000

static const char *TAG = "uart_dtu_manager";
static QueueHandle_t s_sms_queue = NULL;

// 行累积缓冲(仅在uart_dtu_task上下文中使用)
static char s_rx_acc[DTU_LINE_BUF_SIZE];
static int s_rx_acc_len = 0;

// 行读取缓冲,dtu_query与主循环共用(同一任务上下文,无并发)
// 必须是全尺寸:查询期间到达的短信上报行也经此缓冲,截断会导致短信丢失
static char s_line_buf[DTU_LINE_BUF_SIZE];

// DTU固件没有AT通道查IMSI,改用ICCID前缀识别运营商
typedef struct {
    const char *iccid_prefix;
    const char *operator_name_zh;
} iccid_operator_map_t;

static const iccid_operator_map_t s_iccid_operator_map[] = {
    {"898600", "中国移动"},
    {"898602", "中国移动"},
    {"898604", "中国移动"},
    {"898607", "中国移动"},
    {"898608", "中国移动"},
    {"898601", "中国联通"},
    {"898606", "中国联通"},
    {"898609", "中国联通"},
    {"898603", "中国电信"},
    {"898611", "中国电信"},
    {"898615", "中国广电"},
    {"896405", "Skinny"},  // 新西兰 Skinny(Spark旗下MVNO)
};
static const size_t s_iccid_operator_map_size = sizeof(s_iccid_operator_map) / sizeof(s_iccid_operator_map[0]);

// 发送一条DTU配置命令,自动追加\r\n
static void dtu_send_command(const char *cmd) {
    ESP_LOGD(TAG, "DTU cmd: %s", cmd);
    uart_write_bytes(UART_PORT_NUM, cmd, strlen(cmd));
    uart_write_bytes(UART_PORT_NUM, "\r\n", 2);
}

// 从UART读出一行(去掉\r\n)。返回true表示取到一行,false表示超时
static bool dtu_read_line(char *line, size_t line_size, TickType_t timeout_ticks) {
    TickType_t start = xTaskGetTickCount();
    while (1) {
        char *nl = memchr(s_rx_acc, '\n', s_rx_acc_len);
        if (nl) {
            int line_len = nl - s_rx_acc;
            int copy_len = line_len;
            if (copy_len > 0 && s_rx_acc[copy_len - 1] == '\r') {
                copy_len--;
            }
            if (copy_len >= (int)line_size) {
                ESP_LOGW(TAG, "RX line truncated from %d to %d bytes", copy_len, (int)line_size - 1);
                copy_len = line_size - 1;
            }
            memcpy(line, s_rx_acc, copy_len);
            line[copy_len] = '\0';
            int remain = s_rx_acc_len - (line_len + 1);
            memmove(s_rx_acc, nl + 1, remain);
            s_rx_acc_len = remain;
            return true;
        }

        if (xTaskGetTickCount() - start >= timeout_ticks) {
            return false;
        }

        int space = DTU_LINE_BUF_SIZE - s_rx_acc_len;
        if (space <= 0) {
            // 无换行的超长数据(如误开的透传通道数据),丢弃防止卡死
            ESP_LOGW(TAG, "RX buffer full without newline, dropping %d bytes", s_rx_acc_len);
            s_rx_acc_len = 0;
            space = DTU_LINE_BUF_SIZE;
        }
        int len = uart_read_bytes(UART_PORT_NUM, (uint8_t *)s_rx_acc + s_rx_acc_len, space,
                                  pdMS_TO_TICKS(DTU_RX_CHUNK_TIMEOUT_MS));
        if (len > 0) {
            s_rx_acc_len += len;
        }
    }
}

static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// 截断处若正好切在UTF-8多字节字符中间,去掉末尾不完整的序列,返回新长度
static size_t utf8_trim_incomplete_tail(char *buf, size_t len) {
    size_t end = len;
    int back = 0;
    while (end > 0 && back < 3 && ((unsigned char)buf[end - 1] & 0xC0) == 0x80) {
        end--;
        back++;
    }
    if (end == 0) {
        return len; // 找不到lead字节,数据本身非法,原样保留
    }
    unsigned char lead = (unsigned char)buf[end - 1];
    int seq_len = 1;
    if ((lead & 0xE0) == 0xC0) seq_len = 2;
    else if ((lead & 0xF0) == 0xE0) seq_len = 3;
    else if ((lead & 0xF8) == 0xF0) seq_len = 4;
    if (back + 1 < seq_len) {
        buf[end - 1] = '\0';
        return end - 1;
    }
    return len;
}

// 银尔达DTU固件短信内容为UTF-8的hex编码,解码后即为UTF-8字符串
// 返回解码后的字节数,失败返回-1
static int decode_utf8_hex(const char *hex, char *out, size_t out_size) {
    size_t hex_len = strlen(hex);
    if (hex_len == 0 || hex_len % 2 != 0) {
        return -1;
    }
    size_t byte_len = hex_len / 2;
    bool truncated = false;
    if (byte_len >= out_size) {
        ESP_LOGW(TAG, "SMS content too long (%d bytes), truncating to %d", (int)byte_len, (int)out_size - 1);
        byte_len = out_size - 1;
        truncated = true;
    }
    for (size_t i = 0; i < byte_len; i++) {
        int hi = hex_char_to_int(hex[i * 2]);
        int lo = hex_char_to_int(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (char)((hi << 4) | lo);
    }
    out[byte_len] = '\0';
    if (truncated) {
        byte_len = utf8_trim_incomplete_tail(out, byte_len);
    }
    return byte_len;
}

// 解析短信行: config,sms,ok,<号码>,<UTF-8内容hex>
// 注意发送短信的响应是 config,sms,ok,<0/1> (无内容字段),需排除
static bool parse_sms_line(const char *line, sms_message_t *sms) {
    static const char prefix[] = "config,sms,ok,";
    if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *sender_start = line + sizeof(prefix) - 1;
    const char *comma = strchr(sender_start, ',');
    if (comma == NULL) {
        return false; // 无内容字段: 是发送响应或空缓存返回
    }
    size_t sender_len = comma - sender_start;
    if (sender_len == 0 || sender_len >= sizeof(sms->sender)) {
        return false;
    }

    memset(sms, 0, sizeof(sms_message_t));
    memcpy(sms->sender, sender_start, sender_len);
    sms->sender[sender_len] = '\0';

    if (decode_utf8_hex(comma + 1, sms->content, sizeof(sms->content)) < 0) {
        ESP_LOGW(TAG, "Failed to decode SMS content hex: %.64s...", comma + 1);
        return false;
    }
    return true;
}

// 去重: 主动上报与轮询config,get,sms可能重复送达同一条短信,
// 且轮询若不清除DTU缓存会反复读到同一条。窗口内相同(号码+内容)只入队一次
#define DTU_DEDUP_CACHE_SIZE 8
#define DTU_DEDUP_WINDOW_MS  (10 * 60 * 1000)

typedef struct {
    uint32_t hash;
    TickType_t tick;
    bool valid;
} sms_dedup_entry_t;

static sms_dedup_entry_t s_dedup_cache[DTU_DEDUP_CACHE_SIZE];
static int s_dedup_next = 0;

// FNV-1a hash of sender + content
static uint32_t sms_fingerprint(const sms_message_t *sms) {
    uint32_t h = 2166136261u;
    for (const char *p = sms->sender; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    h ^= (uint8_t)','; h *= 16777619u;
    for (const char *p = sms->content; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    return h;
}

// 已见过返回true;命中时刷新时间戳,保证缓存被反复读到时持续抑制
static bool sms_is_duplicate(const sms_message_t *sms) {
    uint32_t h = sms_fingerprint(sms);
    TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < DTU_DEDUP_CACHE_SIZE; i++) {
        if (s_dedup_cache[i].valid && s_dedup_cache[i].hash == h &&
            now - s_dedup_cache[i].tick < pdMS_TO_TICKS(DTU_DEDUP_WINDOW_MS)) {
            s_dedup_cache[i].tick = now;
            return true;
        }
    }
    return false;
}

// 入队成功后调用;队列满丢弃的短信不记录,以便后续轮询补送
static void sms_dedup_record(const sms_message_t *sms) {
    s_dedup_cache[s_dedup_next].hash = sms_fingerprint(sms);
    s_dedup_cache[s_dedup_next].tick = xTaskGetTickCount();
    s_dedup_cache[s_dedup_next].valid = true;
    s_dedup_next = (s_dedup_next + 1) % DTU_DEDUP_CACHE_SIZE;
}

// 处理一行DTU输出: 是短信则去重后入队,其余仅记录日志
static void dtu_handle_line(const char *line) {
    static sms_message_t sms; // 仅在uart_dtu_task上下文中使用,static避免占用任务栈
    if (line[0] == '\0') {
        return;
    }
    ESP_LOGD(TAG, "DTU line: %s", line);

    if (parse_sms_line(line, &sms)) {
        if (sms_is_duplicate(&sms)) {
            ESP_LOGD(TAG, "Duplicate SMS from %s ignored", sms.sender);
            return;
        }
        ESP_LOGI(TAG, "SMS received from %s: %s", sms.sender, sms.content);
        if (xQueueSend(s_sms_queue, &sms, pdMS_TO_TICKS(1000)) == pdPASS) {
            sms_dedup_record(&sms);
        } else {
            ESP_LOGE(TAG, "SMS queue full, message from %s dropped", sms.sender);
        }
    }
}

// 发送命令并等待以resp_prefix开头的响应行,其余行交给dtu_handle_line
// 读取走全尺寸s_line_buf,保证等待期间到达的短信上报行不被截断
static bool dtu_query(const char *cmd, const char *resp_prefix, char *resp, size_t resp_size) {
    dtu_send_command(cmd);
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(DTU_CMD_RESPONSE_TIMEOUT_MS);
    TickType_t elapsed;
    while ((elapsed = xTaskGetTickCount() - start) < timeout) {
        if (!dtu_read_line(s_line_buf, sizeof(s_line_buf), timeout - elapsed)) {
            break;
        }
        if (strncmp(s_line_buf, resp_prefix, strlen(resp_prefix)) == 0) {
            snprintf(resp, resp_size, "%s", s_line_buf);
            return true;
        }
        dtu_handle_line(s_line_buf);
    }
    ESP_LOGW(TAG, "No response for command: %s", cmd);
    return false;
}

// 通过 config,get,iccid 获取ICCID并识别运营商,结果写入g_sim_operator
static void detect_operator_from_iccid(void) {
    char resp[128];
    // 响应格式: config,iccid,ok,<iccid>
    if (!dtu_query("config,get,iccid", "config,iccid,ok,", resp, sizeof(resp))) {
        ESP_LOGW(TAG, "Failed to get ICCID, operator unknown");
        return;
    }
    const char *iccid = resp + strlen("config,iccid,ok,");
    ESP_LOGI(TAG, "ICCID: %s", iccid);
    for (size_t i = 0; i < s_iccid_operator_map_size; i++) {
        if (strncmp(iccid, s_iccid_operator_map[i].iccid_prefix, strlen(s_iccid_operator_map[i].iccid_prefix)) == 0) {
            strncpy(g_sim_operator, s_iccid_operator_map[i].operator_name_zh, sizeof(g_sim_operator) - 1);
            ESP_LOGI(TAG, "Operator: %s", g_sim_operator);
            return;
        }
    }
    ESP_LOGW(TAG, "Unknown ICCID prefix, operator unknown");
}

esp_err_t uart_dtu_init(QueueHandle_t sms_queue) {
    s_sms_queue = sms_queue;

    // Clean up existing UART driver if already installed (important for device restarts)
    if (uart_is_driver_installed(UART_PORT_NUM)) {
        ESP_LOGI(TAG, "UART driver already installed on port %d, uninstalling first...", UART_PORT_NUM);
        uart_driver_delete(UART_PORT_NUM);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    s_rx_acc_len = 0;

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

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, DTU_LINE_BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TXD, UART_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART DTU manager initialized on port %d, TX:%d, RX:%d, Baud:%d",
             UART_PORT_NUM, UART_TXD, UART_RXD, UART_BAUD_RATE);

    return ESP_OK;
}

void uart_dtu_task(void *pvParameters) {
    char resp[128];

    // 等待DTU模块开机就绪,丢弃开机期间的输出
    vTaskDelay(pdMS_TO_TICKS(3000));
    uart_flush_input(UART_PORT_NUM);

    // 探测通信: 查询固件版本,重试3次
    bool dtu_ready = false;
    for (int i = 0; i < 3; i++) {
        // 响应格式: config,firmwarever,ok,<version>
        if (dtu_query("config,get,firmwarever", "config,firmwarever,", resp, sizeof(resp))) {
            ESP_LOGI(TAG, "DTU firmware: %s", resp);
            dtu_ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (!dtu_ready) {
        ESP_LOGE(TAG, "DTU module not responding, will keep listening for SMS reports anyway");
    }

    detect_operator_from_iccid();

    // 开启短信功能: 开启/不过滤/不限号码/默认上报格式/通道1/开启缓存 (银尔达手册示例值)
    if (dtu_query("config,set,smson,1,0,0,0,0,1,1", "config,smson,", resp, sizeof(resp))) {
        ESP_LOGI(TAG, "SMS function enabled: %s", resp);
    } else {
        ESP_LOGE(TAG, "Failed to enable DTU SMS function");
    }

    ESP_LOGI(TAG, "DTU initialization complete. Operator: %s",
             strlen(g_sim_operator) > 0 ? g_sim_operator : "UNKNOWN");

    if (mqtt_manager_publish_device_ready(g_sim_operator) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish device ready message (MQTT may not be connected yet)");
    }

    // 主循环: 接收DTU主动上报的短信,并定期轮询读取缓存短信
    TickType_t last_poll = xTaskGetTickCount();
    while (1) {
        if (dtu_read_line(s_line_buf, sizeof(s_line_buf), pdMS_TO_TICKS(1000))) {
            dtu_handle_line(s_line_buf);
        }
        if (xTaskGetTickCount() - last_poll >= pdMS_TO_TICKS(DTU_SMS_POLL_INTERVAL_MS)) {
            last_poll = xTaskGetTickCount();
            // 读取缓存短信,响应格式: config,sms,ok,<号码>,<内容hex>,在主循环中解析
            dtu_send_command("config,get,sms");
        }
    }
}
