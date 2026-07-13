/*
 *  v2.c - the v2 codec: learned bucket tables, single offset context
 */

#include "v2.h"

#include "richc/arena.h"
#include "richc/array/u64.h"
#include "richc/macros.h"

#include "bitreader.h"
#include "bitutils.h"
#include "bitwriter.h"
#include "matches.h"

enum {
    v2_table_capacity  = 32,    // storage bound; also the memo stride
    v2_off_max_buckets = 16,    // addressable by the flat offset index
    v2_len_max_buckets = 31,    // gamma index has no structural pin
    v2_iterations      = 8,     // parse <-> table refinement rounds
    v2_off_index_bits  = 4,     // flat offset bucket index width
    v2_big_cost        = 1000000,
};

// ---- learned interval tables ----
//
// A value v >= minval is coded as a bucket index (flat bits or Elias
// gamma(index+1)) followed by width_i raw extra bits, where bucket i
// spans [start_i, start_i + 2^width_i) and starts accumulate from minval.
// The widths are chosen per file by a memoized partition DP over the
// value histogram - a parametric entropy code with a tiny header and a
// trivial decoder.

typedef struct v2_table {
    uint32_t minval;
    uint32_t num_buckets;
    uint32_t index_bits;    // 0 = Elias gamma index, else fixed width
    uint32_t width[v2_table_capacity];
    uint32_t start[v2_table_capacity];
} v2_table;

typedef struct v2_tables {
    v2_table off;
    v2_table len;
} v2_tables;

static void table_build_starts(v2_table *t)
{
    RC_ASSERT(t->num_buckets <= v2_table_capacity);
    uint32_t s = t->minval;
    for (uint32_t i = 0; i < t->num_buckets; i++) {
        t->start[i] = s;
        s += 1u << t->width[i];
    }
}

// Bucket containing v, or RC_INDEX_NONE when no bucket covers it
static uint32_t table_index(const v2_table *t, uint32_t v)
{
    for (uint32_t i = 0; i < t->num_buckets; i++) {
        if (v >= t->start[i] && v < t->start[i] + (1u << t->width[i])) {
            return i;
        }
    }
    return RC_INDEX_NONE;
}

// Cost in bits of coding v; v2_big_cost when uncovered.  Coverage is a
// contiguous range from minval, so once a value is uncovered every larger
// value is too - callers exploit that to break out of candidate loops.
static uint32_t table_cost(const v2_table *t, uint32_t v)
{
    uint32_t i = table_index(t, v);
    if (i == RC_INDEX_NONE) {
        return v2_big_cost;
    }
    uint32_t prefix = t->index_bits ? t->index_bits : gamma_bits(i + 1);
    return prefix + t->width[i];
}

// ---- table optimizer ----
//
// stats[v] holds the suffix count of histogram entries >= v, so a bucket
// [start, end) covers stats[start] - stats[end] values.  For each
// (start, depth) try every width and keep the one minimizing this
// bucket's bits plus the best partition of the rest; memoized on
// (start - minval) * maxdepth + depth.  The prefix cost per bucket is the
// index code: flat index_bits, or gamma(depth + 1).

static uint64_t opt_rec(rc_view_u32 stats, uint32_t minval, uint32_t maxval,
                        uint32_t start, uint32_t depth, uint32_t index_bits,
                        uint32_t max_buckets,
                        rc_span_u32 memo_width, rc_span_u64 memo_cost,
                        rc_span_u32 memo_seen)
{
    uint32_t key = (start - minval) * v2_table_capacity + depth;
    if (rc_span_u32_get(memo_seen, key)) {
        return rc_span_u64_get(memo_cost, key);
    }

    uint64_t prefix = index_bits ? index_bits : gamma_bits(depth + 1);
    uint64_t best = UINT64_MAX;
    uint32_t best_width = 0;
    for (uint32_t w = 0; w <= 15; w++) {
        uint64_t end = (uint64_t)start + (1ull << w);
        uint32_t end_clamped = (end > maxval + 1u) ? (maxval + 1) : (uint32_t)end;
        uint64_t here = (uint64_t)(rc_view_u32_get(stats, start) - rc_view_u32_get(stats, end_clamped))
                      * (prefix + w);
        uint64_t total;
        if (end_clamped <= maxval && rc_view_u32_get(stats, end_clamped) > 0) {
            // More values beyond this bucket: recurse unless out of depth.
            if (depth + 1 >= max_buckets) {
                if (end > maxval + 1u) {
                    break;
                }
                continue;
            }
            uint64_t rest = opt_rec(stats, minval, maxval, end_clamped, depth + 1,
                                    index_bits, max_buckets,
                                    memo_width, memo_cost, memo_seen);
            if (rest == UINT64_MAX) {
                if (end > maxval + 1u) {
                    break;
                }
                continue;
            }
            total = here + rest;
        }
        else {
            total = here;
        }
        if (total < best) {
            best = total;
            best_width = w;
        }
        if (end > maxval + 1u) {
            break;
        }
    }
    rc_span_u32_set(memo_width, key, best_width);
    rc_span_u64_set(memo_cost, key, best);
    rc_span_u32_set(memo_seen, key, 1);
    return best;
}

