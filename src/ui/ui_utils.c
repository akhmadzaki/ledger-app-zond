#include "ui_utils.h"

void bytes_to_hex_string(const uint8_t *src, size_t src_len, char *dst) {
    const char *hex_chars = "0123456789abcdef";
    for (size_t i = 0; i < src_len; i++) {
        dst[i * 2]     = hex_chars[(src[i] >> 4) & 0x0F]; // High nibble
        dst[i * 2 + 1] = hex_chars[src[i] & 0x0F];       // Low nibble
    }
    // Always null-terminate the string
    dst[src_len * 2] = '\0';
}