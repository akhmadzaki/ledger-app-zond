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

#include <stdbool.h>  // bool

#include "crypto_helpers.h"

#include "validate.h"
#include "menu.h"
#include "sw.h"
#include "globals.h"
#include "send_response.h"
#include "lcx_mldsa.h"
#include "cx_mldsa_internal.h"

const uint8_t QRL_CTX [] = {'Z', 'O', 'N', 'D', 0x01, 0x01, 0x00, 0x00};

void validate_pubkey(bool choice) {
    if (choice) {
        helper_send_response_address();
    } else {
        PRINTF("HERE 1\n");
        io_send_sw(SW_DENY);
    }
}

static int crypto_sign_message(void) {
    PRINTF("crypto sign start\n");
    PRINTF("bip32_path_len %d\n", G_context.bip32_path_len);
    PRINTF("raw_tx_len %d\n", G_context.tx_info.raw_tx_len);
    PRINTF("SIGNING START\n");

    static uint8_t sk[MLDSA87_SECRETKEYBYTES];
    uint8_t sig[MLDSA87_SIGBYTES];
    uint8_t raw_seed[64] = {0};
    cx_err_t err =
        os_derive_bip32_no_throw(CX_CURVE_SECP256K1, G_context.bip32_path, G_context.bip32_path_len, raw_seed, NULL);
    if (err != CX_OK) {
        return -1;
    }

    uint8_t mldsa87_seed[32] = {0};
    for (int i = 0; i < 32; i++) {
        mldsa87_seed[i] = raw_seed[i];
    }

    err = MLDSA_internal_keygen(sig, MLDSA87_PUBLICKEYBYTES, sk, sizeof(sk), mldsa87_seed, MLDSA_87);
    explicit_bzero(mldsa87_seed, sizeof(mldsa87_seed));
    explicit_bzero(raw_seed, sizeof(raw_seed));
    if (err != CX_OK) {
        return -1;
    }

    size_t actual_len = 0;
    PRINTF("HASH: ");
    for(int i = 0; i < 32; i++) {
        PRINTF("%02x", G_context.tx_info.m_hash[i]);
    }
    PRINTF("\n");

    PRINTF("CTX: ");
    for(unsigned int i = 0; i < sizeof(QRL_CTX); i++) {
        PRINTF("%02x", QRL_CTX[i]);
    }
    PRINTF("\n");

    err = MLDSA_sign(sig,
                    sizeof(sig),
                    &actual_len,
                    G_context.tx_info.m_hash,
                    32,
                    QRL_CTX,
                    8,
                    sk,
                    sizeof(sk),
                    MLDSA_87);
    explicit_bzero(sk, sizeof(sk));
    PRINTF("bip32_path_len %d\n", G_context.bip32_path_len);
    PRINTF("raw_tx_len %d\n", G_context.tx_info.raw_tx_len);

    for (size_t i = 0; i < MLDSA87_SIGBYTES; i++) {
        uint8_t tmp = sig[i];
        nvm_write((void *) &N_storage.sig[i], &tmp, sizeof(uint8_t));
    }
    
    PRINTF("SIGNING END\n");
    if (err != CX_OK || actual_len != MLDSA87_SIGBYTES) {
        return -1;
    }
    PRINTF("VERIFY START\n");

    err = MLDSA_verify((uint8_t *) N_storage.sig,
                      MLDSA87_SIGBYTES,
                      G_context.tx_info.m_hash,
                      32,
                      QRL_CTX,
                      8,
                      (uint8_t *) N_storage.pk,
                      MLDSA87_PUBLICKEYBYTES,
                      MLDSA_87);

    PRINTF("VERIFY END\n");
    if (err == CX_OK) {
        PRINTF("SIGNATURE CORRECT\n");
    } else {
        PRINTF("SIGNATURE WRONG\n");
    }

    if (err != CX_OK) {
        return -1;
    }

    return 0;
}

bool validate_transaction(bool choice) {
    if (choice) {
        G_context.state = STATE_APPROVED;

        if (crypto_sign_message() != 0) {
            G_context.state = STATE_NONE;
            io_send_sw(SW_SIGNATURE_FAIL);
            return false;
        } else {
            helper_send_response_sig(0);
            return true;
        }
    } else {
        io_send_sw(SW_DENY);
        return false;
    }
}
