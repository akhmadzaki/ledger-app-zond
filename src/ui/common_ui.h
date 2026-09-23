#pragma once

#include <stdint.h>

void ui_error_blind_signing(void);
void ui_contract_call_init(uint8_t param_count);
int ui_confirm_parameter(void);
int ui_confirm_selector(void);
