/**
 * @file abi_calldata.c
 * @brief QRL ABI calldata parser implementation.
 *
 * **Zero dynamic memory allocation.**  All node storage comes from a
 * static pool (`g_abi_pool`).  The pool is reset on each call to
 * `abi_calldata_parse()`.
 */

#include "abi_calldata.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Local replacements for libc functions that pull .data symbols      */
/*  (ctype isdigit table, stdio buffers, strtoul).                     */
/* ------------------------------------------------------------------ */

static inline bool local_isdigit(char c) {
    return c >= '0' && c <= '9';
}

static size_t local_strtoul(const char *s, const char **endptr) {
    size_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (size_t) (*s - '0');
        s++;
    }
    if (endptr) *endptr = s;
    return val;
}

/* ================================================================== */
/*  Static node pool                                                   */
/* ================================================================== */

/** Total nodes available in the static pool. */
#define ABI_MAX_NODES 64

static abi_param_t g_abi_pool[ABI_MAX_NODES];
static uint16_t    g_abi_pool_count;

/** Allocate one zero-filled node from the pool.  Returns NULL if full. */
static abi_param_t *pool_alloc(void) {
    if (g_abi_pool_count >= ABI_MAX_NODES) return NULL;
    abi_param_t *node = &g_abi_pool[g_abi_pool_count];
    memset(node, 0, sizeof(*node));
    g_abi_pool_count++;
    return node;
}

/** Reset the pool (called at the start of each parse). */
static void pool_reset(void) {
    g_abi_pool_count = 0;
}

/* ================================================================== */
/*  Internal helpers — safe arithmetic                                 */
/* ================================================================== */

/** Saturating addition: returns false on overflow. */
static bool safe_add(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) return false;
    *out = a + b;
    return true;
}

/* ================================================================== */
/*  Internal helpers — calldata bounds                                 */
/* ================================================================== */

/** Check that [off, off+len) lies within calldata. */
static bool in_bounds(size_t off, size_t len, size_t calldata_len) {
    size_t end;
    if (!safe_add(off, len, &end)) return false;
    return end <= calldata_len;
}

/**
 * Read a big-endian uint256 from calldata at offset into a size_t
 * (truncated to platform width; used for lengths/offsets).
 *
 * The uint256 value is right-aligned within the ABI slot: it occupies
 * the last 32 bytes of the slot.  Validates that the upper 24 bytes
 * of that 32-byte region are zero to catch malformed offsets that
 * would overflow size_t.
 */
static bool read_u256_be(const uint8_t *cd, size_t cd_len,
                         size_t off, size_t *out) {
    size_t val_off = off + ABI_SLOT_SIZE - 32;
    if (!in_bounds(val_off, 32, cd_len)) return false;
    for (int i = 0; i < 24; i++) {
        if (cd[val_off + i] != 0) return false; /* value too large */
    }
    uint64_t lo = 0;
    for (int i = 24; i < 32; i++) {
        lo = (lo << 8) | cd[val_off + i];
    }
    *out = (size_t) lo;
    return true;
}

/* ================================================================== */
/*  Signature tokeniser                                                */
/* ================================================================== */

typedef struct {
    const char *sig;       /* original signature string                */
    size_t      pos;       /* current parse position                   */
    size_t      len;       /* length of sig                            */
} sig_ctx_t;

/** Peek current character, 0 at end. */
static char sig_peek(sig_ctx_t *ctx) {
    if (ctx->pos >= ctx->len) return '\0';
    return ctx->sig[ctx->pos];
}

/** Advance one character. */
static void sig_advance(sig_ctx_t *ctx) {
    if (ctx->pos < ctx->len) ctx->pos++;
}

/** Skip whitespace. */
static void sig_skip_ws(sig_ctx_t *ctx) {
    while (sig_peek(ctx) == ' ') sig_advance(ctx);
}

/** Consume a specific character; return false if not present. */
static bool sig_consume(sig_ctx_t *ctx, char c) {
    if (sig_peek(ctx) != c) return false;
    sig_advance(ctx);
    return true;
}

/**
 * Parse a single type token from the signature into `out`.
 * Returns the number of characters consumed, or 0 on error.
 *
 * Handles:
 *   uint<M>  int<M>  address  bool  bytes<M>  bytes  string
 *   fixed<M>x<N>  ufixed<M>x<N>  function
 *   <type>[<N>]  <type>[]   (arrays)
 *   (T1,T2,…)   (tuples)
 */