// Optimal widths for the histogram hist over [minval, maxval]; hist[v]
// counts occurrences of value v
static v2_table optimize_table(rc_view_u32 hist, uint32_t minval, uint32_t maxval,
                               uint32_t index_bits, uint32_t max_buckets,
                               rc_arena scratch)
{
    RC_ASSERT(max_buckets <= v2_table_capacity);
    v2_table t = {
        .minval = minval,
        .num_buckets = 0,
        .index_bits = index_bits,
    };
    if (maxval < minval) {
        return t;
    }
    RC_ASSERT(maxval < hist.num);

    // Suffix counts, so bucket coverage is a subtraction.
    rc_array_u32 stats = {0};
    rc_array_u32_resize(&stats, maxval + 2, &scratch);
    rc_array_u32_set(&stats, maxval + 1, 0);
    uint32_t acc = 0;
    for (uint32_t v = maxval; ; v--) {
        acc += rc_view_u32_get(hist, v);
        rc_array_u32_set(&stats, v, acc);
        if (v == minval) {
            break;
        }
    }
    if (acc == 0) {
        return t;
    }

    uint32_t n = maxval + 2 - minval;
    rc_array_u32 memo_width = {0};
    rc_array_u32_resize(&memo_width, n * v2_table_capacity, &scratch);
    rc_array_u64 memo_cost = {0};
    rc_array_u64_resize(&memo_cost, n * v2_table_capacity, &scratch);
    rc_array_u32 memo_seen = {0};
    rc_array_u32_resize(&memo_seen, n * v2_table_capacity, &scratch);
    for (uint32_t i = 0; i < n * v2_table_capacity; i++) {
        rc_array_u32_set(&memo_seen, i, 0);
    }

    opt_rec(stats.view, minval, maxval, minval, 0, index_bits, max_buckets,
            memo_width.span, memo_cost.span, memo_seen.span);

    // Walk the memoized decisions to extract the winning partition.
    uint32_t start = minval;
    uint32_t depth = 0;
    while (t.num_buckets < max_buckets) {
        uint32_t key = (start - minval) * v2_table_capacity + depth;
        uint32_t w = rc_array_u32_get(&memo_width, key);
        t.width[t.num_buckets++] = w;
        uint64_t end = (uint64_t)start + (1ull << w);
        if (end > maxval || rc_array_u32_get(&stats, (uint32_t)end) == 0) {
            break;
        }
        start = (uint32_t)end;
        depth++;
    }
    table_build_starts(&t);
    return t;
}

// ---- table and value serialization ----

static uint32_t table_transmit_bits(const v2_table *t)
{
    return 5 + 4 * t->num_buckets;
}

static void write_table(bitwriter *w, const v2_table *t)
{
    RC_ASSERT(t->num_buckets <= v2_table_capacity);
    bitwriter_bits(w, t->num_buckets, 5);
    for (uint32_t i = 0; i < t->num_buckets; i++) {
        bitwriter_bits(w, t->width[i], 4);
    }
}

// Wide extra fields travel as two <= 8-bit pieces (the bit I/O cap); the
// 6502 shifts them into a 16-bit value either way.
static void write_extra(bitwriter *w, uint32_t extra, uint32_t width)
{
    if (width > 8) {
        bitwriter_bits(w, extra >> 8, width - 8);
        bitwriter_bits(w, extra & 0xFF, 8);
    }
    else {
        bitwriter_bits(w, extra, width);
    }
}

static uint32_t read_extra(bitreader *r, uint32_t width)
{
    if (width > 8) {
        uint32_t hi = bitreader_bits(r, width - 8);
        return (hi << 8) | bitreader_bits(r, 8);
    }
    return bitreader_bits(r, width);
}

static void write_value(bitwriter *w, const v2_table *t, uint32_t v)
{
    uint32_t i = table_index(t, v);
    RC_ASSERT(i != RC_INDEX_NONE);
    if (t->index_bits) {
        bitwriter_bits(w, i, t->index_bits);
    }
    else {
        bitwriter_gamma(w, i + 1);
    }
    write_extra(w, v - t->start[i], t->width[i]);
}

// ---- optimal forward parse ----
//
// v0's exact forward DP: flag-per-token keeps token costs independent of
// position, so cost[i] is a single number and every relaxation is final.
// The parse is exact over the cached nearest-offset-per-length match
// candidates; a learned table can in principle be non-monotone in offset
// (a farther bucket with a smaller width), where a farther offset would
// be cheaper than the nearest - rare, and accepted, as exomizer does.

