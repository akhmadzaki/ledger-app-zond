#pragma once

#include <stdint.h>
#include <stddef.h>

void bytes_to_hex_string(const uint8_t *src, size_t src_len, char *dst);

#define UI_BUFFER_SIZE 140

extern char g_ui_buffer[UI_BUFFER_SIZE];
