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
#include <string.h>   // memset

#include "os.h"
#include "glyphs.h"
#include "os_io_seproxyhal.h"
#include "nbgl_use_case.h"
#include "io.h"
#include "bip32.h"
#include "format.h"

#include "display.h"
#include "constants.h"
#include "globals.h"
#include "sw.h"
#include "address.h"
#include "validate.h"
#include "menu.h"
#include "address.h"

static char g_from_address[1 + ADDRESS_SIZE * 2 + 1];
static char g_amount[30];
static char g_to_address[1 + ADDRESS_SIZE * 2 + 1];
static char g_max_fees[30];
static char g_nonce[10];
static char g_tx_hash[67];

static nbgl_contentTagValue_t pairs[6];
static nbgl_contentTagValueList_t pairList;

#define MAX_DECIMAL_DIGITS 40
#define MAX_RESULT_LEN     50



static void uint8_array_to_decimal(const uint8_t *bytes, size_t len, char *out) {
    uint8_t temp[32] = {0};  // Ensure full zero-init
    memcpy(temp, bytes, len);

    char result[MAX_DECIMAL_DIGITS];
    int result_index = MAX_DECIMAL_DIGITS;

    // Repeated division by 10
    while (1) {
        int remainder = 0;
        int is_zero = 1;

        for (size_t i = 0; i < len; i++) {
            int val = (remainder << 8) | temp[i];
            temp[i] = val / 10;
            remainder = val % 10;
            if (temp[i] != 0) is_zero = 0;
        }

        result[--result_index] = '0' + remainder;
        if (is_zero) break;
    }

    size_t num_digits = MAX_DECIMAL_DIGITS - result_index;
    memcpy(out, &result[result_index], num_digits);
    out[num_digits] = '\0';  // Proper null termination
}

// Inserts decimal point `decimals` from the right, trims trailing zeros
static void format_with_decimals(const char *raw, size_t decimals, char *out, size_t out_len) {
    size_t len = strlen(raw);
    char buffer[MAX_RESULT_LEN] = {0};
    size_t pos = 0;

    if (decimals == 0) {
        snprintf(out, out_len, "%s", raw);
        return;
    }

    if (len <= decimals) {
        buffer[pos++] = '0';
        buffer[pos++] = '.';
        for (size_t i = 0; i < decimals - len && pos < MAX_RESULT_LEN - 1; i++) {
            buffer[pos++] = '0';
        }
        for (size_t i = 0; i < len && pos < MAX_RESULT_LEN - 1; i++) {
            buffer[pos++] = raw[i];
        }
    } else {
        size_t int_part = len - decimals;
        for (size_t i = 0; i < int_part && pos < MAX_RESULT_LEN - 1; i++) {
            buffer[pos++] = raw[i];
        }
        if (pos < MAX_RESULT_LEN - 1) {
            buffer[pos++] = '.';
        }
        for (size_t i = int_part; i < len && pos < MAX_RESULT_LEN - 1; i++) {
            buffer[pos++] = raw[i];
        }
    }
    buffer[pos] = '\0';

    // Trim trailing zeros
    char *end = buffer + strlen(buffer) - 1;
    while (*end == '0' && end > buffer) {
        *end-- = '\0';
    }
    if (*end == '.') {
        *end = '\0';  // remove dot if nothing after
    }

    snprintf(out, out_len, "%s", buffer);
}

// Wrapper: amount in wei to QRL units
static void convert_amount_to_eth(const uint8_t *amount,
                                  size_t len,
                                  char *out_str,
                                  size_t out_len) {
    char dec[MAX_RESULT_LEN];
    uint8_array_to_decimal(amount, len, dec);
    format_with_decimals(dec, 18, out_str, out_len);
}

// called when long press button on 3rd page is long-touched or when reject footer is touched
static void review_choice(bool confirm) {
    // Answer, display a status page and go back to main
    nbgl_useCaseSpinner("Signing");
    validate_transaction(confirm);
    if (confirm) {
        nbgl_useCaseReviewStatus(STATUS_TYPE_TRANSACTION_SIGNED, ui_menu_main);
    } else {
        nbgl_useCaseReviewStatus(STATUS_TYPE_TRANSACTION_REJECTED, ui_menu_main);
    }
}