static uint32_t dp_pass(rc_view_bytes in, const matches *m, const v2_tables *tables,
                        rc_span_token arrival, rc_arena scratch)
{
    uint32_t n = in.num;
    rc_array_u32 cost = {0};
    rc_array_u32_resize(&cost, n + 1, &scratch);
    rc_array_u32_set(&cost, 0, 0);
    for (uint32_t j = 1; j <= n; j++) {
        rc_array_u32_set(&cost, j, UINT32_MAX);
    }

    for (uint32_t i = 0; i < n; i++) {
        uint32_t here = rc_array_u32_get(&cost, i);

        // Literal edge: flag bit + aligned byte.
        uint32_t lit = here + 1 + 8;
        if (lit < rc_array_u32_get(&cost, i + 1)) {
            rc_array_u32_set(&cost, i + 1, lit);
            if (arrival.num) {
                rc_span_token_set(arrival, i + 1, (token) {
                    .length = 1,
                    .literal = rc_view_bytes_get(in, i),
                });
            }
        }

        // Match edges: every truncation of every breakpoint the tables can
        // encode.  Coverage is contiguous from the minimum, so the first
        // uncovered offset ends the (offset-ascending) breakpoint list,
        // and the first uncovered length ends the truncation loop.
        uint32_t lo = min_match - 1;
        uint32_t end = rc_array_u32_get(&m->start, i + 1);
        for (uint32_t k = rc_array_u32_get(&m->start, i); k < end; k++) {
            token t = rc_array_token_get(&m->bp, k);
            uint32_t off_cost = table_cost(&tables->off, t.offset);
            if (off_cost >= v2_big_cost) {
                break;
            }
            uint32_t base = here + 1 + off_cost;
            for (uint32_t len = lo + 1; len <= t.length; len++) {
                uint32_t len_cost = table_cost(&tables->len, len);
                if (len_cost >= v2_big_cost) {
                    break;
                }
                uint32_t c = base + len_cost;
                if (c < rc_array_u32_get(&cost, i + len)) {
                    rc_array_u32_set(&cost, i + len, c);
                    if (arrival.num) {
                        rc_span_token_set(arrival, i + len, (token) {
                            .length = (uint16_t)len,
                            .offset = t.offset,
                        });
                    }
                }
            }
            lo = t.length;
        }
    }
    return rc_array_u32_get(&cost, n);
}

// Exact stream bits of a token sequence under the given tables (flag bits
// + literals + matches; excludes the table headers)
static uint32_t data_bits(rc_view_token tokens, const v2_tables *tables)
{
    uint32_t bits = 0;
    for (uint32_t k = 0; k < tokens.num; k++) {
        token t = rc_view_token_get(tokens, k);
        if (t.length == 1) {
            bits += 1 + 8;
        }
        else {
            bits += 1 + table_cost(&tables->off, t.offset)
                  + table_cost(&tables->len, t.length);
        }
    }
    return bits;
}

// ---- encoding ----

static rc_array_bytes encode(rc_view_bytes in, rc_view_token tokens,
                             const v2_tables *tables, rc_arena *arena)
{
    rc_array_bytes out = {0};
    rc_array_bytes_reserve(&out, 64, arena);

    // Raw two-byte length header, then everything else through the writer.
    rc_array_bytes_push(&out, (uint8_t)(in.num), arena);
    rc_array_bytes_push(&out, (uint8_t)(in.num >> 8), arena);

    bitwriter w = bitwriter_make(&out, arena);
    write_table(&w, &tables->off);
    write_table(&w, &tables->len);

    uint32_t total = 0;
    for (uint32_t k = 0; k < tokens.num; k++) {
        token t = rc_view_token_get(tokens, k);
        if (t.length == 1) {
            bitwriter_bits(&w, 0, 1);
            bitwriter_byte(&w, t.literal);
        }
        else {
            // Match: offset then length, each through its table.
            bitwriter_bits(&w, 1, 1);
            write_value(&w, &tables->off, t.offset);
            write_value(&w, &tables->len, t.length);
        }
        total += t.length;
    }
    RC_ASSERT(total == in.num);
    return out;
}

// Seed tables cover the full value ranges so the first parse can reach
// every candidate; they are deliberately gamma-like, not tuned.
static v2_tables seed_tables(void)
{
    v2_tables t = {
        .off = {
            .minval = 1,
            .num_buckets = 16,
            .index_bits = v2_off_index_bits,
        },
        .len = {
            .minval = 2,
            .num_buckets = 9,
            .index_bits = 0,
        },
    };
    for (uint32_t i = 0; i < t.off.num_buckets; i++) {
        t.off.width[i] = i;
    }
    for (uint32_t i = 0; i < t.len.num_buckets; i++) {
        t.len.width[i] = i;
    }
    table_build_starts(&t.off);
    table_build_starts(&t.len);
    return t;
}

