/*
 *  v2.h - the v2 codec: learned bucket tables, length-conditioned offsets
 *
 *  Container: [u16 uncompressed_len, little-endian, raw bytes]
 *             [offset table x3][length table][token stream].
 *
 *  Instead of universal Elias gamma codes, match offsets and lengths are
 *  coded through per-file learned interval tables (exomizer's key idea).
 *  A table is serialized as a 5-bit bucket count K (0..16), then K 4-bit
 *  widths; bucket i spans [start_i, start_i + 2^width_i), with starts
 *  accumulated from the table's minimum value (1 for offsets, 2 for
 *  lengths).  Only the widths travel in the stream - the shape of the
 *  distribution, not a symbol tree.  K == 0 only occurs for tables that
 *  are never consulted (an input with no matches).
 *
 *  Offsets are coded through one of three tables conditioned on the match
 *  length (len == 2, len == 3, len >= 4): short matches overwhelmingly
 *  use short offsets, so each class gets its own tuned table.
 *
 *  Token: 1 flag bit; 0 = literal (one byte, byte-aligned quick path),
 *  1 = match, length then offset (the decoder needs the length to select
 *  the offset context).  Length: Elias gamma(index + 1) bucket index,
 *  then extra bits.  Offset: flat 4-bit bucket index, then that bucket's
 *  width in raw extra bits.  The index asymmetry is measured: length
 *  bucket usage is heavily skewed (gamma wins), offset bucket usage is
 *  near-uniform (flat wins).  Extra fields wider than 8 bits are carried
 *  as two <= 8-bit pieces.
 *
 *  Limits: length in [2, 256], offset in [1, position]; no terminator -
 *  decoding stops at uncompressed_len.
 *
 *  The encoder parses with an exact forward DP (flag-per-token keeps
 *  token costs position-independent), refined against the tables to a
 *  fixpoint: parse under current tables, rebuild optimal tables from the
 *  parse's histograms, keep the best true size seen.
 */

#ifndef V2_H_
#define V2_H_

#include <stdbool.h>

#include "richc/bytes.h"

typedef struct rc_arena rc_arena;

enum {
    v2_max_uncompressed = 65535,    // u16 header
};

typedef struct v2_compress_result {
    rc_array_bytes data;        // complete container, allocated in arena
    uint32_t       num_tokens;
} v2_compress_result;

typedef struct v2_decompress_result {
    rc_array_bytes data;        // valid only when ok
    bool           ok;
} v2_decompress_result;

// in.num <= v2_max_uncompressed (asserted); scratch must be a different
// arena from arena (asserted)
v2_compress_result v2_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch);

// comp is untrusted: any malformed input returns ok == false, never traps
v2_decompress_result v2_decompress(rc_view_bytes comp, rc_arena *arena);

#endif // V2_H_