void print_tx_utils(zond_tx_t *tx) {
    PRINTF("======== ZOND TX ========\n");
    PRINTF("Chain ID: 0x");
    for (int i = 0; i < tx->chain_id_len; i++) PRINTF("%02x", tx->chain_id[i]);
    PRINTF("\n");

    PRINTF("Nonce: 0x");
    for (int i = 0; i < tx->nonce_len; i++) PRINTF("%02x", tx->nonce[i]);
    PRINTF("\n");

    PRINTF("Gas Tip Cap: 0x");
    for (int i = 0; i < tx->gas_tip_cap_len; i++) PRINTF("%02x", tx->gas_tip_cap[i]);
    PRINTF("\n");

    PRINTF("Gas Fee Cap: 0x");
    for (int i = 0; i < tx->gas_fee_cap_len; i++) PRINTF("%02x", tx->gas_fee_cap[i]);
    PRINTF("\n");

    PRINTF("Gas: 0x");
    for (int i = 0; i < tx->gas_len; i++) PRINTF("%02x", tx->gas[i]);
    PRINTF("\n");

    PRINTF("To: 0x");
    for (int i = 0; i < ADDRESS_LENGTH; i++) PRINTF("%02x", tx->to[i]);
    PRINTF("\n");

    PRINTF("Value: 0x");
    for (int i = 0; i < tx->value_len; i++) PRINTF("%02x", tx->value[i]);
    PRINTF("\n");
    PRINTF("================\n");
}

void bytes_to_hex_string(const uint8_t *src, size_t src_len, char *dst) {
    const char *hex_chars = "0123456789abcdef";
    for (size_t i = 0; i < src_len; i++) {
        dst[i * 2]     = hex_chars[(src[i] >> 4) & 0x0F]; // High nibble
        dst[i * 2 + 1] = hex_chars[src[i] & 0x0F];       // Low nibble
    }
    // Always null-terminate the string
    dst[src_len * 2] = '\0';
}

