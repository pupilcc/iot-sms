#ifndef LOG_REDACTION_H
#define LOG_REDACTION_H

#include <stddef.h>

#define LOG_REDACTED_VALUE "[REDACTED]"
#define LOG_MASKED_PHONE_SIZE 40

/**
 * @brief Masks a phone number for logging.
 *
 * Phone numbers longer than seven characters keep the first three and last
 * four characters. Short values are fully redacted.
 *
 * @return output when it is writable, otherwise LOG_REDACTED_VALUE.
 */
const char *log_mask_phone(const char *phone, char *output, size_t output_size);

#endif // LOG_REDACTION_H
