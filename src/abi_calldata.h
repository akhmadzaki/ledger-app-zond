/**
 * @file abi_calldata.h
 * @brief QRL ABI calldata parser — extracts function selector and decodes
 *        all Hyperion ABI-encoded parameters from raw calldata.
 *
 * **Zero dynamic memory allocation.**  All storage lives in a static pool
 * inside the .c file; the caller only provides a small stack-allocated
 * `abi_calldata_t` struct.
 *
 * Supports:
 *   - uint<M>, int<M>   (M from 8 to 256, multiples of 8)
 *   - address            (64 bytes)
 *   - bool               (uint8)
 *   - bytes<M>           (static bytes, 1 ≤ M ≤ 64)
 *   - bytes              (dynamic)
 *   - string             (dynamic UTF-8)
 *   - fixed<M>x<N>       (static fixed-point, M from 8 to 256, N from 1 to 80)
 *   - ufixed<M>x<N>      (static unsigned fixed-point)
 *   - <type>[M]          (static array)
 *   - <type>[]           (dynamic array)
 *   - tuple / (T1,T2,…)  (struct / nested tuple)
 *   - function           (bytes24: address + selector)
 *
 * Edge cases handled:
 *   - Empty calldata (no selector)
 *   - Calldata shorter than 4 bytes
 *   - Truncated / malformed calldata (buffer over-read protection)
 *   - Nested dynamic types (dynamic arrays of strings, etc.)
 *   - Deeply nested tuples
 *   - Zero-length dynamic arrays / empty strings / empty bytes
 *   - Integer overflow in offset calculations
 */

#ifndef ABI_CALLDATA_H
#define ABI_CALLDATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os.h"
#include "cx.h"
#include "sw.h"
#include "globals.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Public constants                                                   */
/* ------------------------------------------------------------------ */

/** Maximum nesting depth for tuples / dynamic types (DoS protection). */
#define ABI_MAX_DEPTH 8

/** Maximum children per composite node (array elements / tuple members). */
#define ABI_MAX_CHILDREN 32

/** Maximum length of a Hyperion type string (e.g. "uint256[][5]"). */
#define ABI_TYPE_STR_MAX 128

/** Size of one ABI slot in bytes (Zond uses 64-byte slots). */
#define ABI_SLOT_SIZE 64

/* ------------------------------------------------------------------ */
/*  Public types                                                       */
/* ------------------------------------------------------------------ */

/** Hyperion ABI elementary type tags. */
typedef enum {
    ABI_KIND_UINT,       /* uint<M>                                    */
    ABI_KIND_INT,        /* int<M>                                     */
    ABI_KIND_ADDRESS,    /* address                                    */
    ABI_KIND_BOOL,       /* bool                                       */
    ABI_KIND_BYTES_M,    /* bytes<M>  (static, 1 ≤ M ≤ 64)            */
    ABI_KIND_BYTES,      /* bytes     (dynamic)                        */
    ABI_KIND_STRING,     /* string    (dynamic)                        */
    ABI_KIND_FIXED,      /* fixed<M>x<N>                               */
    ABI_KIND_UFIXED,     /* ufixed<M>x<N>                              */
    ABI_KIND_ARRAY,      /* <elem>[<len>]  static or dynamic           */
    ABI_KIND_TUPLE,      /* (T1,T2,…)                                  */
    ABI_KIND_FUNCTION,   /* function (bytes24)                         */
    ABI_KIND_RAW,        /* raw 32-byte slot (unknown type, no sig)    */
} abi_kind_t;

/**
 * Describes one decoded parameter value.
 *
 * Composite types (arrays, tuples) reference their children by **index**
 * into the internal static node pool.  Use `abi_calldata_get_node()` to
 * resolve an index to a concrete `abi_param_t*`.
 */
typedef struct {
    abi_kind_t     kind;          /* elementary type tag                */
    const uint8_t *data;          /* pointer into original calldata     */
    size_t         data_len;      /* byte length of this value          */
    size_t         head_offset;   /* offset of head slot in calldata    */
    bool           is_dynamic;    /* true if bytes/string/dyn-array     */

    /* --- scalar helpers (valid when kind is uint/int/address/bool) -- */
    union {
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
        uint64_t u64;
        int8_t   i8;
        int16_t  i16;
        int32_t  i32;
        int64_t  i64;
        uint8_t  addr[64];
        bool     boolean;
    } scalar;

    /* --- composite children (array / tuple) --- */
    uint8_t child_count;                 /* number of children           */
    uint8_t children[ABI_MAX_CHILDREN];  /* indices into static pool     */
} abi_param_t;

/**
 * Full calldata parse result.
 *
 * After a successful parse the caller iterates top-level parameters via
 * `params[0 .. param_count-1]`, each being an index into the internal
 * node pool.  Resolve with `abi_calldata_get_node()`.
 */


/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * Parse raw QRL calldata according to a Hyperion function signature.
 *
 * **Zero heap allocations.**  All node storage comes from a static pool;
 * the caller only needs to provide a stack-allocated `abi_calldata_t`.
 *
 * @param calldata       Pointer to raw calldata bytes.
 * @param calldata_len   Length of calldata in bytes.
 * @param signature      Hyperion function signature, e.g.
 *                       "transfer(address,uint256)".
 *                       May be NULL — then only the selector is extracted
 *                       and no parameters are decoded.
 * @param out            Output structure (stack-allocated by caller).
 * @return true on success, false on parse error.
 *
 * Error conditions (return false):
 *   - calldata is NULL and calldata_len > 0
 *   - signature is malformed / unsupported type
 *   - calldata is truncated (buffer over-read would occur)
 *   - nesting exceeds ABI_MAX_DEPTH
 *   - parameter count exceeds ABI_MAX_PARAMS
 *   - node pool exhausted (too many nested elements)
 *   - integer overflow in offset arithmetic
 */
bool abi_calldata_parse(const uint8_t *calldata,
                        size_t         calldata_len,
                        const char    *signature,
                        abi_calldata_t *out);

/**
 * Resolve a node index (from `params[]` or `children[]`) to a concrete
 * `abi_param_t*` inside the static pool.  Returns NULL if the index is
 * out of range.
 *
 * The returned pointer remains valid until the next call to
 * `abi_calldata_parse()`.
 */
const abi_param_t *abi_calldata_get_node(uint8_t index);

/**
 * Convenience: pretty-print a parsed calldata tree to stdout (for debugging).
 */
void abi_calldata_dump(const abi_calldata_t *cd);

#ifdef __cplusplus
}
#endif

#endif /* ABI_CALLDATA_H */