v2_compress_result v2_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    RC_ASSERT(arena);
    RC_ASSERT(in.num <= v2_max_uncompressed);
    RC_ASSERT(arena->base != scratch.base);

    uint32_t n = in.num;

    // Find and cache all match candidates once.
    matches m = scan_matches(in, &scratch);

    // The best tables depend on the parse and the best parse depends on
    // the tables: iterate to a fixpoint, keeping the best true size seen
    // (table headers included - a fancier table must pay for itself).
    v2_tables tables = seed_tables();
    v2_tables best_tables = tables;
    rc_array_token best_tokens = {0};
    uint32_t best_bits = UINT32_MAX;

    rc_array_token arrival = {0};
    rc_array_token_resize(&arrival, n + 1, &scratch);

    for (uint32_t iter = 0; iter < v2_iterations; iter++) {
        // Exact parse under the current tables.
        dp_pass(in, &m, &tables, arrival.span, scratch);
        rc_array_token tokens = {0};
        rc_array_token_reserve(&tokens, n ? n : 1, &scratch);
        uint32_t j = n;
        while (j > 0) {
            token t = rc_array_token_get(&arrival, j);
            rc_array_token_push(&tokens, t, &scratch);
            j -= t.length;
        }
        rc_span_token_reverse(tokens.span);

        // Histogram the values this parse actually used...
        rc_array_u32 len_hist = {0};
        rc_array_u32_resize(&len_hist, max_match + 1, &scratch);
        for (uint32_t v = 0; v <= max_match; v++) {
            rc_array_u32_set(&len_hist, v, 0);
        }
        rc_array_u32 off_hist = {0};
        rc_array_u32_resize(&off_hist, n + 1, &scratch);
        for (uint32_t v = 0; v <= n; v++) {
            rc_array_u32_set(&off_hist, v, 0);
        }
        uint32_t max_len = 0;
        uint32_t max_off = 0;
        for (uint32_t k = 0; k < tokens.num; k++) {
            token t = rc_view_token_get(tokens.view, k);
            if (t.length == 1) {
                continue;
            }
            rc_array_u32_set(&len_hist, t.length, rc_array_u32_get(&len_hist, t.length) + 1);
            rc_array_u32_set(&off_hist, t.offset, rc_array_u32_get(&off_hist, t.offset) + 1);
            if (t.length > max_len) {
                max_len = t.length;
            }
            if (t.offset > max_off) {
                max_off = t.offset;
            }
        }

        // ...and rebuild optimal tables for exactly that usage.
        v2_tables next = {
            .off = optimize_table(off_hist.view, 1, max_off, v2_off_index_bits,
                                  v2_off_max_buckets, scratch),
            .len = optimize_table(len_hist.view, 2, max_len, 0,
                                  v2_len_max_buckets, scratch),
        };

        uint32_t bits = table_transmit_bits(&next.off) + table_transmit_bits(&next.len)
                      + data_bits(tokens.view, &next);
        if (bits < best_bits) {
            best_bits = bits;
            best_tokens = tokens;
            best_tables = next;
        }
        tables = next;
    }

    return (v2_compress_result) {
        .data = encode(in, best_tokens.view, &best_tables, arena),
        .num_tokens = best_tokens.num,
    };
}

// ---- decoding ----

static v2_decompress_result fail(void) { return (v2_decompress_result) {.data = {0}, .ok = false}; }

typedef struct table_result {
    v2_table table;
    bool     ok;            // false on a malformed bucket count
} table_result;

static table_result read_table(bitreader *r, uint32_t minval, uint32_t index_bits,
                               uint32_t max_buckets)
{
    table_result res = {
        .table = {
            .minval = minval,
            .index_bits = index_bits,
        },
        .ok = true,
    };
    uint32_t count = bitreader_bits(r, 5);
    if (count > max_buckets) {
        res.ok = false;
        return res;
    }
    res.table.num_buckets = count;
    for (uint32_t i = 0; i < count; i++) {
        res.table.width[i] = bitreader_bits(r, 4);
    }
    table_build_starts(&res.table);
    return res;
}

typedef struct value_result {
    uint32_t value;
    bool     ok;            // false on an out-of-range bucket index
} value_result;

// The reader's fault latch separately covers truncation/malformed gammas
static value_result read_value(bitreader *r, const v2_table *t)
{
    uint32_t i;
    if (t->index_bits) {
        i = bitreader_bits(r, t->index_bits);
    }
    else {
        i = bitreader_gamma(r) - 1;
    }
    if (i >= t->num_buckets) {
        return (value_result) {.value = 0, .ok = false};
    }
    return (value_result) {
        .value = t->start[i] + read_extra(r, t->width[i]),
        .ok = true,
    };
}

