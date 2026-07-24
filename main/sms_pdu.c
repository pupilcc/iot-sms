#include <string.h>
#include "esp_log.h"

#include "sms_pdu.h"

static const char *TAG = "sms_pdu";

// SMSC最长12字节 + TPDU最长164字节
#define PDU_MAX_BYTES   192
// TP-UDL以septet计时最大160,留些余量容忍畸形UDL
#define PDU_MAX_SEPTETS 176

// GSM 03.38 默认字母表 → UTF-8
static const char *const s_gsm7_default[128] = {
    "@",  "£",  "$",  "¥",  "è",  "é",  "ù",  "ì",
    "ò",  "Ç",  "\n", "Ø",  "ø",  "\r", "Å",  "å",
    "Δ",  "_",  "Φ",  "Γ",  "Λ",  "Ω",  "Π",  "Ψ",
    "Σ",  "Θ",  "Ξ",  "",   "Æ",  "æ",  "ß",  "É",
    " ",  "!",  "\"", "#",  "¤",  "%",  "&",  "'",
    "(",  ")",  "*",  "+",  ",",  "-",  ".",  "/",
    "0",  "1",  "2",  "3",  "4",  "5",  "6",  "7",
    "8",  "9",  ":",  ";",  "<",  "=",  ">",  "?",
    "¡",  "A",  "B",  "C",  "D",  "E",  "F",  "G",
    "H",  "I",  "J",  "K",  "L",  "M",  "N",  "O",
    "P",  "Q",  "R",  "S",  "T",  "U",  "V",  "W",
    "X",  "Y",  "Z",  "Ä",  "Ö",  "Ñ",  "Ü",  "§",
    "¿",  "a",  "b",  "c",  "d",  "e",  "f",  "g",
    "h",  "i",  "j",  "k",  "l",  "m",  "n",  "o",
    "p",  "q",  "r",  "s",  "t",  "u",  "v",  "w",
    "x",  "y",  "z",  "ä",  "ö",  "ñ",  "ü",  "à",
};

// 0x1B转义后的扩展表,未列出的转义回落到默认字母表
static const struct {
    uint8_t     code;
    const char *utf8;
} s_gsm7_ext[] = {
    {0x0A, "\n"}, // 规范为换页符,按换行呈现更实用
    {0x14, "^"},
    {0x28, "{"},
    {0x29, "}"},
    {0x2F, "\\"},
    {0x3C, "["},
    {0x3D, "~"},
    {0x3E, "]"},
    {0x40, "|"},
    {0x65, "€"},
};
static const size_t s_gsm7_ext_size = sizeof(s_gsm7_ext) / sizeof(s_gsm7_ext[0]);

