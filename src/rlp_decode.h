#pragma once

#include <stdint.h>
#include <stddef.h>
#include "types.h"

int decode_ledger_tx(const uint8_t *rlp, size_t rlp_len, zond_tx_t *tx);
