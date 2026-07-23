#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "sdkconfig.h"

#include "remote_log.h"
#include "mqtt_manager.h"
#include "log_redaction.h"

#if CONFIG_APP_REMOTE_LOG_ENABLE

static const char *TAG = "remote_log";

#define RL_LINE_MAX        256   // 单行最大长度（超出截断）
#define RL_RINGBUF_SIZE    8192  // 环形缓冲大小（约可缓存 70 行开机日志）
#define RL_BATCH_MAX_LINES 40    // 单批最大行数
#define RL_BATCH_MAX_BYTES 3800  // 批量缓冲刷新阈值
#define RL_FLUSH_MS        2000  // 距首行的最长等待时间
#define RL_BATCH_BUF_SIZE  4096  // 批量 JSON 缓冲大小

static RingbufHandle_t s_log_rb = NULL;
static vprintf_like_t s_orig_vprintf = NULL;
static TaskHandle_t s_fwd_task = NULL;
static _Atomic uint32_t s_dropped = 0;   // 钩子入队失败的累计丢弃行数
static uint32_t s_seq = 0;               // 批次序号，仅转发任务访问
static char s_device_id[32];
static char s_phone[24];
static QueueHandle_t s_sms_queue = NULL;

// esp-mqtt/TLS 栈内部 tag 的 DEBUG/VERBOSE 行不上报：日志发布本身会触发
// 这些 tag 的 DEBUG 输出，上报会形成回环。WARN/ERROR 不受此名单影响。
static const char *s_tag_blocklist[] = {
    "mqtt_client", "outbox", "transport", "transport_base",
    "tcp_transport", "transport_ws", "esp-tls",
};

// 原地去掉 ANSI 颜色码（CONFIG_LOG_COLORS=y）和尾部换行，返回新长度
static int rl_strip_line(char *buf, int len)
{
    int w = 0;
    for (int r = 0; r < len; r++) {
        if (buf[r] == '\x1b') {
            if (r + 1 < len && buf[r + 1] == '[') {
                r += 2;
                while (r < len && (buf[r] < 0x40 || buf[r] > 0x7e)) {
                    r++;
                }
                // for 循环的 r++ 会跳过 CSI 序列的终止字节（如 'm'）
            }
            continue;
        }
        buf[w++] = buf[r];
    }
    while (w > 0 && (buf[w - 1] == '\n' || buf[w - 1] == '\r')) {
        w--;
    }
    buf[w] = '\0';
    return w;
}

// 解析标准前缀 "L (12345) tag: ..."；失败返回 false
static bool rl_parse_prefix(const char *line, char *level, const char **tag,
                            size_t *tag_len, uint32_t *ts_ms)
{
    const char *p = line;
    if (p[0] == '\0' || strchr("EWIDV", p[0]) == NULL ||
        p[1] != ' ' || p[2] != '(') {
        return false;
    }
    char lvl = p[0];
    p += 3;
    if (*p < '0' || *p > '9') {
        return false;
    }
    uint32_t ts = 0;
    while (*p >= '0' && *p <= '9') {
        ts = ts * 10 + (uint32_t)(*p - '0');
        p++;
    }
    if (p[0] != ')' || p[1] != ' ') {
        return false;
    }
    p += 2;
    const char *colon = strchr(p, ':');
    if (colon == NULL || colon == p) {
        return false;
    }
    *level = lvl;
    *tag = p;
    *tag_len = (size_t)(colon - p);
    *ts_ms = ts;
    return true;
}

static int rl_level_num(char level)
{
    switch (level) {
    case 'E': return 1;
    case 'W': return 2;
    case 'I': return 3;
    case 'D': return 4;
    case 'V': return 5;
    default:  return 3;
    }
}

static bool rl_tag_blocked(const char *tag, size_t tag_len)
{
    for (size_t i = 0; i < sizeof(s_tag_blocklist) / sizeof(s_tag_blocklist[0]); i++) {
        if (strlen(s_tag_blocklist[i]) == tag_len &&
            strncmp(s_tag_blocklist[i], tag, tag_len) == 0) {
            return true;
        }
    }
    return false;
}