v2_decompress_result v2_decompress(rc_view_bytes comp, rc_arena *arena)
{
    RC_ASSERT(arena);

    // Length header first: it bounds everything that follows.
    if (comp.num < 2) {
        return fail();
    }
    uint32_t len = (uint32_t)rc_view_bytes_get(comp, 0)
                 | ((uint32_t)rc_view_bytes_get(comp, 1) << 8);

    bitreader r = bitreader_make(rc_view_bytes_get_tail(comp, 2));
    table_result off = read_table(&r, 1, v2_off_index_bits, v2_off_max_buckets);
    table_result len_table = read_table(&r, 2, 0, v2_len_max_buckets);
    if (!off.ok || !len_table.ok || r.fault != bit_fault_ok) {
        return fail();
    }
    v2_tables tables = {
        .off = off.table,
        .len = len_table.table,
    };

    rc_array_bytes out = {0};
    rc_array_bytes_reserve(&out, len ? len : 1, arena);
    while (out.num < len) {
        if (bitreader_bits(&r, 1) == 0) {
            // Literal: one aligned byte.  Check the fault before trusting
            // the value; past-the-end reads yield zeros.
            uint8_t v = bitreader_byte(&r);
            if (r.fault != bit_fault_ok) {
                return fail();
            }
            rc_array_bytes_push(&out, v, arena);
        }
        else {
            // Match: offset then length through the tables, then validate
            // both against what has actually been produced so far - the
            // stream is untrusted.
            value_result offset = read_value(&r, &tables.off);
            value_result length = read_value(&r, &tables.len);
            if (!offset.ok || !length.ok || r.fault != bit_fault_ok) {
                return fail();
            }
            if (offset.value > out.num || out.num + length.value > len) {
                return fail();
            }
            // Forward byte-by-byte copy handles overlap (offset < length).
            uint32_t src = out.num - offset.value;
            for (uint32_t k = 0; k < length.value; k++) {
                rc_array_bytes_push(&out, rc_array_bytes_get(&out, src + k), arena);
            }
        }
    }
    return (v2_decompress_result) {.data = out, .ok = true};
}

// ------------------------------------------------------------------
//  tests
// ------------------------------------------------------------------

#ifdef ENABLE_TESTS

#include <string.h>

#include "richc/test.h"

#include "v0.h"

RC_TEST_GROUP_DATA(v2) {
    rc_arena a;
    rc_arena scratch;
};

RC_TEST_GROUP_INIT(v2, fix)
{
    fix->a = rc_arena_make(1u << 30);
    fix->scratch = rc_arena_make(1u << 30);
}

RC_TEST_GROUP_DEINIT(v2, fix)
{
    rc_arena_deinit(&fix->scratch);
    rc_arena_deinit(&fix->a);
}

// ---- table machinery ----

RC_TEST(v2, table_starts_index_cost)
{
    v2_table t = {
        .minval = 1,
        .num_buckets = 4,
        .index_bits = 4,
        .width = {0, 1, 2, 3},
    };
    table_build_starts(&t);
    RC_CHECK(t.start[0], ==, 1u);
    RC_CHECK(t.start[1], ==, 2u);
    RC_CHECK(t.start[2], ==, 4u);
    RC_CHECK(t.start[3], ==, 8u);

    RC_CHECK(table_index(&t, 1), ==, 0u);
    RC_CHECK(table_index(&t, 3), ==, 1u);
    RC_CHECK(table_index(&t, 7), ==, 2u);
    RC_CHECK(table_index(&t, 15), ==, 3u);
    RC_CHECK(table_index(&t, 16), ==, RC_INDEX_NONE);
    RC_CHECK(table_index(&t, 0), ==, RC_INDEX_NONE);

    RC_CHECK(table_cost(&t, 1), ==, 4u);            // flat 4 + width 0
    RC_CHECK(table_cost(&t, 3), ==, 5u);            // flat 4 + width 1
    RC_CHECK(table_cost(&t, 16), ==, (uint32_t)v2_big_cost);

    // Gamma-indexed variant of the same shape.
    t.index_bits = 0;
    RC_CHECK(table_cost(&t, 1), ==, 1u);            // gamma(1) + 0
    RC_CHECK(table_cost(&t, 3), ==, 4u);            // gamma(2)=3 + 1
    RC_CHECK(table_cost(&t, 9), ==, 8u);            // gamma(4)=5 + 3
}

