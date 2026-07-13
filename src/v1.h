/*
 *  v1.h - the v1 codec: block-framed tokens
 *
 *  Container: [u16 uncompressed_len, little-endian, raw bytes]
 *             [3 bits B-1][blocks], bit fields packed as in bitwriter.
 *
 *  Blocks alternate literal, match, literal, ... always starting with a
 *  literal block (a match cannot exist at position 0).  Each block is
 *  Elias gamma(count), count in [1, 256], then count tokens.  Blocks are
 *  maximal same-type runs, so counts are never 0 and the alternation is
 *  unambiguous; the per-token flag bit of v0 is consolidated into one
 *  gamma header per run.
 *
 *  Literal token: one byte-aligned byte.  Match token: as v0 - Elias
 *  gamma(((offset - 1) >> B) + 1), then the low B bits of (offset - 1) as
 *  raw bits, then Elias gamma(length - 1), with 2 <= length <= 256,
 *  1 <= offset <= position and ((offset - 1) >> B) + 1 <= 256.  There is
 *  no terminator; decoding stops at uncompressed_len.
 *
 *  A same-type run longer than 256 tokens cannot be represented (the gamma
 *  cap): the parse avoids that where any alternative token exists, and
 *  v1_compress returns ok == false where it cannot (representing longer
 *  runs is deliberately deferred).
 *
 *  The encoder uses an optimal forward parse.  Block headers make a
 *  token's cost depend on its run length, so each position keeps a small
 *  Pareto frontier of (run length, cost) states per token type; the parse
 *  is exact for the format under its run-cap constraint, evaluated for
 *  every B in 1..8 with the jointly best pair kept.
 */

#ifndef V1_H_
#define V1_H_

#include <stdbool.h>

#include "richc/bytes.h"

typedef struct rc_arena rc_arena;

enum {
    v1_max_uncompressed = 65535,    // u16 header
};

typedef struct v1_compress_result {
    rc_array_bytes data;        // valid only when ok
    uint32_t       b;           // chosen offset split, 1..8
    uint32_t       num_tokens;
    bool           ok;          // false: a run over 256 tokens was unavoidable
} v1_compress_result;

typedef struct v1_decompress_result {
    rc_array_bytes data;        // valid only when ok
    bool           ok;
} v1_decompress_result;

// in.num <= v1_max_uncompressed (asserted); scratch must be a different
// arena from arena (asserted)
v1_compress_result v1_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch);

// comp is untrusted: any malformed input returns ok == false, never traps
v1_decompress_result v1_decompress(rc_view_bytes comp, rc_arena *arena);

#endif // V1_H_
