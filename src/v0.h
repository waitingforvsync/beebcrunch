/*
 *  v0.h - the v0 codec: the simplest beebcrunch format
 *
 *  Container: [u16 uncompressed_len, little-endian, raw bytes]
 *             [3 bits B-1][token stream], bits MSB-first.  B is in 1..8.
 *
 *  Token: 1 flag bit; 0 = literal (one byte, byte-aligned quick path),
 *  1 = match with 2 <= length <= 256 and 1 <= offset <= position, coded
 *  as the offset then the length: Elias gamma(((offset - 1) >> B) + 1),
 *  then the low B bits of (offset - 1) as raw bits (gamma first so a 6502
 *  decoder shifts the raw bits straight up into a 16-bit value), then
 *  Elias gamma(length - 1).  There is no terminator; decoding stops at
 *  uncompressed_len.
 *
 *  Match lengths cap at 256 so a 6502 decoder can keep the length in one
 *  byte (256 wraps to 0, which is fine: 0 is not a valid length).
 *
 *  The encoder uses an optimal forward parse (exact for this format given
 *  nearest-offset-per-length match candidates), evaluated for every B in
 *  1..8; the (parse, B) pair with the smallest encoding wins.
 */

#ifndef V0_H_
#define V0_H_

#include <stdbool.h>

#include "richc/bytes.h"

typedef struct rc_arena rc_arena;

enum {
    v0_max_uncompressed = 65535,    // u16 header
};

typedef struct v0_compress_result {
    rc_array_bytes data;        // complete container, allocated in arena
    uint32_t       b;           // chosen offset split, 1..8
    uint32_t       num_tokens;
} v0_compress_result;

typedef struct v0_decompress_result {
    rc_array_bytes data;        // valid only when ok
    bool           ok;
} v0_decompress_result;

// in.num <= v0_max_uncompressed (asserted); scratch must be a different
// arena from arena (asserted)
v0_compress_result v0_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch);

// comp is untrusted: any malformed input returns ok == false, never traps
v0_decompress_result v0_decompress(rc_view_bytes comp, rc_arena *arena);

#endif // V0_H_