RC_TEST_STEP(v2, table_serialize_roundtrip, fix)
{
    v2_table t = {
        .minval = 2,
        .num_buckets = 5,
        .index_bits = 0,
        .width = {0, 2, 4, 3, 7},
    };
    table_build_starts(&t);

    rc_array_bytes out = {0};
    bitwriter w = bitwriter_make(&out, &fix->a);
    write_table(&w, &t);

    bitreader r = bitreader_make(out.view);
    table_result back = read_table(&r, 2, 0, v2_len_max_buckets);
    RC_CHECK_TRUE(back.ok);
    RC_CHECK(r.fault, ==, bit_fault_ok);
    RC_CHECK(back.table.num_buckets, ==, t.num_buckets);
    for (uint32_t i = 0; i < t.num_buckets; i++) {
        RC_CHECK(back.table.width[i], ==, t.width[i]);
        RC_CHECK(back.table.start[i], ==, t.start[i]);
    }
}

RC_TEST_STEP(v2, table_rejects_oversized_count, fix)
{
    for (uint32_t count = v2_off_max_buckets + 1; count <= 31; count++) {
        rc_array_bytes out = {0};
        uint32_t mark = fix->a.top;
        bitwriter w = bitwriter_make(&out, &fix->a);
        bitwriter_bits(&w, count, 5);
        bitwriter_bits(&w, 0, 8);
        bitwriter_bits(&w, 0, 8);

        bitreader r = bitreader_make(out.view);
        RC_CHECK_FALSE(read_table(&r, 1, 4, v2_off_max_buckets).ok);
        rc_arena_free_to(&fix->a, mark);
    }
}

RC_TEST_STEP(v2, value_roundtrip_wide_extras, fix)
{
    // A late bucket with width 15 exercises the split extra-field path.
    v2_table t = {
        .minval = 1,
        .num_buckets = 3,
        .index_bits = 4,
        .width = {2, 9, 15},
    };
    table_build_starts(&t);

    rc_array_bytes out = {0};
    bitwriter w = bitwriter_make(&out, &fix->a);
    uint32_t vals[] = {1, 4, 5, 300, 516, 517, 10000, 33284};
    for (uint32_t k = 0; k < sizeof vals / sizeof vals[0]; k++) {
        write_value(&w, &t, vals[k]);
    }

    bitreader r = bitreader_make(out.view);
    for (uint32_t k = 0; k < sizeof vals / sizeof vals[0]; k++) {
        value_result v = read_value(&r, &t);
        RC_CHECK_TRUE(v.ok);
        RC_CHECK(v.value, ==, vals[k]);
    }
    RC_CHECK(r.fault, ==, bit_fault_ok);
}

// ---- optimizer vs brute force ----

// Exhaustive minimal data bits over all partitions (small widths only),
// mirroring the DP's objective and termination rule.
static uint64_t brute_partition(rc_view_u32 stats, uint32_t maxval,
                                uint32_t start, uint32_t depth, uint32_t index_bits)
{
    if (start > maxval || rc_view_u32_get(stats, start) == 0) {
        return 0;
    }
    if (depth >= 16) {
        return UINT64_MAX;
    }
    uint64_t best = UINT64_MAX;
    for (uint32_t w = 0; w <= 6; w++) {
        uint64_t end = (uint64_t)start + (1ull << w);
        uint32_t end_clamped = (end > maxval + 1u) ? (maxval + 1) : (uint32_t)end;
        uint64_t prefix = index_bits ? index_bits : gamma_bits(depth + 1);
        uint64_t here = (uint64_t)(rc_view_u32_get(stats, start) - rc_view_u32_get(stats, end_clamped))
                      * (prefix + w);
        uint64_t rest = brute_partition(stats, maxval, end_clamped, depth + 1, index_bits);
        if (rest != UINT64_MAX && here + rest < best) {
            best = here + rest;
        }
        if (end > maxval) {
            break;
        }
    }
    return best;
}

static uint64_t table_data_bits(rc_view_u32 hist, uint32_t minval, uint32_t maxval,
                                const v2_table *t)
{
    uint64_t bits = 0;
    for (uint32_t v = minval; v <= maxval; v++) {
        uint32_t count = rc_view_u32_get(hist, v);
        if (count > 0) {
            uint32_t c = table_cost(t, v);
            RC_CHECK(c, <, (uint32_t)v2_big_cost);
            bits += (uint64_t)count * c;
        }
    }
    return bits;
}

static void check_optimizer(rc_view_u32 hist, uint32_t minval, uint32_t maxval,
                            uint32_t index_bits, rc_arena *a)
{
    rc_array_u32 stats = {0};
    rc_array_u32_resize(&stats, maxval + 2, a);
    rc_array_u32_set(&stats, maxval + 1, 0);
    uint32_t acc = 0;
    for (uint32_t v = maxval; ; v--) {
        acc += rc_view_u32_get(hist, v);
        rc_array_u32_set(&stats, v, acc);
        if (v == minval) {
            break;
        }
    }
    for (uint32_t v = 0; v < minval; v++) {
        rc_array_u32_set(&stats, v, acc);
    }

    v2_table t = optimize_table(hist, minval, maxval, index_bits, 16, *a);
    if (acc == 0) {
        RC_CHECK(t.num_buckets, ==, 0u);
        return;
    }
    uint64_t got = table_data_bits(hist, minval, maxval, &t);
    uint64_t want = brute_partition(stats.view, maxval, minval, 0, index_bits);
    RC_CHECK(got, ==, want);
}

