/*
 *  v5.h - the v5 codec: v4's length-1 matches inside block framing
 *
 *  v4's match coding carried by v1's framing: alternating blocks of
 *  literals and matches - always starting with literals - each prefixed
 *  with Elias gamma(count), count in [1, 256].  Matches go through the
 *  five learned interval tables of the length-1 composition (tables.h):
 *  a length table with minimum value 1, three offset contexts by length
 *  class, and the tiny length-1 offset table.
 *
 *  Container: [u16 uncompressed_len, little-endian, raw bytes]
 *             [off1 table][offset table x3][length table][blocks].
 *
 *  Match token: length first through the length table (the decoder needs
 *  it to pick the offset context, including length 1), then the offset
 *  through that context's table.  Literal token: one byte-aligned byte.
 *  A length-1 match is a match token, so it sits in - and extends - match
 *  blocks: a lone repeated byte no longer forces a literal block break.
 *  Blocks are maximal runs; no terminator - decoding stops at
 *  uncompressed_len.
 *
 *  As in v3, a same-type run longer than 256 tokens cannot be represented
 *  (the gamma cap): the parse avoids that where any alternative exists
 *  and v5_compress returns ok == false where it cannot.  Length-1 matches
 *  make that rarer than v3: any repeated byte at a coverable distance
 *  breaks a literal run.
 *
 *  The encoder is v3's: the Pareto-frontier optimal parse with one extra
 *  edge per position (the nearest same-byte distance as a length-1
 *  match), refined against the tables to the same fixpoint.
 */

#ifndef V5_H_
#define V5_H_

#include <stdbool.h>

#include "richc/bytes.h"

typedef struct rc_arena rc_arena;

enum {
    v5_max_uncompressed = 65535,    // u16 header
};

typedef struct v5_compress_result {
    rc_array_bytes data;        // valid only when ok
    uint32_t       num_tokens;
    bool           ok;          // false: a run over 256 tokens was unavoidable
} v5_compress_result;

typedef struct v5_decompress_result {
    rc_array_bytes data;        // valid only when ok
    bool           ok;
} v5_decompress_result;

// in.num <= v5_max_uncompressed (asserted); scratch must be a different
// arena from arena (asserted)
v5_compress_result v5_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch);

// comp is untrusted: any malformed input returns ok == false, never traps
v5_decompress_result v5_decompress(rc_view_bytes comp, rc_arena *arena);

#endif // V5_H_
