/*
 *  tables.h - shared learned interval tables for match offsets and lengths
 *
 *  A value v >= minval is coded as a bucket index (flat bits or Elias
 *  gamma(index+1)) followed by width_i raw extra bits, where bucket i
 *  spans [start_i, start_i + 2^width_i) and starts accumulate from minval.
 *  The widths are chosen per file by a memoized partition DP over the
 *  value histogram - a parametric entropy code with a tiny header and a
 *  trivial decoder.  A table is serialized as a 5-bit bucket count plus a
 *  4-bit width per bucket; only the distribution's shape travels.
 *
 *  Codecs share one composition: three offset tables conditioned on the
 *  match length (len == 2, len == 3, len >= 4 - short matches use short
 *  offsets) plus one length table.  Offset bucket indices are flat 4-bit
 *  (up to 16 buckets - a measured optimum: every match pays the index
 *  width); length bucket indices are Elias gamma (up to 31 buckets - the
 *  skew makes gamma win and it has no structural pin).
 */

#ifndef TABLES_H_
#define TABLES_H_

#include <stdbool.h>

#include "richc/array/u32.h"

#include "bitreader.h"
#include "bitutils.h"
#include "bitwriter.h"

typedef struct rc_arena rc_arena;

enum {
    table_capacity  = 32,       // storage bound; also the memo stride
    table_big_cost  = 1000000,  // cost of a value no bucket covers
    off_index_bits  = 4,        // flat offset bucket index width
    off_max_buckets = 16,       // addressable by the flat offset index
    len_max_buckets = 31,       // gamma index has no structural pin
    num_contexts    = 3,        // offset contexts: len==2, len==3, len>=4
    off1_index_bits  = 2,       // flat index of the tiny length-1 offset table
    off1_max_buckets = 4,
    index_unary      = 16,      // out-of-band index_bits value: unary bucket
                                // index, i zeros then a 1 (i + 1 bits); real
                                // flat widths cap at the 8-bit I/O limit
};

// Short matches overwhelmingly use short offsets, so each length class
// gets its own tuned offset table (exomizer's context split)
static inline uint32_t len_ctx(uint32_t length) { return (length == 2) ? 0 : (length == 3) ? 1 : 2; }

typedef struct table {
    uint32_t minval;
    uint32_t num_buckets;
    uint32_t index_bits;    // 0 = Elias gamma index, index_unary = unary
                            // index, else fixed width
    uint32_t width[table_capacity];
    uint32_t start[table_capacity];
} table;

// The shared composition: offsets by length context, plus lengths
typedef struct coding_tables {
    table off[num_contexts];
    table len;
} coding_tables;

typedef struct table_result {
    table table;
    bool  ok;               // false on a malformed bucket count
} table_result;

typedef struct value_result {
    uint32_t value;
    bool     ok;            // false on an out-of-range bucket index
} value_result;

// Cost in bits of the bucket index i under an index_bits mode
static inline uint32_t index_cost(uint32_t index_bits, uint32_t i)
{
    return (index_bits == 0)           ? gamma_bits(i + 1)
         : (index_bits == index_unary) ? i + 1
         : index_bits;
}

void table_build_starts(table *t);

// Bucket containing v, or RC_INDEX_NONE when no bucket covers it
uint32_t table_index(const table *t, uint32_t v);

// Cost in bits of coding v; table_big_cost when uncovered.  Coverage is a
// contiguous range from minval, so once a value is uncovered every larger
// value is too - callers exploit that to break out of candidate loops.
uint32_t table_cost(const table *t, uint32_t v);

// Optimal widths for the histogram hist over [minval, maxval]; hist[v]
// counts occurrences of value v
table optimize_table(rc_view_u32 hist, uint32_t minval, uint32_t maxval,
                     uint32_t index_bits, uint32_t max_buckets, rc_arena scratch);

static inline uint32_t table_transmit_bits(const table *t) { return 5 + 4 * t->num_buckets; }

// Serialized size in bits of a full composition's headers
uint32_t coding_transmit_bits(const coding_tables *t);

void write_table(bitwriter *w, const table *t);
void write_value(bitwriter *w, const table *t, uint32_t v);
void write_coding_tables(bitwriter *w, const coding_tables *t);

// The reader's fault latch separately covers truncation/malformed gammas
table_result read_table(bitreader *r, uint32_t minval, uint32_t index_bits,
                        uint32_t max_buckets);
value_result read_value(bitreader *r, const table *t);

typedef struct coding_tables_result {
    coding_tables tables;
    bool          ok;
} coding_tables_result;

coding_tables_result read_coding_tables(bitreader *r);

// Seed tables cover the full value ranges so a first parse can reach
// every candidate; deliberately gamma-like, not tuned
coding_tables seed_coding_tables(void);

// The length-1 composition (v4 onwards): the length table's minimum value
// drops to 1, and a deliberately tiny fourth offset table serves length-1
// matches - they only ever beat a 9-bit literal at small offsets, and the
// table learns exactly that range.  Serialized off1 first, then the three
// offset contexts, then lengths.
typedef struct len1_tables {
    table off1;
    table off[num_contexts];
    table len;
} len1_tables;

// Offset table for a match length, including the length-1 context
static inline const table *len1_off_table(const len1_tables *t, uint32_t length)
{
    return (length == 1) ? &t->off1 : &t->off[len_ctx(length)];
}

uint32_t len1_transmit_bits(const len1_tables *t);

void write_len1_tables(bitwriter *w, const len1_tables *t);

typedef struct len1_tables_result {
    len1_tables tables;
    bool        ok;
} len1_tables_result;

len1_tables_result read_len1_tables(bitreader *r);

// off1's seed spans 1..60: beyond that a length-1 match can never beat a
// 9-bit literal, so the profitable range is fully explorable
len1_tables seed_len1_tables(void);

#endif // TABLES_H_