static size_t sig_parse_type(sig_ctx_t *ctx, char *out, size_t out_sz) {
    size_t start = ctx->pos;
    size_t w = 0;

#define EMIT(c) do { \
    if (w + 1 < out_sz) out[w] = (c); \
    w++; \
} while(0)

    char c = sig_peek(ctx);

    /* --- tuple ---------------------------------------------------- */
    if (c == '(') {
        EMIT('(');
        sig_advance(ctx);
        int depth = 1;
        while (depth > 0) {
            c = sig_peek(ctx);
            if (c == '\0') return 0; /* unterminated tuple */
            if (c == '(') depth++;
            if (c == ')') depth--;
            EMIT(c);
            sig_advance(ctx);
        }
        /* Now parse any array suffix after the tuple */
        goto array_suffix;
    }

    /* --- elementary types ----------------------------------------- */
    if (strncmp(ctx->sig + ctx->pos, "uint", 4) == 0) {
        for (int i = 0; i < 4; i++) { EMIT(ctx->sig[ctx->pos]); sig_advance(ctx); }
        while (local_isdigit(sig_peek(ctx))) {
            EMIT(sig_peek(ctx)); sig_advance(ctx);
        }
    } else if (strncmp(ctx->sig + ctx->pos, "int", 3) == 0) {
        for (int i = 0; i < 3; i++) { EMIT(ctx->sig[ctx->pos]); sig_advance(ctx); }
        while (local_isdigit(sig_peek(ctx))) {
            EMIT(sig_peek(ctx)); sig_advance(ctx);
        }
    } else if (strncmp(ctx->sig + ctx->pos, "address", 7) == 0) {
        for (int i = 0; i < 7; i++) { EMIT(ctx->sig[ctx->pos]); sig_advance(ctx); }
    } else if (strncmp(ctx->sig + ctx->pos, "bool", 4) == 0) {
        for (int i = 0; i < 4; i++) { EMIT(ctx->sig[ctx->pos]); sig_advance(ctx); }
    } else if (strncmp(ctx->sig + ctx->pos, "bytes", 5) == 0) {
        for (int i = 0; i < 5; i++) { EMIT(ctx->sig[ctx->pos]); sig_advance(ctx); }
        while (local_isdigit(sig_peek(ctx))) {
            EMIT(sig_peek(ctx)); sig_advance(ctx);
        }
    } else if (strncmp(ctx->sig + ctx->pos, "string", 6) == 0) {
        for (int i = 0; i < 6; i++) { EMIT(ctx->sig[ctx->pos]); sig_advance(ctx); }
    } else if (strncmp(ctx->sig + ctx->pos, "fixed", 5) == 0) {
        for (int i = 0; i < 5; i++) { EMIT(ctx->sig[ctx->pos]); sig_advance(ctx); }
        while (local_isdigit(sig_peek(ctx))) {
            EMIT(sig_peek(ctx)); sig_advance(ctx);
        }
        if (sig_peek(ctx) == 'x') { EMIT('x'); sig_advance(ctx); }
        while (local_isdigit(sig_peek(ctx))) {
            EMIT(sig_peek(ctx)); sig_advance(ctx);
        }
    } else if (strncmp(ctx->sig + ctx->pos, "ufixed", 6) == 0) {
        for (int i = 0; i < 6; i++) { EMIT(ctx->sig[ctx->pos]); sig_advance(ctx); }
        while (local_isdigit(sig_peek(ctx))) {
            EMIT(sig_peek(ctx)); sig_advance(ctx);
        }
        if (sig_peek(ctx) == 'x') { EMIT('x'); sig_advance(ctx); }
        while (local_isdigit(sig_peek(ctx))) {
            EMIT(sig_peek(ctx)); sig_advance(ctx);
        }
    } else if (strncmp(ctx->sig + ctx->pos, "function", 8) == 0) {
        for (int i = 0; i < 8; i++) { EMIT(ctx->sig[ctx->pos]); sig_advance(ctx); }
    } else {
        return 0; /* unknown type */
    }

