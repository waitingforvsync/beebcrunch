/*
 *  v7.h - the v7 codec: v6's coding inside block framing
 *
 *  v6's match coding carried by v1's framing: alternating blocks of
 *  literals and matches - always starting with literals - each prefixed
 *  with a count in [1, 256].  Matches go through v6's five learned
 *  interval tables: a unary-indexed length table (minimum value 1, at
 *  most 16 buckets), three offset contexts by length class, and the tiny
 *  length-1 offset table, with the offset bucket counts pinned by the
 *  format (bare width nibbles, no count fields).
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
 *  Every input is representable: at most 256 positions in a file are
 *  first occurrences of their byte value, so a literal run of 257 always
 *  contains a length-1 escape valve (and an over-long match run converts
 *  a match back to literals).  The encoder falls back to a full-coverage
 *  length-1 seed when the tuned seed cannot reach a needed escape; the
 *  ok flag survives only as a defensive check.
 *
 *  The encoder is v3's: the Pareto-frontier optimal parse with one extra
 *  edge per position (the nearest same-byte distance as a length-1
 *  match), refined against the tables to the same fixpoint.
 */

#ifndef V7_H_
#define V7_H_

#include <stdbool.h>

#include "richc/bytes.h"

typedef struct rc_arena rc_arena;

enum {
    v7_max_uncompressed = 65535,    // u16 header
};

typedef struct v7_compress_result {
    rc_array_bytes data;        // valid only when ok
    uint32_t       num_tokens;
    bool           ok;          // false: a run over 256 tokens was unavoidable
} v7_compress_result;

typedef struct v7_decompress_result {
    rc_array_bytes data;        // valid only when ok
    bool           ok;
} v7_decompress_result;

// in.num <= v7_max_uncompressed (asserted); scratch must be a different
// arena from arena (asserted)
v7_compress_result v7_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch);

// comp is untrusted: any malformed input returns ok == false, never traps
v7_decompress_result v7_decompress(rc_view_bytes comp, rc_arena *arena);

#endif // V7_H_