static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// 十六进制字符串转字节。返回字节数,非法或超长返回-1
static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_size) {
    size_t hex_len = strlen(hex);
    // 容忍行尾残留的空白
    while (hex_len > 0 && (hex[hex_len - 1] == '\r' || hex[hex_len - 1] == '\n' ||
                           hex[hex_len - 1] == ' ' || hex[hex_len - 1] == '\t')) {
        hex_len--;
    }
    if (hex_len == 0 || hex_len % 2 != 0 || hex_len / 2 > out_size) {
        return -1;
    }
    for (size_t i = 0; i < hex_len / 2; i++) {
        int hi = hex_char_to_int(hex[i * 2]);
        int lo = hex_char_to_int(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(hex_len / 2);
}

// 追加一个UTF-8字符,放不下时返回原idx(调用方据此停止)
static size_t utf8_append(char *buf, size_t buf_size, size_t idx, const char *s) {
    size_t n = strlen(s);
    if (idx + n + 1 > buf_size) {
        return idx;
    }
    memcpy(buf + idx, s, n);
    return idx + n;
}

static size_t utf8_append_cp(char *buf, size_t buf_size, size_t idx, uint32_t cp) {
    char seq[5];
    size_t n = 0;
    if (cp < 0x80) {
        seq[n++] = (char)cp;
    } else if (cp < 0x800) {
        seq[n++] = (char)(0xC0 | (cp >> 6));
        seq[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        seq[n++] = (char)(0xE0 | (cp >> 12));
        seq[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        seq[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        seq[n++] = (char)(0xF0 | (cp >> 18));
        seq[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        seq[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        seq[n++] = (char)(0x80 | (cp & 0x3F));
    }
    seq[n] = '\0';
    return utf8_append(buf, buf_size, idx, seq);
}

// 从bit 0开始解包septet_count个7位字符。返回实际解出的个数
static size_t gsm7_unpack(const uint8_t *data, size_t data_len, size_t septet_count,
                          uint8_t *out, size_t out_size) {
    size_t n = 0;
    for (size_t i = 0; i < septet_count && n < out_size; i++) {
        size_t bit_pos = i * 7;
        size_t byte_pos = bit_pos / 8;
        size_t shift = bit_pos % 8;
        if (byte_pos >= data_len) {
            break;
        }
        uint16_t v = (uint16_t)(data[byte_pos] >> shift);
        if (shift > 1 && byte_pos + 1 < data_len) {
            v |= (uint16_t)((uint16_t)data[byte_pos + 1] << (8 - shift));
        }
        out[n++] = (uint8_t)(v & 0x7F);
    }
    return n;
}

static const char *gsm7_ext_lookup(uint8_t code) {
    for (size_t i = 0; i < s_gsm7_ext_size; i++) {
        if (s_gsm7_ext[i].code == code) {
            return s_gsm7_ext[i].utf8;
        }
    }
    return NULL;
}

static size_t gsm7_septets_to_utf8(const uint8_t *septets, size_t count, char *out, size_t out_size) {
    size_t idx = 0;
    for (size_t i = 0; i < count; i++) {
        const char *s;
        if (septets[i] == 0x1B) {
            i++;
            if (i >= count) {
                break;
            }
            s = gsm7_ext_lookup(septets[i]);
            if (s == NULL) {
                s = s_gsm7_default[septets[i] & 0x7F]; // 未定义的转义按默认表呈现
            }
        } else {
            s = s_gsm7_default[septets[i] & 0x7F];
        }
        size_t next = utf8_append(out, out_size, idx, s);
        if (next == idx && s[0] != '\0') {
            ESP_LOGW(TAG, "Text buffer full, truncating decoded SMS part");
            break;
        }
        idx = next;
    }
    out[idx] = '\0';
    return idx;
}

// UTF-16BE(含代理对) → UTF-8
static size_t ucs2_to_utf8(const uint8_t *data, size_t len, char *out, size_t out_size) {
    size_t idx = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t cp = ((uint32_t)data[i] << 8) | data[i + 1];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
            uint32_t low = ((uint32_t)data[i + 2] << 8) | data[i + 3];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        size_t next = utf8_append_cp(out, out_size, idx, cp);
        if (next == idx) {
            ESP_LOGW(TAG, "Text buffer full, truncating decoded SMS part");
            break;
        }
        idx = next;
    }
    out[idx] = '\0';
    return idx;
}

// 8bit数据按Latin-1解释,保证输出仍是合法UTF-8
static size_t latin1_to_utf8(const uint8_t *data, size_t len, char *out, size_t out_size) {
    size_t idx = 0;
    for (size_t i = 0; i < len; i++) {
        size_t next = utf8_append_cp(out, out_size, idx, data[i]);
        if (next == idx) {
            break;
        }
        idx = next;
    }
    out[idx] = '\0';
    return idx;
}

// 解析TP-OA。consumed返回该地址字段占用的字节数
static bool decode_address(const uint8_t *p, size_t avail, char *out, size_t out_size,
                           size_t *consumed) {
    if (avail < 2) {
        return false;
    }
    uint8_t addr_len = p[0]; // 半八位组数(数字地址即数字个数)
    uint8_t toa = p[1];
    size_t addr_bytes = ((size_t)addr_len + 1) / 2;
    if (avail < 2 + addr_bytes) {
        return false;
    }
    *consumed = 2 + addr_bytes;

    if ((toa & 0x70) == 0x50) {
        // 字母型地址(如"giffgaff"): GSM-7打包。septet数必须由半八位组数算,
        // 按字节数×8/7算会从填充位里多解出一个字符
        size_t septet_count = (size_t)addr_len * 4 / 7;
        uint8_t septets[32];
        if (septet_count > sizeof(septets)) {
            septet_count = sizeof(septets);
        }
        size_t n = gsm7_unpack(p + 2, addr_bytes, septet_count, septets, sizeof(septets));
        gsm7_septets_to_utf8(septets, n, out, out_size);
        return true;
    }

    static const char bcd_digits[] = "0123456789*#abc";
    size_t idx = 0;
    if ((toa & 0x70) == 0x10 && idx + 1 < out_size) {
        out[idx++] = '+'; // 国际号码
    }
    for (size_t d = 0; d < addr_len && idx + 1 < out_size; d++) {
        uint8_t b = p[2 + d / 2];
        uint8_t nibble = (d % 2 == 0) ? (b & 0x0F) : (b >> 4);
        if (nibble == 0x0F) {
            break; // 填充半字节
        }
        out[idx++] = bcd_digits[nibble];
    }
    out[idx] = '\0';
    return true;
}

// 解析UDH中的拼接信息(IEI 0x00为8位参考号, 0x08为16位)
static void parse_udh(const uint8_t *udh, size_t udh_total, sms_pdu_t *out) {
    size_t i = 1; // udh[0]是UDHL
    while (i + 1 < udh_total) {
        uint8_t iei = udh[i];
        uint8_t ie_len = udh[i + 1];
        if (i + 2 + ie_len > udh_total) {
            break;
        }
        const uint8_t *ie = udh + i + 2;
        if (iei == 0x00 && ie_len >= 3) {
            out->concat = true;
            out->ref = ie[0];
            out->total = ie[1];
            out->seq = ie[2];
        } else if (iei == 0x08 && ie_len >= 4) {
            out->concat = true;
            out->ref = (uint16_t)(((uint16_t)ie[0] << 8) | ie[1]);
            out->total = ie[2];
            out->seq = ie[3];
        }
        i += 2 + ie_len;
    }
}

size_t sms_pdu_utf8_trim_tail(char *buf, size_t len) {
    size_t end = len;
    int back = 0;
    while (end > 0 && back < 3 && ((unsigned char)buf[end - 1] & 0xC0) == 0x80) {
        end--;
        back++;
    }
    if (end == 0) {
        buf[len] = '\0';
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
    buf[len] = '\0';
    return len;
}

esp_err_t sms_pdu_decode(const char *pdu_hex, sms_pdu_t *out) {
    uint8_t pdu[PDU_MAX_BYTES];

    if (pdu_hex == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int pdu_len = hex_to_bytes(pdu_hex, pdu, sizeof(pdu));
    if (pdu_len < 0) {
        ESP_LOGW(TAG, "Invalid or oversized PDU hex (hex_len=%u)", (unsigned)strlen(pdu_hex));
        return ESP_FAIL;
    }
    size_t total_len = (size_t)pdu_len;

    memset(out, 0, sizeof(*out));

    // SMSC地址: 首字节是其后跟随的字节数
    size_t pos = 0;
    if (total_len < 1) {
        return ESP_FAIL;
    }
    pos = 1 + (size_t)pdu[0];
    if (pos >= total_len) {
        ESP_LOGW(TAG, "PDU truncated in SMSC field");
        return ESP_FAIL;
    }

    uint8_t first_octet = pdu[pos++];
    if ((first_octet & 0x03) != 0x00) {
        ESP_LOGW(TAG, "Not an SMS-DELIVER PDU (MTI=%d)", first_octet & 0x03);
        return ESP_FAIL;
    }
    bool has_udh = (first_octet & 0x40) != 0;

    size_t consumed = 0;
    if (!decode_address(pdu + pos, total_len - pos, out->sender, sizeof(out->sender), &consumed)) {
        ESP_LOGW(TAG, "PDU truncated in originating address");
        return ESP_FAIL;
    }
    pos += consumed;

    // TP-PID(1) + TP-DCS(1) + TP-SCTS(7) + TP-UDL(1)
    if (pos + 10 > total_len) {
        ESP_LOGW(TAG, "PDU truncated before user data");
        return ESP_FAIL;
    }
    pos += 1; // TP-PID
    uint8_t dcs = pdu[pos++];
    pos += 7; // TP-SCTS: 时间戳由下游按本机时间生成,此处不解析
    uint8_t udl = pdu[pos++];

    const uint8_t *ud = pdu + pos;
    size_t ud_len = total_len - pos;
    if (udl > 0 && ud_len == 0) {
        ESP_LOGW(TAG, "PDU truncated in user data (udl=%u)", udl);
        return ESP_FAIL;
    }

    size_t udh_total = 0;
    if (has_udh) {
        if (ud_len < 1 || (size_t)ud[0] + 1 > ud_len) {
            ESP_LOGW(TAG, "PDU truncated in user data header");
            return ESP_FAIL;
        }
        udh_total = (size_t)ud[0] + 1; // UDHL自身也计入
        parse_udh(ud, udh_total, out);
    }

    // TP-DCS字符集: 通用编码组取bit3-2,消息类别组(1111xxxx)取bit2
    uint8_t alphabet;
    if ((dcs & 0xC0) == 0x00) {
        alphabet = (dcs >> 2) & 0x03;
    } else if ((dcs & 0xF0) == 0xF0) {
        alphabet = (dcs >> 2) & 0x01;
    } else {
        alphabet = 0;
    }

    if (alphabet == 0) { // GSM-7
        // UDL以septet计且包含UDH占位。UDH之后补填充位对齐到septet边界,
        // 因此正文起始septet索引 = ceil(UDH位数/7)
        size_t skip = (udh_total * 8 + 6) / 7;
        size_t septet_count = udl;
        uint8_t septets[PDU_MAX_SEPTETS];
        if (septet_count > sizeof(septets)) {
            septet_count = sizeof(septets);
        }
        size_t n = gsm7_unpack(ud, ud_len, septet_count, septets, sizeof(septets));
        if (skip > n) {
            skip = n;
        }
        gsm7_septets_to_utf8(septets + skip, n - skip, out->text, sizeof(out->text));
    } else {
        // UCS2与8bit的UDL以字节计
        size_t len = udl;
        if (len > ud_len) {
            len = ud_len;
        }
        size_t skip = udh_total > len ? len : udh_total;
        if (alphabet == 2) {
            ucs2_to_utf8(ud + skip, len - skip, out->text, sizeof(out->text));
        } else {
            latin1_to_utf8(ud + skip, len - skip, out->text, sizeof(out->text));
        }
    }

    return ESP_OK;
}