// Public function to start the transaction review
// - Check if the app is in the right state for transaction review
// - Format the amount and address strings in g_amount and g_address buffers
// - Display the first screen of the transaction review
// - Display a warning if the transaction is blind-signed
int ui_display_transaction_bs_choice(bool is_blind_signed, zond_tx_t *tx) {
    if (G_context.req_type != CONFIRM_TRANSACTION || G_context.state != STATE_PARSED) {
        G_context.state = STATE_NONE;
        return io_send_sw(SW_BAD_STATE);
    }

    print_tx_utils(tx);

    PRINTF("DERIVE ADDRESS START\n");
    nbgl_useCaseSpinner("Getting address");
    cx_err_t error =
        address_from_bip32_path(G_context.bip32_path, G_context.bip32_path_len, G_context.address);
    if (error != CX_OK) {
        return io_send_sw(SW_DISPLAY_ADDRESS_FAIL);
    }
    PRINTF("DERIVE ADDRESS END\n");
    PRINTF("from ");
    for (int i = 0; i < ADDRESS_SIZE; i++) {
        PRINTF("%02x", G_context.address[i]);
    }
    PRINTF("\n");

    // Format nonce
    uint8_array_to_decimal(tx->nonce, tx->nonce_len, g_nonce);

    // Format tx hash
    char tx_hash[65] = {0};
    memset(tx_hash, 0, sizeof(tx_hash));
    bytes_to_hex_string(G_context.tx_info.m_hash, 32, tx_hash);
    tx_hash[64] = '\0';
    PRINTF("tx hash %s\n", tx_hash);
    memset(g_tx_hash, 0, sizeof(g_tx_hash));
    snprintf(g_tx_hash, sizeof(g_tx_hash), "0x%.*s", sizeof(tx_hash), tx_hash);
    PRINTF("tx hash %s\n", g_tx_hash);

    // Format from address
    memset(g_from_address, 0, sizeof(g_from_address));
    if (!format_checksummed_address(G_context.address, g_from_address, sizeof(g_from_address))) {
        return io_send_sw(SW_DISPLAY_ADDRESS_FAIL);
    }

    // Format amount
    char amount[30] = {0};
    memset(amount, 0, sizeof(amount));
    convert_amount_to_eth(tx->value, tx->value_len, amount, sizeof(amount));
    PRINTF("amount %s\n", amount);
    memset(g_amount, 0, sizeof(g_amount));
    snprintf(g_amount, sizeof(g_amount), "QRL %.*s", sizeof(amount), amount);

    // Format to address
    memset(g_to_address, 0, sizeof(g_to_address));
    if (!format_checksummed_address(tx->to, g_to_address, sizeof(g_to_address))) {
        return io_send_sw(SW_DISPLAY_ADDRESS_FAIL);
    }
    PRINTF("to %s\n", g_to_address);

    // Format max_fees
    char max_fees[30] = {0};
    memset(max_fees, 0, sizeof(max_fees));
    convert_amount_to_eth(tx->gas_fee_cap, tx->gas_fee_cap_len, max_fees, sizeof(max_fees));
    PRINTF("max fees %s\n", max_fees);
    memset(g_max_fees, 0, sizeof(g_max_fees));
    snprintf(g_max_fees, sizeof(g_max_fees), "QRL %.*s", sizeof(max_fees), max_fees);

    uint8_t num_pairs = 0;

    // Setup data to display
    if(N_storage.display_nonce && N_storage.display_tx_hash) {
        pairs[0].item = "From";
        pairs[0].value = g_from_address;
        pairs[1].item = "Amount";
        pairs[1].value = g_amount;
        pairs[2].item = "To";
        pairs[2].value = g_to_address;
        pairs[3].item = "Nonce";
        pairs[3].value = g_nonce;
        pairs[4].item = "Max fees";
        pairs[4].value = g_max_fees;
#ifdef SCREEN_SIZE_WALLET
        pairs[5].item = "Transaction hash";
#else
        pairs[5].item = "Tx hash";
#endif
        pairs[5].value = g_tx_hash;
        num_pairs = 6;
    } else if(N_storage.display_nonce) {
        pairs[0].item = "From";
        pairs[0].value = g_from_address;
        pairs[1].item = "Amount";
        pairs[1].value = g_amount;
        pairs[2].item = "To";
        pairs[2].value = g_to_address;
        pairs[3].item = "Nonce";
        pairs[3].value = g_nonce;
        pairs[4].item = "Max fees";
        pairs[4].value = g_max_fees;
        num_pairs = 5;
    } else if(N_storage.display_tx_hash) {
        pairs[0].item = "From";
        pairs[0].value = g_from_address;
        pairs[1].item = "Amount";
        pairs[1].value = g_amount;
        pairs[2].item = "To";
        pairs[2].value = g_to_address;
        pairs[3].item = "Max fees";
        pairs[3].value = g_max_fees;
#ifdef SCREEN_SIZE_WALLET
        pairs[4].item = "Transaction hash";
#else
        pairs[4].item = "Tx hash";
#endif
        pairs[4].value = g_tx_hash;
        num_pairs = 5;
    } else {
        pairs[0].item = "From";
        pairs[0].value = g_from_address;
        pairs[1].item = "Amount";
        pairs[1].value = g_amount;
        pairs[2].item = "To";
        pairs[2].value = g_to_address;
        pairs[3].item = "Max fees";
        pairs[3].value = g_max_fees;
        num_pairs = 4;
    }
    
    // Setup list
    pairList.nbMaxLinesForValue = 0;
    pairList.nbPairs = num_pairs;
    pairList.pairs = pairs;

    if (is_blind_signed) {
        // Start blind-signing review flow
        nbgl_useCaseReviewBlindSigning(TYPE_TRANSACTION,
                                       &pairList,
                                       &ICON_APP_BOILERPLATE,
                                       "Review transaction\n",
                                       NULL,
#ifdef SCREEN_SIZE_WALLET
                                       "Accept risk and sign transaction?\n",
#else
                                       NULL,
#endif
                                       NULL,
                                       review_choice);
    } else {
        // Start review flow
        nbgl_useCaseReview(TYPE_TRANSACTION,
                           &pairList,
                           &ICON_APP_BOILERPLATE,
                           "Review transaction\n",
                           NULL,
#ifdef SCREEN_SIZE_WALLET
                           "Sign transaction\n",
#else
                           NULL,
#endif
                           review_choice);
    }
    return 0;
}

// Flow used to display a blind-signed transaction
int ui_display_blind_signed_transaction(zond_tx_t *tx) {
    return ui_display_transaction_bs_choice(true, tx);
}

// Flow used to display a clear-signed transaction
int ui_display_transaction(zond_tx_t *tx) {
    return ui_display_transaction_bs_choice(false, tx);
}
