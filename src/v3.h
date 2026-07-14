/*
 *  v3.h - the v3 codec: learned bucket tables inside block framing
 *
 *  v2's match coding carried by v1's framing: instead of a flag bit per
 *  token, alternating blocks of literals and matches - always starting
 *  with literals - each prefixed with Elias gamma(count), count in
 *  [1, 256].  Contiguous same-type runs pay one gamma header instead of
 *  one bit per token, and match offsets/lengths go through the per-file
 *  learned interval tables (three offset contexts by length class plus a
 *  length table; see tables.h).
 *
 *  Container: [u16 uncompressed_len, little-endian, raw bytes]
 *             [offset table x3][length table][blocks].
 *
 *  Match token: length first through the length table (Elias gamma bucket
 *  index + extra bits; the decoder needs the length to pick the offset
 *  context), then the offset through that context's table (flat 4-bit
 *  bucket index + extra bits).  Literal token: one byte-aligned byte.
 *  Blocks are maximal runs; no terminator - decoding stops at
 *  uncompressed_len.
 *
 *  As in v1, a same-type run longer than 256 tokens cannot be represented
 *  (the gamma cap): the parse avoids that where any alternative exists
 *  and v3_compress returns ok == false where it cannot.
 *
 *  The encoder combines v1's Pareto-frontier optimal parse (block headers
 *  make token cost depend on run length) with v2's parse <-> tables
 *  refinement fixpoint, keeping the best true size seen.
 */

#ifndef V3_H_
#define V3_H_

#include <stdbool.h>

#include "richc/bytes.h"

typedef struct rc_arena rc_arena;

enum {
    v3_max_uncompressed = 65535,    // u16 header
};

typedef struct v3_compress_result {
    rc_array_bytes data;        // valid only when ok
    uint32_t       num_tokens;
    bool           ok;          // false: a run over 256 tokens was unavoidable
} v3_compress_result;

typedef struct v3_decompress_result {
    rc_array_bytes data;        // valid only when ok
    bool           ok;
} v3_decompress_result;

// in.num <= v3_max_uncompressed (asserted); scratch must be a different
// arena from arena (asserted)
v3_compress_result v3_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch);

// comp is untrusted: any malformed input returns ok == false, never traps
v3_decompress_result v3_decompress(rc_view_bytes comp, rc_arena *arena);

#endif // V3_H_