static int remote_log_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    // 串口输出无条件保持原样（颜色保留）
    int ret = s_orig_vprintf(fmt, args);

    // 递归护栏：转发任务上下文中产生的日志（QoS 0 publish 在调用者上下文
    // 写 socket，esp-mqtt 的 DEBUG 输出会从转发任务发出）只上串口，不再入队
    if (s_log_rb == NULL || xPortInIsrContext() ||
        xTaskGetCurrentTaskHandle() == s_fwd_task) {
        va_end(copy);
        return ret;
    }

    char buf[RL_LINE_MAX];
    int len = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (len <= 0) {
        return ret;
    }
    if (len >= (int)sizeof(buf)) {
        len = (int)sizeof(buf) - 1;
    }

    len = rl_strip_line(buf, len);
    if (len == 0) {
        return ret;
    }

    char level = 'I';
    const char *tag = NULL;
    size_t tag_len = 0;
    uint32_t ts_ms = 0;
    bool parsed = rl_parse_prefix(buf, &level, &tag, &tag_len, &ts_ms);

    int level_num = rl_level_num(parsed ? level : 'I');
    if (level_num > CONFIG_APP_REMOTE_LOG_LEVEL) {
        return ret;
    }
    if (parsed && level_num >= 4 && rl_tag_blocked(tag, tag_len)) {
        return ret;
    }

    // 永不阻塞；缓冲满则丢弃并计数
    if (xRingbufferSend(s_log_rb, buf, (size_t)len + 1, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1);
    }
    return ret;
}

// 将转义后的 src 追加到 dst，空间不足返回 -1
static int rl_json_escape(char *dst, size_t dst_size, const char *src)
{
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (w + 2 >= dst_size) {
                return -1;
            }
            dst[w++] = '\\';
            dst[w++] = (char)*p;
        } else if (*p < 0x20) {
            if (w + 6 >= dst_size) {
                return -1;
            }
            w += (size_t)snprintf(&dst[w], dst_size - w, "\\u%04x", *p);
        } else {
            if (w + 1 >= dst_size) {
                return -1;
            }
            dst[w++] = (char)*p;
        }
    }
    dst[w] = '\0';
    return (int)w;
}

// 追加一行到批量缓冲（批空时先写 header）；空间不足返回 false 且缓冲保持原状
static bool rl_batch_append(char *batch, size_t *len, int *lines, const char *item)
{
    char level = 'I';
    const char *tag = "raw";
    size_t tag_len = 3;
    uint32_t ts_ms = 0;
    const char *ptag = NULL;
    size_t ptag_len = 0;
    if (rl_parse_prefix(item, &level, &ptag, &ptag_len, &ts_ms)) {
        // tag 含引号/反斜杠会破坏 JSON（正常固件 tag 不会出现），降级为 raw
        if (memchr(ptag, '"', ptag_len) == NULL && memchr(ptag, '\\', ptag_len) == NULL) {
            tag = ptag;
            tag_len = ptag_len;
        }
    }

    const size_t avail = RL_BATCH_BUF_SIZE - 3; // 预留结尾 "]}" 和 NUL
    size_t pos = *len;
    size_t save = pos;

    if (*lines == 0) {
        int n = snprintf(batch, avail,
                         "{\"device\":\"%s\",\"phone\":\"%s\",\"seq\":%lu,\"dropped\":%lu,\"lines\":[",
                         s_device_id, s_phone,
                         (unsigned long)s_seq,
                         (unsigned long)atomic_load(&s_dropped));
        if (n < 0 || (size_t)n >= avail) {
            *len = 0;
            return false;
        }
        pos = (size_t)n;
        save = 0;
    }

    int n = snprintf(&batch[pos], avail - pos,
                     "%s{\"level\":\"%c\",\"tag\":\"%.*s\",\"ts_ms\":%lu,\"msg\":\"",
                     *lines > 0 ? "," : "", level, (int)tag_len, tag,
                     (unsigned long)ts_ms);
    if (n < 0 || pos + (size_t)n >= avail) {
        goto fail;
    }
    pos += (size_t)n;

    n = rl_json_escape(&batch[pos], avail - pos, item);
    if (n < 0) {
        goto fail;
    }
    pos += (size_t)n;

    if (pos + 2 >= avail) {
        goto fail;
    }
    batch[pos++] = '"';
    batch[pos++] = '}';
    batch[pos] = '\0';
    *len = pos;
    (*lines)++;
    return true;

fail:
    batch[save] = '\0';
    *len = (*lines == 0) ? 0 : save;
    return false;
}

static void rl_batch_flush(char *batch, size_t *len, int *lines)
{
    if (*lines == 0) {
        return;
    }
    size_t pos = *len;
    batch[pos++] = ']';
    batch[pos++] = '}';
    batch[pos] = '\0';
    // 发布失败即丢弃（QoS 0 语义），接收端可通过 seq 断档发现丢失
    mqtt_manager_publish(CONFIG_APP_MQTT_TOPIC_LOG, batch, (int)pos, 0);
    s_seq++;
    *len = 0;
    *lines = 0;
}

