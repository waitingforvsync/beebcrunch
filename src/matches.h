/*
 *  matches.h - shared token type and breakpoint match scan
 *
 *  Every codec parses the same raw material: for each input position, a
 *  cached breakpoint list of matches with strictly increasing length, each
 *  at the nearest offset achieving that length.  Offsets also increase
 *  strictly down each list (chains are walked nearest-first and an entry
 *  is only kept when it beats every earlier length), so the lists are
 *  pruned by construction.  Codec offset codes are monotone non-decreasing
 *  in offset, so the nearest offset per length is never costlier than a
 *  farther one and breakpoints preserve exact parse optimality.
 */

#ifndef MATCHES_H_
#define MATCHES_H_

#include <stdbool.h>

#include "richc/array/u32.h"
#include "richc/bytes.h"

enum {
    min_match = 2,
    max_match = 256,    // a 6502 length byte wraps 256 to 0; 0 is invalid
};

// A token: length == 0 -> uninitialized; length == 1 with offset == 0 ->
// literal (the byte is read from the input at its position); otherwise a
// match of that length at that offset (length 1 matches exist: repeat the
// byte from offset ago)
typedef struct token {
    uint16_t length;
    uint16_t offset;
} token;

static inline bool token_is_literal(token t) { return t.length == 1 && t.offset == 0; }

#define RC_ARRAY_TYPE token
#define RC_ARRAY_NAME token
#include "richc/template/array.h"

// Breakpoints for position i are bp[start[i] .. start[i+1])
typedef struct matches {
    rc_array_u32   head;        // 1 << 16 chain heads
    rc_array_u32   prev;        // per-position chain links
    rc_array_u32   start;       // n+1: breakpoint range per position
    rc_array_token bp;          // breakpoint pool
    rc_array_u32   near1;       // distance to the previous occurrence of
                                // the byte at each position, 0 when none
} matches;

matches scan_matches(rc_view_bytes in, rc_arena *arena);

#endif // MATCHES_H_
