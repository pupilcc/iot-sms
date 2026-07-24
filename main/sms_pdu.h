#ifndef SMS_PDU_H
#define SMS_PDU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// 单段短信解码后的UTF-8上限: GSM-7单段最多160字符,扩展字符最坏3字节/字符
#define SMS_PDU_TEXT_MAX 512

// 一条SMS-DELIVER PDU解码后的结果(长短信为其中一段)
typedef struct {
    char     sender[32];             // 发件号码,或字母型地址(如"giffgaff"),UTF-8
    char     text[SMS_PDU_TEXT_MAX]; // 本段正文,UTF-8
    bool     concat;                 // UDH中是否含长短信拼接信息
    uint16_t ref;                    // 拼接参考号(8位或16位)
    uint8_t  total;                  // 总段数
    uint8_t  seq;                    // 本段序号(1..total)
} sms_pdu_t;

/**
 * @brief 解码一条SMS-DELIVER PDU(十六进制字符串,含SMSC字段)
 *
 * 自行处理TP-OA(数字/字母型地址)、TP-DCS(GSM-7/8bit/UCS2)与UDH拼接头,
 * 不依赖模组的文本模式转换。
 *
 * @param pdu_hex +CMT URC正文行,如"0891683108200155F0240D91..."
 * @param out     解码结果,仅在返回ESP_OK时有效
 * @return ESP_OK 成功; ESP_ERR_INVALID_ARG 参数为空; ESP_FAIL 十六进制非法、
 *         长度截断或不是SMS-DELIVER
 */
esp_err_t sms_pdu_decode(const char *pdu_hex, sms_pdu_t *out);

/**
 * @brief 截断处若正好切在UTF-8多字节字符中间,去掉末尾不完整的序列
 *
 * @param buf 待处理缓冲,函数保证结果以'\0'结尾
 * @param len 当前字节长度
 * @return 处理后的字节长度
 */
size_t sms_pdu_utf8_trim_tail(char *buf, size_t len);

#endif // SMS_PDU_H
