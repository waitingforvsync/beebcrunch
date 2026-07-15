/*
 *  v6.h - the v6 codec: v4 with a unary length bucket index
 *
 *  v4's format exactly, except the length table's bucket index is unary
 *  (exomizer's choice) instead of Elias gamma, capped at 16 buckets
 *  (indices 0-15): index i costs i + 1 bits, so indices 1 and 3 - where
 *  short-match lengths live - cost 2 and 4 bits against gamma's 3 and 5,
 *  in exchange for getting worse past index 5.  On the 6502 unary is
 *  also the cheapest possible index loop.
 *
 *  Container: [u16 uncompressed_len, little-endian, raw bytes]
 *             [off1 table][off2 table][off3 table][off4+ table]
 *             [length table][token stream].  The four offset tables have
 *  format-pinned bucket counts (4 / 16 / 16 / 16) and serialize as bare
 *  width nibbles, exomizer-style; only the length table carries a 5-bit
 *  bucket count.
 *
 *  Token: 1 flag bit; 0 = literal (one byte, byte-aligned quick path),
 *  1 = match, length then offset.  Length: unary bucket index (i zeros
 *  then a 1) + extra bits, values in [1, 256].  Offset: through the
 *  table selected by the length (1 -> off1 with 2-bit index; 2 / 3 /
 *  >= 4 as v2, 4-bit index), 1 <= offset <= position.
 *
 *  No terminator; decoding stops at uncompressed_len.  Encoder is v2's
 *  exact forward DP with one extra edge per position (the nearest
 *  same-byte distance from the shared match scan), refined against the
 *  tables to the same fixpoint.
 */

#ifndef V6_H_
#define V6_H_

#include <stdbool.h>

#include "richc/bytes.h"

typedef struct rc_arena rc_arena;

enum {
    v6_max_uncompressed = 65535,    // u16 header
};

typedef struct v6_compress_result {
    rc_array_bytes data;        // complete container, allocated in arena
    uint32_t       num_tokens;
} v6_compress_result;

typedef struct v6_decompress_result {
    rc_array_bytes data;        // valid only when ok
    bool           ok;
} v6_decompress_result;

// in.num <= v6_max_uncompressed (asserted); scratch must be a different
// arena from arena (asserted)
v6_compress_result v6_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch);

// comp is untrusted: any malformed input returns ok == false, never traps
v6_decompress_result v6_decompress(rc_view_bytes comp, rc_arena *arena);

#endif // V6_H_
