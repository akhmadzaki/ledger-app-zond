#pragma once

#include <stdint.h>
#include <stddef.h>

void bytes_to_hex_string(const uint8_t *src, size_t src_len, char *dst);
