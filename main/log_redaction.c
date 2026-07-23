#include <stdio.h>
#include <string.h>

#include "log_redaction.h"

const char *log_mask_phone(const char *phone, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return LOG_REDACTED_VALUE;
    }

    if (phone == NULL || phone[0] == '\0' || strcmp(phone, "UNKNOWN") == 0) {
        snprintf(output, output_size, "%s", "UNKNOWN");
        return output;
    }

    size_t length = strlen(phone);
    for (size_t i = 0; i < length; i++) {
        if ((unsigned char)phone[i] >= 0x80) {
            snprintf(output, output_size, "%s", LOG_REDACTED_VALUE);
            return output;
        }
    }
    if (length <= 7) {
        snprintf(output, output_size, "%s", LOG_REDACTED_VALUE);
        return output;
    }

    int written = snprintf(output, output_size, "%.3s***%s",
                           phone, phone + length - 4);
    if (written < 0 || (size_t)written >= output_size) {
        snprintf(output, output_size, "%s", LOG_REDACTED_VALUE);
    }
    return output;
}