static void rl_publish_metrics(void)
{
    wifi_ap_record_t ap = {0};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    char payload[320];
    int len = snprintf(payload, sizeof(payload),
                       "{\"device\":\"%s\",\"phone\":\"%s\",\"uptime_s\":%lld,"
                       "\"free_heap\":%lu,\"min_free_heap\":%lu,\"rssi_dbm\":%d,"
                       "\"sms_queue_depth\":%u,\"log_dropped_total\":%lu,\"log_seq\":%lu}",
                       s_device_id, s_phone,
                       (long long)(esp_timer_get_time() / 1000000),
                       (unsigned long)esp_get_free_heap_size(),
                       (unsigned long)esp_get_minimum_free_heap_size(),
                       rssi,
                       s_sms_queue ? (unsigned)uxQueueMessagesWaiting(s_sms_queue) : 0,
                       (unsigned long)atomic_load(&s_dropped),
                       (unsigned long)s_seq);
    if (len > 0 && len < (int)sizeof(payload)) {
        mqtt_manager_publish(CONFIG_APP_MQTT_TOPIC_METRICS, payload, len, 0);
    }
}

static void remote_log_task(void *arg)
{
    (void)arg;
    static char batch[RL_BATCH_BUF_SIZE];
    size_t batch_len = 0;
    int batch_lines = 0;
    int64_t first_line_us = 0;
    // 启动 5 秒后发首条指标（等 MQTT 连接），之后按配置间隔
    int64_t next_metrics_us = esp_timer_get_time() + 5 * 1000000LL;

    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now >= next_metrics_us && mqtt_manager_is_connected()) {
            rl_publish_metrics();
            next_metrics_us = now + (int64_t)CONFIG_APP_METRICS_INTERVAL_S * 1000000LL;
        }

        if (!mqtt_manager_is_connected()) {
            // 断连期间不取行：环形缓冲继续积累，钩子满则丢弃并计数
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        size_t item_size = 0;
        char *item = (char *)xRingbufferReceive(s_log_rb, &item_size, pdMS_TO_TICKS(200));
        if (item) {
            if (!rl_batch_append(batch, &batch_len, &batch_lines, item)) {
                rl_batch_flush(batch, &batch_len, &batch_lines);
                if (rl_batch_append(batch, &batch_len, &batch_lines, item) &&
                    batch_lines == 1) {
                    first_line_us = esp_timer_get_time();
                }
            } else if (batch_lines == 1) {
                first_line_us = esp_timer_get_time();
            }
            vRingbufferReturnItem(s_log_rb, item);
        }

        if (batch_lines > 0 &&
            (batch_lines >= RL_BATCH_MAX_LINES ||
             batch_len >= RL_BATCH_MAX_BYTES ||
             esp_timer_get_time() - first_line_us >= RL_FLUSH_MS * 1000LL)) {
            rl_batch_flush(batch, &batch_len, &batch_lines);
        }
    }
}

esp_err_t remote_log_early_init(void)
{
    if (strlen(CONFIG_APP_DEVICE_NAME) > 0) {
        strlcpy(s_device_id, CONFIG_APP_DEVICE_NAME, sizeof(s_device_id));
    } else {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_device_id, sizeof(s_device_id), "esp32c3-%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
    }
    log_mask_phone(CONFIG_APP_SIM_PHONE_NUMBER, s_phone, sizeof(s_phone));

    s_log_rb = xRingbufferCreate(RL_RINGBUF_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (s_log_rb == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_orig_vprintf = esp_log_set_vprintf(remote_log_vprintf);
    return ESP_OK;
}

esp_err_t remote_log_start(QueueHandle_t sms_queue)
{
    if (s_log_rb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sms_queue = sms_queue;
    if (xTaskCreate(remote_log_task, "log_fwd", 4096, NULL, 3, &s_fwd_task) != pdPASS) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Remote log forwarding started (device=%s, topic=%s)",
             s_device_id, CONFIG_APP_MQTT_TOPIC_LOG);
    return ESP_OK;
}

#else // !CONFIG_APP_REMOTE_LOG_ENABLE

esp_err_t remote_log_early_init(void)
{
    return ESP_OK;
}

esp_err_t remote_log_start(QueueHandle_t sms_queue)
{
    (void)sms_queue;
    return ESP_OK;
}

#endif // CONFIG_APP_REMOTE_LOG_ENABLE
