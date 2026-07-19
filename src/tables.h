/*
 *  tables.h - the learned interval table and its machinery
 *
 *  A value v >= minval is coded as a bucket index (flat bits or unary -
 *  i zeros then a 1) followed by width_i raw extra bits, where
 *  bucket i spans [start_i, start_i + 2^width_i) and starts accumulate
 *  from minval.  The widths are chosen per file by a memoized partition
 *  DP over the value histogram - a parametric entropy code with a tiny
 *  header and a trivial decoder.  A table serializes as 4-bit width
 *  nibbles, prefixed by a 5-bit bucket count unless the format pins the
 *  count.  How tables compose into a codec's coding scheme lives with
 *  the shared composition (offset contexts plus lengths) lives in
 *  coding.h.
 */

#ifndef TABLES_H_
#define TABLES_H_

#include <stdbool.h>

#include "richc/array/u32.h"

#include "bitreader.h"
#include "bitwriter.h"

typedef struct rc_arena rc_arena;

enum {
    table_capacity = 32,        // storage bound; also the memo stride
    table_big_cost = 1000000,   // cost of a value no bucket covers
};

typedef struct table {
    uint32_t minval;
    uint32_t num_buckets;
    uint32_t index_bits;    // 0 = unary index, else fixed width
    uint32_t width[table_capacity];
    uint32_t start[table_capacity];
} table;

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
    return (index_bits == 0) ? i + 1 : index_bits;
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

void write_table(bitwriter *w, const table *t);
void write_value(bitwriter *w, const table *t, uint32_t v);

// The reader's fault latch separately covers truncation/malformed gammas
table_result read_table(bitreader *r, uint32_t minval, uint32_t index_bits,
                        uint32_t max_buckets);

// Fixed-count serialization (v6 onwards): a table whose bucket count the
// format pins carries bare width nibbles - no 5-bit count field; unused
// tail buckets pad as width 0.  Reading cannot fail structurally.
void write_fixed_table(bitwriter *w, const table *t, uint32_t count);
table read_fixed_table(bitreader *r, uint32_t minval, uint32_t index_bits,
                       uint32_t count);
value_result read_value(bitreader *r, const table *t);

#endif // TABLES_H_
