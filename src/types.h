#pragma once

#include <stddef.h>  // size_t
#include <stdint.h>  // uint*_t

#include "bip32.h"

#include "constants.h"
#include "lcx_mldsa.h"

/**
 * Enumeration with expected INS of APDU commands.
 */
typedef enum {
    GET_VERSION = 0x03,     /// version of the application
    GET_APP_NAME = 0x04,    /// name of the application
    GET_PUBLIC_KEY = 0x05,  /// public key of corresponding BIP32 path
    SIGN_TX = 0x06,         /// sign transaction with BIP32 path
    VERIFY_MSG = 0x07       /// verify message with BIP32 path
} command_e;
/**
 * Enumeration with parsing state.
 */
typedef enum {
    STATE_NONE,     /// No state
    STATE_PARSED,   /// Transaction data parsed
    STATE_APPROVED  /// Transaction data approved
} state_e;

/**
 * Enumeration with user request type.
 */
typedef enum {
    CONFIRM_ADDRESS,     /// confirm address derived from public key
    CONFIRM_TRANSACTION  /// confirm transaction information
} request_type_e;

/**
 * Structure for public key context information.
 */
typedef struct {
    uint8_t raw_public_key[65];  /// format (1), x-coordinate (32), y-coodinate (32)
    uint8_t chain_code[32];      /// for public key derivation
    uint8_t address[24];
} pubkey_ctx_t;

#define ADDRESS_LENGTH 64
#define MAX_FIELD_SIZE 32

typedef struct {
    uint8_t chain_id[MAX_FIELD_SIZE];
    uint8_t chain_id_len;

    uint8_t nonce[8];
    uint8_t nonce_len;

    uint8_t gas_tip_cap[MAX_FIELD_SIZE];
    uint8_t gas_tip_cap_len;

    uint8_t gas_fee_cap[MAX_FIELD_SIZE];
    uint8_t gas_fee_cap_len;

    uint8_t gas[8];
    uint8_t gas_len;

    uint8_t to[ADDRESS_LENGTH];
    uint8_t to_len;

    uint8_t value[MAX_FIELD_SIZE];
    uint8_t value_len;

    uint8_t data[MAX_DATA_SIZE];    
    uint16_t data_len;

    uint8_t descriptor[3];
    uint8_t descriptor_len;
} zond_tx_t;

// /** Maximum number of top-level function parameters. */
#define ABI_MAX_PARAMS 16

typedef struct {
    uint8_t selector[4];             /* 4-byte function selector        */
    bool    has_selector;            /* false if calldata < 4 bytes     */
    uint8_t param_count;             /* number of top-level params      */
    uint8_t params[ABI_MAX_PARAMS];  /* indices into static node pool   */
} abi_calldata_t;

/**
 * Structure for transaction information context.
 */
typedef struct {
    uint8_t raw_tx[MAX_TRANSACTION_LEN];  /// raw transaction serialized
    size_t raw_tx_len;                    /// length of raw transaction
    uint8_t m_hash[32];                   /// message hash digest
    zond_tx_t tx_data;
    abi_calldata_t calldata;
} transaction_ctx_t;

/**
 * Structure for global context.
 */
typedef struct {
    request_type_e req_type; 
    state_e state;
    uint32_t bip32_path[MAX_BIP32_PATH];    
    uint8_t bip32_path_len;           
    uint8_t address[ADDRESS_SIZE];  
    union {
        pubkey_ctx_t pk_info;       
        transaction_ctx_t tx_info;
    };                                     
} global_ctx_t;
