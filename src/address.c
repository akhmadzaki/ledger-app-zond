/*****************************************************************************
 *   Ledger App Boilerplate.
 *   (c) 2020 Ledger SAS.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *****************************************************************************/

#include <stdint.h>   // uint*_t
#include <stddef.h>   // size_t
#include <stdbool.h>  // bool
#include <string.h>   // memmove

#include "os.h"
#include "cx.h"
#include "crypto_helpers.h"
#include "ledger_assert.h"
#include "nbgl_use_case.h"

#include "address.h"

#include "constants.h"
#include "globals.h"
#include "lcx_mldsa.h"
#include "lcx_sha3.h"

bool is_valid_zond_bip32_path(const uint32_t bip32_path[], size_t bip32_path_len) {
    if (bip32_path_len != 5) {
        return false;
    }
    if (bip32_path[0] != 0x8000002Cu ||  // purpose 44'
        bip32_path[1] != 0x800000EEu ||  // coin type 238'
        (bip32_path[2] & 0x80000000u) == 0 || (bip32_path[3] & 0x80000000u) != 0 ||
        (bip32_path[4] & 0x80000000u) != 0) {
        return false;
    }
    return true;
}

cx_err_t address_from_bip32_path(const uint32_t bip32_path[],
                                 size_t bip32_path_len,
                                 uint8_t address[ADDRESS_SIZE]) {
    PRINTF("address start\n");
    uint8_t raw_seed[64] = {0};
    PRINTF("%d\n", bip32_path_len);
    cx_err_t err =
        os_derive_bip32_no_throw(CX_CURVE_SECP256K1, bip32_path, bip32_path_len, raw_seed, NULL);
    PRINTF("after derive\n");
    if (err != CX_OK) {
        PRINTF("0x%x\n", err);
        return err;
    }
    PRINTF("after error\n");
    uint8_t mldsa87_seed[32] = {0};
    for (int i = 0; i < 32; i++) {
        mldsa87_seed[i] = raw_seed[i];
    }
    uint8_t sk[MLDSA87_SECRETKEYBYTES] = {0};
    uint8_t pk[MLDSA87_PUBLICKEYBYTES] = {0};
    PRINTF("keygen start\n");
    err = MLDSA_internal_keygen(pk, sizeof(pk), sk, sizeof(sk), mldsa87_seed, MLDSA_87);
    explicit_bzero(mldsa87_seed, sizeof(mldsa87_seed));
    explicit_bzero(raw_seed, sizeof(raw_seed));
    explicit_bzero(sk, sizeof(sk));
    PRINTF("keygen end\n");
    if (err != CX_OK) {
        return -1;
    }

    for (size_t i = 0; i < MLDSA87_PUBLICKEYBYTES; i++) {
        uint8_t tmp = pk[i];
        nvm_write((void *) &N_storage.pk[i], &tmp, sizeof(uint8_t));
    }

    uint8_t desc[DESCRIPTOR_BYTES] = {1, 0, 0};  // ML-DSA-87 descriptor

    // address = SHAKE256_XOF(descriptor || pk, 64)
    // shake256_ctx ctx;
    // shake256_init(&ctx);
    // shake256_absorb(&ctx, desc, DESCRIPTOR_BYTES);
    // shake256_absorb(&ctx, N_storage.pk, MLDSA87_PUBLICKEYBYTES);
    // shake256_finalize(&ctx);
    // shake256_squeeze(&ctx, address, ADDRESS_SIZE);
    // shake256_clear(&ctx);
    cx_sha3_t hash;
    err = cx_sha3_xof_init_no_throw(&hash, 256, ADDRESS_SIZE);
    if(err != CX_OK) {
        return -1;
    }
    err =  cx_sha3_update(&hash, desc, DESCRIPTOR_BYTES);
    if(err != CX_OK) {
        return -1;
    }

    err = cx_sha3_update(&hash,  (const uint8_t *) N_storage.pk, MLDSA87_PUBLICKEYBYTES);
    if(err != CX_OK) {
        return -1;
    }

    err = cx_sha3_final(&hash, address);
    if(err != CX_OK) {
        return -1;
    }
    PRINTF("END\n");

    return 0;
}

// EIP-55-style checksum casing with SHAKE-256 over the 128-char lowercase hex
// body (the 'Q' prefix is not hashed). out must hold 1 + 2*ADDRESS_SIZE + 1.
bool format_checksummed_address(const uint8_t address[ADDRESS_SIZE], char *out, size_t out_len) {
    static const char hexc[] = "0123456789abcdef";

    if (out == NULL || out_len < 1 + 2 * ADDRESS_SIZE + 1) {
        return false;
    }

    char *body = out + 1;
    for (int i = 0; i < ADDRESS_SIZE; i++) {
        body[2 * i] = hexc[address[i] >> 4];
        body[2 * i + 1] = hexc[address[i] & 0x0f];
    }

    uint8_t mask[ADDRESS_SIZE] = {0};
    cx_sha3_t hash;
    cx_err_t err = cx_sha3_xof_init_no_throw(&hash, 256, ADDRESS_SIZE);
    if(err != CX_OK) {
        return false;
    }
    err =  cx_sha3_update(&hash, (const uint8_t *) body, 2 * ADDRESS_SIZE);
    if(err != CX_OK) {
        return false;
    }
    err = cx_sha3_final(&hash, mask);
    if(err != CX_OK) {
        return false;
    }

    for (int i = 0; i < 2 * ADDRESS_SIZE; i++) {
        uint8_t nibble = (i % 2 == 0) ? (mask[i / 2] >> 4) : (mask[i / 2] & 0x0f);
        if (body[i] >= 'a' && body[i] <= 'f' && nibble >= 8) {
            body[i] -= 'a' - 'A';
        }
    }

    out[0] = 'Q';
    out[1 + 2 * ADDRESS_SIZE] = '\0';
    return true;
}