RC_TEST_STEP(v2, optimizer_matches_brute_force, fix)
{
    enum { N = 16 };
    uint32_t hist[N];
    rc_view_u32 view = (rc_view_u32) RC_VIEW(hist);

    for (uint32_t index_bits = 0; index_bits <= 4; index_bits += 4) {
        // Empty histogram
        for (uint32_t v = 0; v < N; v++) { hist[v] = 0; }
        check_optimizer(view, 1, 14, index_bits, &fix->a);

        // Spike at minval, spike at maxval
        hist[1] = 100;
        check_optimizer(view, 1, 14, index_bits, &fix->a);
        for (uint32_t v = 0; v < N; v++) { hist[v] = 0; }
        hist[14] = 7;
        check_optimizer(view, 1, 14, index_bits, &fix->a);

        // Uniform and geometric
        for (uint32_t v = 1; v <= 14; v++) { hist[v] = 1; }
        check_optimizer(view, 1, 14, index_bits, &fix->a);
        for (uint32_t v = 1; v <= 14; v++) { hist[v] = 1u << (14 - v); }
        check_optimizer(view, 1, 14, index_bits, &fix->a);

        // Seeded random histograms
        uint32_t seed = 0x12345678;
        for (uint32_t trial = 0; trial < 100; trial++) {
            for (uint32_t v = 1; v <= 14; v++) {
                seed ^= seed << 13;
                seed ^= seed >> 17;
                seed ^= seed << 5;
                hist[v] = seed % 20;
            }
            check_optimizer(view, 1, 14, index_bits, &fix->a);
        }
    }
}

// ---- roundtrips ----

static uint32_t roundtrip(rc_view_bytes in, struct rc_test_group_data_v2 *fix)
{
    v2_compress_result c = v2_compress(in, &fix->a, fix->scratch);
    RC_CHECK(c.data.num, >=, 2u);
    RC_CHECK(rc_array_bytes_get(&c.data, 0), ==, (uint8_t)in.num);
    RC_CHECK(rc_array_bytes_get(&c.data, 1), ==, (uint8_t)(in.num >> 8));

    v2_decompress_result d = v2_decompress(c.data.view, &fix->a);
    RC_CHECK_TRUE(d.ok);
    RC_CHECK(d.data.num, ==, in.num);
    if (in.num > 0) {
        RC_CHECK_TRUE(memcmp(d.data.data, in.data, in.num) == 0);
    }
    return c.data.num;
}

RC_TEST_STEP(v2, roundtrip_edges, fix)
{
    roundtrip((rc_view_bytes) {0}, fix);

    uint8_t one[1] = {0x42};
    roundtrip((rc_view_bytes) RC_VIEW(one), fix);

    uint8_t two[2] = {7, 7};
    roundtrip((rc_view_bytes) RC_VIEW(two), fix);

    static uint8_t same[4096];
    memset(same, 0xEE, sizeof same);
    roundtrip((rc_view_bytes) {.data = same, .num = 256}, fix);
    roundtrip((rc_view_bytes) RC_VIEW(same), fix);

    static uint8_t alt[512];
    for (uint32_t i = 0; i < sizeof alt; i++) {
        alt[i] = (i & 1) ? 0xAA : 0x55;
    }
    roundtrip((rc_view_bytes) RC_VIEW(alt), fix);
}

RC_TEST_STEP(v2, roundtrip_random_incompressible, fix)
{
    static uint8_t buf[4096];
    uint32_t seed = 0xDECAFBAD;
    for (uint32_t i = 0; i < sizeof buf; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        buf[i] = (uint8_t)(seed >> 24);
    }
    roundtrip((rc_view_bytes) RC_VIEW(buf), fix);
}

RC_TEST_STEP(v2, roundtrip_text_beats_v0, fix)
{
    static uint8_t buf[8192];
    const char *phrase = "all work and no play makes jack a dull boy. ";
    uint32_t plen = (uint32_t)strlen(phrase);
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)phrase[i % plen];
    }
    rc_view_bytes in = (rc_view_bytes) RC_VIEW(buf);
    uint32_t packed = roundtrip(in, fix);
    RC_CHECK(packed, <, (uint32_t)(sizeof buf) / 10);

    // Learned tables must not lose to fixed gamma + B codes here.
    v0_compress_result v0 = v0_compress(in, &fix->a, fix->scratch);
    RC_CHECK(packed, <=, v0.data.num);
}

