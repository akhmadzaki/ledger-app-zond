#pragma once

/**
 * Instruction class of the Boilerplate application.
 */
#define CLA 0xE0

/**
 * Length of APPNAME variable in the Makefile.
 */
#define APPNAME_LEN (sizeof(APPNAME) - 1)

/**
 * Maximum length of MAJOR_VERSION || MINOR_VERSION || PATCH_VERSION.
 */
#define APPVERSION_LEN 3

/**
 * Maximum length of application name.
 */
#define MAX_APPNAME_LEN 64

/**
 * Maximum transaction length (bytes).
 */
#ifdef TARGET_NANOX
    #define MAX_TRANSACTION_LEN 512
#else
    #define MAX_TRANSACTION_LEN 2048
#endif

/**
 * Prefix byte for QRL v2.0 addresses.
 */
#define ZOND_ADDRESS_PREFIX 'Q'

/**
 * Maximum data field length (bytes).
 */
#ifdef TARGET_NANOX
    #define MAX_DATA_SIZE 512
#else
    #define MAX_DATA_SIZE 2048
#endif

#define ADDRESS_SIZE     64
#define DESCRIPTOR_BYTES 3

#define SIGNATURE_CHUNK_SIZE      258
#define SIGNATURE_LAST_CHUNK_SIZE 241

#define PK_CHUNK_SIZE      258
#define PK_LAST_CHUNK_SIZE 12
#define PK_CHUNKS          11