array_suffix:
    /* --- array brackets --- */
    while (sig_peek(ctx) == '[') {
        EMIT('[');
        sig_advance(ctx);
        while (local_isdigit(sig_peek(ctx))) {
            EMIT(sig_peek(ctx)); sig_advance(ctx);
        }
        if (!sig_consume(ctx, ']')) return 0; /* missing ] */
        EMIT(']');
    }

#undef EMIT

    if (out) out[w < out_sz ? w : out_sz - 1] = '\0';
    return ctx->pos - start;
}

/* ================================================================== */
/*  Type classification                                               */
/* ================================================================== */

/**
 * Classify a type string and extract its bit-size.
 * `type_str` is a single type token WITHOUT array brackets,
 * e.g. "uint256", "bytes32", "address", "(uint256,bytes)".
 *
 * Array brackets are handled by the caller via strip_array().
 */
static bool classify_type(const char  *type_str,
                          abi_kind_t  *kind,
                          size_t      *bits,
                          bool        *is_dynamic)
{
    *bits       = 256;
    *is_dynamic = false;

    const char *s = type_str;

    /* --- tuple --- */
    if (*s == '(') {
        *kind = ABI_KIND_TUPLE;
        /* A tuple is dynamic if any of its members is dynamic.
         * We can't know that without recursive parsing, so we
         * conservatively mark it dynamic — the decoder will
         * handle it correctly either way. */
        *is_dynamic = true;
        return true;
    }

    /* --- uint / int --- */
    if (strncmp(s, "uint", 4) == 0) {
        *kind = ABI_KIND_UINT;
        s += 4;
        if (local_isdigit(*s)) {
            *bits = local_strtoul(s, NULL);
        } else {
            *bits = 256;
        }
        if (*bits < 8 || *bits > 256 || (*bits % 8) != 0) return false;
        return true;
    }
    if (strncmp(s, "int", 3) == 0) {
        *kind = ABI_KIND_INT;
        s += 3;
        if (local_isdigit(*s)) {
            *bits = local_strtoul(s, NULL);
        } else {
            *bits = 256;
        }
        if (*bits < 8 || *bits > 256 || (*bits % 8) != 0) return false;
        return true;
    }

    /* --- address --- */
    if (strcmp(s, "address") == 0) {
        *kind  = ABI_KIND_ADDRESS;
        *bits  = 512;
        return true;
    }

    /* --- bool --- */
    if (strcmp(s, "bool") == 0) {
        *kind  = ABI_KIND_BOOL;
        *bits  = 8;
        return true;
    }

    /* --- bytes<M> / bytes --- */
    if (strncmp(s, "bytes", 5) == 0) {
        s += 5;
        if (local_isdigit(*s)) {
            *kind = ABI_KIND_BYTES_M;
            *bits = local_strtoul(s, NULL) * 8;
            if (*bits < 8 || *bits > 512) return false;
        } else {
            *kind       = ABI_KIND_BYTES;
            *is_dynamic = true;
        }
        return true;
    }

    /* --- string --- */
    if (strcmp(s, "string") == 0) {
        *kind       = ABI_KIND_STRING;
        *is_dynamic = true;
        return true;
    }

    /* --- fixed / ufixed --- */
    if (strncmp(s, "ufixed", 6) == 0 || strncmp(s, "fixed", 5) == 0) {
        *kind = (*s == 'u') ? ABI_KIND_UFIXED : ABI_KIND_FIXED;
        s += (*s == 'u') ? 6 : 5;
        *bits = local_strtoul(s, NULL);
        if (*bits < 8 || *bits > 256 || (*bits % 8) != 0) return false;
        return true;
    }

    /* --- function --- */
    if (strcmp(s, "function") == 0) {
        *kind = ABI_KIND_FUNCTION;
        *bits = 192; /* 24 bytes */
        return true;
    }

    return false;
}

/**
 * Strip the outermost array brackets from a type string.
 * e.g. "uint256[][5]" → inner="uint256[]", array_len=5
 *      "bytes[]"       → inner="bytes",    array_len=0 (dynamic)
 * Returns true if this is an array type.
 *
 * NOTE: `inner` points into `type_str`; the caller must copy if needed.
 */
static bool strip_array(const char *type_str,
                        const char **inner,
                        size_t      *array_len)
{
    const char *bracket = strrchr(type_str, '[');
    if (!bracket) return false;

    *inner     = type_str;
    *array_len = 0;

    const char *num = bracket + 1;
    if (*num == ']') {
        *array_len = 0; /* dynamic */
    } else {
        *array_len = local_strtoul(num, NULL);
    }
    return true;
}

