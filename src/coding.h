/*
 *  coding.h - the shared coding composition: offset tables by length
 *  context, plus a length table
 *
 *  Offsets are coded through one of four interval tables conditioned on
 *  the match length (1 / 2 / 3 / over 3): short matches overwhelmingly
 *  use short offsets, so each class gets its own tuned table.  The
 *  length-1 context is deliberately tiny (2-bit flat index, at most 4
 *  buckets - a length-1 match only ever beats a literal at small
 *  offsets); the others use a flat 4-bit index with up to 16 buckets, a
 *  measured optimum.  Codecs without length-1 matches (v2/v3) simply
 *  never use context 0 and serialize from context 1.
 *
 *  Length bucket indices are unary with up to 16 buckets; codecs may
 *  pin the count and serialize their own fixed-count headers (v6/v7).
 */

#ifndef CODING_H_
#define CODING_H_

#include "tables.h"

enum {
    num_contexts    = 4,        // offset contexts: len 1 / 2 / 3 / > 3
    off_index_bits  = 4,        // flat offset bucket index width
    off_max_buckets = 16,       // addressable by the flat offset index
    len_max_buckets = 16,       // unary length index cap
};

// Offset context for a match length
static inline uint32_t off_ctx(uint32_t length)
{
    return (length == 1) ? 0 : (length == 2) ? 1 : (length == 3) ? 2 : 3;
}

// Per-context index geometry
static inline uint32_t off_ctx_index_bits(uint32_t c) { return (c == 0) ? 2 : off_index_bits; }
static inline uint32_t off_ctx_buckets(uint32_t c) { return (c == 0) ? 4 : off_max_buckets; }

typedef struct coding_tables {
    table off[num_contexts];    // indexed by off_ctx
    table len;
} coding_tables;

static inline const table *coding_off_table(const coding_tables *t, uint32_t length)
{
    return &t->off[off_ctx(length)];
}

typedef struct coding_tables_result {
    coding_tables tables;
    bool          ok;
} coding_tables_result;

// Counted-header serialization (5-bit count + width nibbles per table):
// offset contexts first_ctx..3 in order, then the length table.  Codecs
// with format-pinned counts serialize themselves via write_fixed_table.
uint32_t coding_transmit_bits(const coding_tables *t, uint32_t first_ctx);
void write_coding_tables(bitwriter *w, const coding_tables *t, uint32_t first_ctx);
coding_tables_result read_coding_tables(bitreader *r, uint32_t first_ctx,
                                        uint32_t len_minval);

// Seed tables cover the full value ranges so a first parse can reach
// every candidate: geometric offsets and lengths, and a tiny length-1
// context spanning 1..60 (beyond that a length-1 match can never beat a
// 9-bit literal, so the profitable range is fully explorable)
coding_tables seed_coding_tables(uint32_t first_ctx, uint32_t len_minval);

#endif // CODING_H_
