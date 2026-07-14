/*
 *  v4.h - the v4 codec: v2's shape plus length-1 matches
 *
 *  v2's format (flag bit per token, learned interval tables,
 *  length-conditioned offsets) extended with matches of length 1:
 *  "repeat the byte from d ago" for a handful of bits instead of a
 *  9-bit literal.  The length table's minimum value becomes 1, and a
 *  fourth, deliberately tiny offset table serves length-1 matches
 *  (flat 2-bit bucket index, at most 4 buckets - a length-1 match only
 *  beats a literal at small offsets, and the table learns exactly that
 *  range).
 *
 *  Container: [u16 uncompressed_len, little-endian, raw bytes]
 *             [off1 table][off2 table][off3 table][off4+ table]
 *             [length table][token stream].
 *
 *  Token: 1 flag bit; 0 = literal (one byte, byte-aligned quick path),
 *  1 = match, length then offset.  Length: Elias gamma(index + 1) bucket
 *  index + extra bits, values in [1, 256].  Offset: through the table
 *  selected by the length (1 -> off1 with 2-bit index; 2 / 3 / >= 4 as
 *  v2, 4-bit index), 1 <= offset <= position.  The decode loop is v2's
 *  exactly - one more row in the context selection, and a length-1 copy
 *  is the same forward copy.
 *
 *  No terminator; decoding stops at uncompressed_len.  Encoder is v2's
 *  exact forward DP with one extra edge per position (the nearest
 *  same-byte distance from the shared match scan), refined against the
 *  tables to the same fixpoint.
 */

#ifndef V4_H_
#define V4_H_

#include <stdbool.h>

#include "richc/bytes.h"

typedef struct rc_arena rc_arena;

enum {
    v4_max_uncompressed = 65535,    // u16 header
};

typedef struct v4_compress_result {
    rc_array_bytes data;        // complete container, allocated in arena
    uint32_t       num_tokens;
} v4_compress_result;

typedef struct v4_decompress_result {
    rc_array_bytes data;        // valid only when ok
    bool           ok;
} v4_decompress_result;

// in.num <= v4_max_uncompressed (asserted); scratch must be a different
// arena from arena (asserted)
v4_compress_result v4_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch);

// comp is untrusted: any malformed input returns ok == false, never traps
v4_decompress_result v4_decompress(rc_view_bytes comp, rc_arena *arena);

#endif // V4_H_
