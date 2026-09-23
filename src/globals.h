#pragma once

#include <stdint.h>

#include "ux.h"

#include "io.h"
#include "types.h"
#include "constants.h"
#include "polyvec.h"
#include "poly.h"

/**
 * Global context for user requests.
 */
extern global_ctx_t G_context;

/**
 * Global structure for NVM data storage.
 */
typedef struct internal_storage_t {
    PolyVecK t1;
    PolyVecK w1;
    PolyVecK w0;
    PolyVecK h;
    PolyVecL y;
    PolyVecL z;
    Poly cp;
    union {
        PolyVecK t0;
        uint8_t buf_verify[K * POLY_W1_PACKED_BYTES];
    } pack1;

    uint8_t sig[CRYPTO_BYTES];
    uint8_t pk[CRYPTO_PUBLIC_KEY_BYTES];
    uint8_t buf[850];
    uint8_t enable_blind_signing;
    uint8_t display_nonce;
    uint8_t display_tx_hash;
    uint8_t enable_debug_smart_contract;
    uint8_t initialized;
    uint8_t is_sending_signature;
} internal_storage_t;

extern const internal_storage_t N_storage_real;
#define N_storage (*(volatile internal_storage_t *) PIC(&N_storage_real))