/**
 * Check whether a type string (which may include array brackets)
 * represents a dynamic type.  Used by the tuple decoder to decide
 * whether the tuple head contains offsets or inline data.
 */
static bool type_is_dynamic(const char *type_str) {
    /* Tuples are always treated as dynamic (conservative). */
    if (type_str[0] == '(') return true;

    /* Dynamic arrays (ending with []) are dynamic. */
    const char *s = type_str;
    while (*s) {
        if (*s == '[') {
            s++;
            if (*s == ']') return true;  /* dynamic array */
            while (local_isdigit(*s)) s++;
            if (*s == ']') s++;  /* static array — keep looking */
            continue;
        }
        s++;
    }

    /* Check the base type. */
    abi_kind_t kind;
    size_t     bits;
    bool       is_dyn;
    if (!classify_type(type_str, &kind, &bits, &is_dyn)) return true; /* unknown → conservative */
    return is_dyn;
}

/* ================================================================== */
/*  Core decoder                                                      */
/* ================================================================== */

/**
 * Decode a single value at `head_off` according to `type_str`.
 *
 * @param cd, cd_len   Calldata buffer and length.
 * @param type_str     Hyperion type string (may include array brackets).
 * @param head_off     Offset in calldata of the head slot (ABI_SLOT_SIZE bytes).
 * @param consumed     [out] Number of head slots consumed (1 for static,
 *                     1 for dynamic — the offset slot itself).
 * @param out          [out] Filled-in parameter (must point to a pool node
 *                     or a caller-provided abi_param_t).
 * @param depth        Current nesting depth (for DoS protection).
 * @return true on success.
 */