RC_TEST_STEP(v2, roundtrip_far_offsets, fix)
{
    static uint8_t buf[40000];
    uint32_t seed = 0xACE1;
    for (uint32_t i = 0; i < sizeof buf; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        buf[i] = (uint8_t)(seed & 0x3F);
    }
    for (uint32_t block = 1; block < 8; block++) {
        uint32_t dst = block * 5000;
        uint32_t src = dst - (block * 617 + 300);
        memcpy(&buf[dst], &buf[src], 200);
    }
    roundtrip((rc_view_bytes) RC_VIEW(buf), fix);
}

RC_TEST_STEP(v2, roundtrip_max_size, fix)
{
    static uint8_t buf[v2_max_uncompressed];
    uint32_t seed = 0x5EED;
    for (uint32_t i = 0; i < sizeof buf; i++) {
        if ((i >> 10) & 1) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            buf[i] = (uint8_t)(seed >> 16);
        }
        else {
            buf[i] = (uint8_t)(i >> 6);
        }
    }
    roundtrip((rc_view_bytes) RC_VIEW(buf), fix);
}

// ---- parse exactness for fixed tables ----

static uint32_t brute_suffix_bits(rc_view_bytes in, const matches *m,
                                  const v2_tables *tables, uint32_t pos)
{
    if (pos == in.num) {
        return 0;
    }
    uint32_t best = 9 + brute_suffix_bits(in, m, tables, pos + 1);
    uint32_t lo = min_match - 1;
    uint32_t end = rc_array_u32_get(&m->start, pos + 1);
    for (uint32_t k = rc_array_u32_get(&m->start, pos); k < end; k++) {
        token t = rc_array_token_get(&m->bp, k);
        uint32_t off_cost = table_cost(&tables->off, t.offset);
        if (off_cost >= v2_big_cost) {
            break;
        }
        for (uint32_t len = lo + 1; len <= t.length; len++) {
            uint32_t len_cost = table_cost(&tables->len, len);
            if (len_cost >= v2_big_cost) {
                break;
            }
            uint32_t c = 1 + off_cost + len_cost
                       + brute_suffix_bits(in, m, tables, pos + len);
            if (c < best) {
                best = c;
            }
        }
        lo = t.length;
    }
    return best;
}

RC_TEST_STEP(v2, parse_is_exact_for_fixed_tables, fix)
{
    // The table fixpoint is a heuristic, but the parse under any FIXED
    // tables must be exactly optimal over the breakpoint candidates.
    v2_tables tables = seed_tables();
    uint32_t seed = 0xFEED;
    for (uint32_t trial = 0; trial < 60; trial++) {
        uint8_t data[12];
        uint32_t n = 4 + trial % 9;
        for (uint32_t i = 0; i < n; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            data[i] = (uint8_t)('a' + seed % 3);
        }
        rc_view_bytes in = {.data = data, .num = n};

        uint32_t mark = fix->scratch.top;
        matches m = scan_matches(in, &fix->scratch);
        uint32_t want = brute_suffix_bits(in, &m, &tables, 0);
        uint32_t got = dp_pass(in, &m, &tables, (rc_span_token) {0}, fix->scratch);
        RC_CHECK(got, ==, want);
        rc_arena_free_to(&fix->scratch, mark);
    }
}

// ---- robustness ----

RC_TEST_STEP(v2, decompress_rejects_prefixes, fix)
{
    static uint8_t buf[600];
    const char *phrase = "abracadabra alakazam ";
    uint32_t plen = (uint32_t)strlen(phrase);
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)phrase[i % plen];
    }
    v2_compress_result c = v2_compress((rc_view_bytes) RC_VIEW(buf), &fix->a, fix->scratch);
    RC_CHECK_TRUE(v2_decompress(c.data.view, &fix->a).ok);

    for (uint32_t n = 0; n < c.data.num; n++) {
        uint32_t mark = fix->a.top;
        rc_view_bytes prefix = {.data = c.data.view.data, .num = n};
        RC_CHECK_FALSE(v2_decompress(prefix, &fix->a).ok);
        rc_arena_free_to(&fix->a, mark);
    }
}

RC_TEST_STEP(v2, decompress_garbage_never_traps, fix)
{
    uint32_t seed = 0xBAADF00D;
    for (uint32_t trial = 0; trial < 400; trial++) {
        uint8_t buf[97];
        uint32_t n = trial % (sizeof buf + 1);
        for (uint32_t i = 0; i < n; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            buf[i] = (uint8_t)(seed >> 8);
        }
        uint32_t mark = fix->a.top;
        v2_decompress_result d = v2_decompress((rc_view_bytes) {.data = buf, .num = n},
                                               &fix->a);
        if (d.ok && n >= 2) {
            uint32_t declared = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
            RC_CHECK(d.data.num, ==, declared);
        }
        rc_arena_free_to(&fix->a, mark);
    }
}

#endif // ENABLE_TESTS