static bool decode_value(const uint8_t *cd, size_t cd_len,
                         const char *type_str,
                         size_t head_off,
                         size_t *consumed,
                         abi_param_t *out,
                         int depth)
{
    if (depth > ABI_MAX_DEPTH) return false;

    memset(out, 0, sizeof(*out));
    out->head_offset = head_off;

    /* --- Handle array suffix on the type string --- */
    const char *inner_type = NULL;
    size_t      array_len  = 0;

    if (strip_array(type_str, &inner_type, &array_len)) {
        /* Extract inner type string (everything before the last '[') */
        size_t inner_len = (size_t)(strrchr(type_str, '[') - type_str);
        char inner_buf[ABI_TYPE_STR_MAX];
        if (inner_len >= sizeof(inner_buf)) return false;
        memcpy(inner_buf, type_str, inner_len);
        inner_buf[inner_len] = '\0';

        out->kind = ABI_KIND_ARRAY;

        if (array_len == 0) {
            /* --- dynamic array --- */
            out->is_dynamic = true;
            size_t tail_off;
            if (!read_u256_be(cd, cd_len, head_off, &tail_off)) return false;
            size_t elem_count;
            if (!read_u256_be(cd, cd_len, tail_off, &elem_count)) return false;

            /* Sanity: element count must be reasonable */
            if (elem_count > ABI_MAX_CHILDREN) return false;

            out->child_count = (uint8_t) elem_count;

            bool inner_dyn = type_is_dynamic(inner_buf);

            size_t cursor = tail_off + ABI_SLOT_SIZE; /* skip length prefix */

            if (inner_dyn) {
                /* Elements are dynamic: head has offsets */
                for (size_t i = 0; i < elem_count; i++) {
                    size_t elem_off;
                    if (!read_u256_be(cd, cd_len, cursor, &elem_off)) return false;
                    abi_param_t *child = pool_alloc();
                    if (!child) return false;
                    out->children[i] = (uint8_t)(child - g_abi_pool);
                    size_t elem_consumed;
                    if (!decode_value(cd, cd_len, inner_buf,
                                      elem_off, &elem_consumed,
                                      child, depth + 1)) return false;
                    cursor += ABI_SLOT_SIZE;
                }
            } else {
                /* Elements are static: in-place slots */
                for (size_t i = 0; i < elem_count; i++) {
                    abi_param_t *child = pool_alloc();
                    if (!child) return false;
                    out->children[i] = (uint8_t)(child - g_abi_pool);
                    size_t elem_consumed;
                    if (!decode_value(cd, cd_len, inner_buf,
                                      cursor, &elem_consumed,
                                      child, depth + 1)) return false;
                    cursor += elem_consumed * ABI_SLOT_SIZE;
                }
            }
        } else {
            /* --- static array --- */
            if (array_len > ABI_MAX_CHILDREN) return false;

            out->child_count = (uint8_t) array_len;

            bool inner_dyn = type_is_dynamic(inner_buf);

            size_t cursor = head_off;

            if (inner_dyn) {
                /* Static array of dynamic elements: head slots are offsets */
                for (size_t i = 0; i < array_len; i++) {
                    size_t elem_off;
                    if (!read_u256_be(cd, cd_len, cursor, &elem_off)) return false;
                    abi_param_t *child = pool_alloc();
                    if (!child) return false;
                    out->children[i] = (uint8_t)(child - g_abi_pool);
                    size_t elem_consumed;
                    if (!decode_value(cd, cd_len, inner_buf,
                                      elem_off, &elem_consumed,
                                      child, depth + 1)) return false;
                    cursor += ABI_SLOT_SIZE;
                }
            } else {
                /* Static array of static elements: in-place */
                for (size_t i = 0; i < array_len; i++) {
                    abi_param_t *child = pool_alloc();
                    if (!child) return false;
                    out->children[i] = (uint8_t)(child - g_abi_pool);
                    size_t elem_consumed;
                    if (!decode_value(cd, cd_len, inner_buf,
                                      cursor, &elem_consumed,
                                      child, depth + 1)) return false;
                    cursor += elem_consumed * ABI_SLOT_SIZE;
                }
            }
        }

        *consumed = (array_len == 0) ? 1 : array_len; /* head slots */
        return true;
    }

    /* --- Classify the elementary type --- */
    abi_kind_t kind;
    size_t     bits;
    bool       is_dyn;

    if (!classify_type(type_str, &kind, &bits, &is_dyn))
        return false;

    out->kind = kind;
    out->is_dynamic = is_dyn;

    switch (kind) {
    /* ===== static scalars (1 slot) ================================= */
    case ABI_KIND_UINT:
    case ABI_KIND_INT:
    case ABI_KIND_BOOL:
    case ABI_KIND_ADDRESS:
    case ABI_KIND_BYTES_M:
    case ABI_KIND_FIXED:
    case ABI_KIND_UFIXED:
    case ABI_KIND_FUNCTION: {
        if (!in_bounds(head_off, ABI_SLOT_SIZE, cd_len)) return false;
        out->data     = cd + head_off;
        out->data_len = ABI_SLOT_SIZE;

        /* Populate scalar union for common types.
         * Values are right-aligned within the 64-byte slot, so the
         * low 8 bytes sit at bytes [ABI_SLOT_SIZE-8 .. ABI_SLOT_SIZE-1]. */
        if (kind == ABI_KIND_UINT || kind == ABI_KIND_INT) {
            uint64_t v = 0;
            for (int i = (int)(ABI_SLOT_SIZE - 8); i < (int)ABI_SLOT_SIZE; i++)
                v = (v << 8) | cd[head_off + i];
            if (kind == ABI_KIND_UINT) {
                out->scalar.u64 = v;
            } else {
                out->scalar.i64 = (int64_t) v;
            }
            out->scalar.u32 = (uint32_t) v;
            out->scalar.u16 = (uint16_t) v;
            out->scalar.u8  = (uint8_t)  v;
            out->scalar.i32 = (int32_t)  v;
            out->scalar.i16 = (int16_t)  v;
            out->scalar.i8  = (int8_t)   v;
        } else if (kind == ABI_KIND_BOOL) {
            out->scalar.boolean = (cd[head_off + ABI_SLOT_SIZE - 1] != 0);
        } else if (kind == ABI_KIND_ADDRESS) {
            memcpy(out->scalar.addr, cd + head_off, 64);
        }
        *consumed = 1;
        return true;
    }

    /* ===== dynamic bytes =========================================== */
    case ABI_KIND_BYTES: {
        out->is_dynamic = true;
        size_t tail_off;
        if (!read_u256_be(cd, cd_len, head_off, &tail_off)) return false;
        size_t byte_len;
        if (!read_u256_be(cd, cd_len, tail_off, &byte_len)) return false;
        if (!in_bounds(tail_off + ABI_SLOT_SIZE, byte_len, cd_len)) return false;
        out->data     = cd + tail_off + ABI_SLOT_SIZE;
        out->data_len = byte_len;
        *consumed = 1;
        return true;
    }

    /* ===== dynamic string ========================================== */
    case ABI_KIND_STRING: {
        out->is_dynamic = true;
        size_t tail_off;
        if (!read_u256_be(cd, cd_len, head_off, &tail_off)) return false;
        size_t byte_len;
        if (!read_u256_be(cd, cd_len, tail_off, &byte_len)) return false;
        if (!in_bounds(tail_off + ABI_SLOT_SIZE, byte_len, cd_len)) return false;
        out->data     = cd + tail_off + ABI_SLOT_SIZE;
        out->data_len = byte_len;
        *consumed = 1;
        return true;
    }

    /* ===== tuple =================================================== */
    case ABI_KIND_TUPLE: {
        /* Parse the tuple members from the type string.
         * type_str looks like "(uint256,bytes,address)" */
        const char *p = type_str;
        if (*p != '(') return false;
        p++; /* skip '(' */

        /* First pass: count members */
        size_t member_count = 0;
        {
            sig_ctx_t tmp_ctx = { .sig = p, .pos = 0, .len = strlen(p) };
            char dummy[ABI_TYPE_STR_MAX];
            while (sig_peek(&tmp_ctx) != ')' && sig_peek(&tmp_ctx) != '\0') {
                sig_skip_ws(&tmp_ctx);
                if (sig_peek(&tmp_ctx) == ')' || sig_peek(&tmp_ctx) == '\0') break;
                size_t consumed_sig = sig_parse_type(&tmp_ctx, dummy, sizeof(dummy));
                if (consumed_sig == 0) return false;
                member_count++;
                sig_skip_ws(&tmp_ctx);
                if (sig_peek(&tmp_ctx) == ',') sig_advance(&tmp_ctx);
            }
        }

        if (member_count > ABI_MAX_CHILDREN) return false;

        out->child_count = (uint8_t) member_count;

        if (member_count == 0) {
            /* Empty tuple — static, 0 slots */
            *consumed = 0;
            return true;
        }

        /* Determine if tuple is dynamic: check if any member is dynamic */
        bool tuple_is_dynamic = false;
        {
            sig_ctx_t tmp_ctx = { .sig = p, .pos = 0, .len = strlen(p) };
            char memb[ABI_TYPE_STR_MAX];
            for (size_t i = 0; i < member_count; i++) {
                sig_skip_ws(&tmp_ctx);
                sig_parse_type(&tmp_ctx, memb, sizeof(memb));
                sig_skip_ws(&tmp_ctx);
                if (sig_peek(&tmp_ctx) == ',') sig_advance(&tmp_ctx);

                if (type_is_dynamic(memb)) {
                    tuple_is_dynamic = true;
                    break;
                }
            }
        }

        out->is_dynamic = tuple_is_dynamic;

        size_t cursor;
        if (tuple_is_dynamic) {
            /* Head contains a single offset to the tail */
            if (!read_u256_be(cd, cd_len, head_off, &cursor)) return false;
        } else {
            cursor = head_off;
        }

        /* Second pass: decode each member */
        {
            sig_ctx_t tmp_ctx = { .sig = p, .pos = 0, .len = strlen(p) };
            char memb[ABI_TYPE_STR_MAX];
            for (size_t i = 0; i < member_count; i++) {
                sig_skip_ws(&tmp_ctx);
                sig_parse_type(&tmp_ctx, memb, sizeof(memb));
                sig_skip_ws(&tmp_ctx);
                if (sig_peek(&tmp_ctx) == ',') sig_advance(&tmp_ctx);

                abi_param_t *child = pool_alloc();
                if (!child) return false;
                out->children[i] = (uint8_t)(child - g_abi_pool);

                size_t memb_consumed;
                if (!decode_value(cd, cd_len, memb,
                                  cursor, &memb_consumed,
                                  child, depth + 1)) return false;
                cursor += memb_consumed * ABI_SLOT_SIZE;
            }
        }

        *consumed = tuple_is_dynamic ? 1 : (cursor - head_off) / ABI_SLOT_SIZE;
        return true;
    }

    default:
        return false;
    }
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

bool abi_calldata_parse(const uint8_t *calldata,
                        size_t         calldata_len,
                        const char    *signature,
                        abi_calldata_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    /* Reset the static node pool for this parse. */
    pool_reset();

    /* NULL calldata with non-zero length is invalid */
    if (!calldata && calldata_len > 0) return false;

    /* Empty calldata: no selector, no params */
    if (calldata_len == 0) {
        out->has_selector = false;
        out->param_count  = 0;
        return true;
    }

    /* Extract selector (or as much as available) */
    if (calldata_len >= 4) {
        memcpy(out->selector, calldata, 4);
        out->has_selector = true;
    } else {
        /* Calldata shorter than 4 bytes: copy what we have, zero-pad */
        memcpy(out->selector, calldata, calldata_len);
        memset(out->selector + calldata_len, 0, (size_t)(4 - calldata_len));
        out->has_selector = false;
        out->param_count  = 0;
        return true;
    }

    /* If no signature provided, treat remaining calldata as raw slots */
    if (!signature || signature[0] == '\0') {
        size_t data_len   = calldata_len - 4;
        size_t slot_count = data_len / ABI_SLOT_SIZE;
        size_t remainder  = data_len % ABI_SLOT_SIZE;

        size_t total_slots = slot_count + (remainder > 0 ? 1 : 0);
        if (total_slots > ABI_MAX_PARAMS) total_slots = ABI_MAX_PARAMS;

        out->param_count = (uint8_t) total_slots;

        for (size_t i = 0; i < slot_count && i < ABI_MAX_PARAMS; i++) {
            abi_param_t *node = pool_alloc();
            if (!node) return false;
            out->params[i] = (uint8_t)(node - g_abi_pool);
            node->kind     = ABI_KIND_RAW;
            node->data     = calldata + 4 + i * ABI_SLOT_SIZE;
            node->data_len = ABI_SLOT_SIZE;
        }

        if (remainder > 0 && slot_count < ABI_MAX_PARAMS) {
            abi_param_t *node = pool_alloc();
            if (!node) return false;
            out->params[slot_count] = (uint8_t)(node - g_abi_pool);
            node->kind     = ABI_KIND_RAW;
            node->data     = calldata + 4 + slot_count * ABI_SLOT_SIZE;
            node->data_len = remainder;
        }

        return true;
    }

    /* Parse the signature: skip function name, extract parameter list */
    const char *paren = strchr(signature, '(');
    if (!paren) return false;
    const char *closing = strchr(paren, ')');
    if (!closing) return false;

    /* Count parameters */
    size_t param_count = 0;
    {
        sig_ctx_t ctx = { .sig = paren + 1, .pos = 0,
                          .len = (size_t)(closing - paren - 1) };
        char dummy[ABI_TYPE_STR_MAX];
        while (sig_peek(&ctx) != '\0') {
            sig_skip_ws(&ctx);
            if (sig_peek(&ctx) == '\0') break;
            size_t consumed = sig_parse_type(&ctx, dummy, sizeof(dummy));
            if (consumed == 0) return false;
            param_count++;
            sig_skip_ws(&ctx);
            sig_consume(&ctx, ',');
        }
    }

    if (param_count > ABI_MAX_PARAMS) return false;

    out->param_count = (uint8_t) param_count;
    if (param_count == 0) {
        return true;
    }

    /* Decode each parameter starting after the 4-byte selector */
    size_t cursor = 4;
    sig_ctx_t ctx = { .sig = paren + 1, .pos = 0,
                      .len = (size_t)(closing - paren - 1) };
    char type_buf[ABI_TYPE_STR_MAX];

    for (size_t i = 0; i < param_count; i++) {
        sig_skip_ws(&ctx);
        size_t consumed_sig = sig_parse_type(&ctx, type_buf, sizeof(type_buf));
        if (consumed_sig == 0) return false;
        sig_skip_ws(&ctx);
        sig_consume(&ctx, ',');

        /* Allocate a pool node for this top-level parameter */
        abi_param_t *param_node = pool_alloc();
        if (!param_node) return false;
        out->params[i] = (uint8_t)(param_node - g_abi_pool);

        size_t slots_consumed;
        if (!decode_value(calldata, calldata_len, type_buf,
                          cursor, &slots_consumed,
                          param_node, 0)) return false;
        cursor += slots_consumed * ABI_SLOT_SIZE;
    }

    return true;
}

const abi_param_t *abi_calldata_get_node(uint8_t index) {
    if (index >= g_abi_pool_count) return NULL;
    return &g_abi_pool[index];
}

static void dump_param(const abi_param_t *p, int indent) {
    if (!p) { PRINTF("(null)\n"); return; }

    for (int i = 0; i < indent; i++) PRINTF("  ");

    switch (p->kind) {
    case ABI_KIND_UINT:
        PRINTF("uint: %llu\n", (unsigned long long) p->scalar.u64);
        break;
    case ABI_KIND_INT:
        PRINTF("int: %lld\n", (long long) p->scalar.i64);
        break;
    case ABI_KIND_ADDRESS:
        PRINTF("address: 0x");
        for (int i = 0; i < 64; i++) PRINTF("%02x", p->scalar.addr[i]);
        PRINTF("\n");
        break;
    case ABI_KIND_BOOL:
        PRINTF("bool: %s\n", p->scalar.boolean ? "true" : "false");
        break;
    case ABI_KIND_BYTES_M:
        PRINTF("bytes%u: 0x", p->data_len);
        for (size_t i = 0; i < p->data_len && i < (size_t)ABI_SLOT_SIZE; i++)
            PRINTF("%02x", p->data[i]);
        PRINTF("\n");
        break;
    case ABI_KIND_BYTES:
        PRINTF("bytes (dynamic, len=%u): 0x", p->data_len);
        for (size_t i = 0; i < p->data_len && i < (size_t)(ABI_SLOT_SIZE * 2); i++)
            PRINTF("%02x", p->data[i]);
        if (p->data_len > (size_t)(ABI_SLOT_SIZE * 2)) PRINTF("...");
        PRINTF("\n");
        break;
    case ABI_KIND_STRING:
        PRINTF("string: \"");
        for (size_t i = 0; i < p->data_len && i < 128; i++) {
            char c = (char) p->data[i];
            if (c >= 0x20 && c < 0x7f) PRINTF("%c", c);
            else PRINTF("\\x%02x", (unsigned char) c);
        }
        if (p->data_len > 128) PRINTF("...");
        PRINTF("\"\n");
        break;
    case ABI_KIND_ARRAY:
        PRINTF("array [%u]:\n", p->child_count);
        for (uint8_t i = 0; i < p->child_count; i++) {
            for (int j = 0; j < indent + 1; j++) PRINTF("  ");
            PRINTF("[%u]: ", i);
            dump_param(abi_calldata_get_node(p->children[i]), indent + 2);
        }
        break;
    case ABI_KIND_TUPLE:
        PRINTF("tuple (%u fields):\n", p->child_count);
        for (uint8_t i = 0; i < p->child_count; i++) {
            dump_param(abi_calldata_get_node(p->children[i]), indent + 1);
        }
        break;
    case ABI_KIND_FIXED:
    case ABI_KIND_UFIXED:
        PRINTF("fixed: 0x");
        for (size_t i = 0; i < (size_t)ABI_SLOT_SIZE; i++)
            PRINTF("%02x", p->data[i]);
        PRINTF("\n");
        break;
    case ABI_KIND_FUNCTION:
        PRINTF("function: 0x");
        for (size_t i = 0; i < 24; i++) PRINTF("%02x", p->data[i]);
        PRINTF("\n");
        break;
    case ABI_KIND_RAW:
        PRINTF("raw (len= %u): 0x", p->data_len);
        for (size_t i = 0; i < p->data_len && i < 64; i++)
            PRINTF("%02x", p->data[i]);
        if (p->data_len > 64) PRINTF("...");
        PRINTF("\n");
        break;
    default:
        PRINTF("unknown kind\n");
        break;
    }
}

void abi_calldata_dump(const abi_calldata_t *cd) {
    if (!cd) { PRINTF("(null)\n"); return; }
    if (cd->has_selector) {
        PRINTF("Selector: 0x%02x%02x%02x%02x\n",
               cd->selector[0], cd->selector[1],
               cd->selector[2], cd->selector[3]);
    } else {
        PRINTF("Selector: (none — calldata < 4 bytes)\n");
    }
    PRINTF("Parameters: %u\n", cd->param_count);
    for (uint8_t i = 0; i < cd->param_count; i++) {
        PRINTF("[%u] ", i);
        dump_param(abi_calldata_get_node(cd->params[i]), 0);
    }
}
