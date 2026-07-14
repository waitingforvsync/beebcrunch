/*
 *  tables.c - shared learned interval tables for match offsets and lengths
 */

#include "tables.h"

#include "richc/arena.h"
#include "richc/array/u64.h"
#include "richc/macros.h"

#include "bitutils.h"

void table_build_starts(table *t)
{
    RC_ASSERT(t->num_buckets <= table_capacity);
    uint32_t s = t->minval;
    for (uint32_t i = 0; i < t->num_buckets; i++) {
        t->start[i] = s;
        s += 1u << t->width[i];
    }
}

uint32_t table_index(const table *t, uint32_t v)
{
    for (uint32_t i = 0; i < t->num_buckets; i++) {
        if (v >= t->start[i] && v < t->start[i] + (1u << t->width[i])) {
            return i;
        }
    }
    return RC_INDEX_NONE;
}

uint32_t table_cost(const table *t, uint32_t v)
{
    uint32_t i = table_index(t, v);
    if (i == RC_INDEX_NONE) {
        return table_big_cost;
    }
    uint32_t prefix = t->index_bits ? t->index_bits : gamma_bits(i + 1);
    return prefix + t->width[i];
}

// ---- optimizer ----
//
// stats[v] holds the suffix count of histogram entries >= v, so a bucket
// [start, end) covers stats[start] - stats[end] values.  For each
// (start, depth) try every width and keep the one minimizing this
// bucket's bits plus the best partition of the rest; memoized on
// (start - minval) * table_capacity + depth.  The prefix cost per bucket
// is the index code: flat index_bits, or gamma(depth + 1).

static uint64_t opt_rec(rc_view_u32 stats, uint32_t minval, uint32_t maxval,
                        uint32_t start, uint32_t depth, uint32_t index_bits,
                        uint32_t max_buckets,
                        rc_span_u32 memo_width, rc_span_u64 memo_cost,
                        rc_span_u32 memo_seen)
{
    uint32_t key = (start - minval) * table_capacity + depth;
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

table optimize_table(rc_view_u32 hist, uint32_t minval, uint32_t maxval,
                     uint32_t index_bits, uint32_t max_buckets, rc_arena scratch)
{
    RC_ASSERT(max_buckets <= table_capacity);
    table t = {
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
    rc_array_u32_resize(&memo_width, n * table_capacity, &scratch);
    rc_array_u64 memo_cost = {0};
    rc_array_u64_resize(&memo_cost, n * table_capacity, &scratch);
    rc_array_u32 memo_seen = {0};
    rc_array_u32_resize(&memo_seen, n * table_capacity, &scratch);
    for (uint32_t i = 0; i < n * table_capacity; i++) {
        rc_array_u32_set(&memo_seen, i, 0);
    }

    opt_rec(stats.view, minval, maxval, minval, 0, index_bits, max_buckets,
            memo_width.span, memo_cost.span, memo_seen.span);

    // Walk the memoized decisions to extract the winning partition.
    uint32_t start = minval;
    uint32_t depth = 0;
    while (t.num_buckets < max_buckets) {
        uint32_t key = (start - minval) * table_capacity + depth;
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

// ---- serialization ----

uint32_t coding_transmit_bits(const coding_tables *t)
{
    uint32_t bits = table_transmit_bits(&t->len);
    for (uint32_t c = 0; c < num_contexts; c++) {
        bits += table_transmit_bits(&t->off[c]);
    }
    return bits;
}

void write_table(bitwriter *w, const table *t)
{
    RC_ASSERT(t->num_buckets <= table_capacity);
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

void write_value(bitwriter *w, const table *t, uint32_t v)
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

void write_coding_tables(bitwriter *w, const coding_tables *t)
{
    for (uint32_t c = 0; c < num_contexts; c++) {
        write_table(w, &t->off[c]);
    }
    write_table(w, &t->len);
}

table_result read_table(bitreader *r, uint32_t minval, uint32_t index_bits,
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

value_result read_value(bitreader *r, const table *t)
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

coding_tables_result read_coding_tables(bitreader *r)
{
    coding_tables_result res = {.ok = true};
    for (uint32_t c = 0; c < num_contexts; c++) {
        table_result off = read_table(r, 1, off_index_bits, off_max_buckets);
        if (!off.ok) {
            res.ok = false;
            return res;
        }
        res.tables.off[c] = off.table;
    }
    table_result len = read_table(r, 2, 0, len_max_buckets);
    if (!len.ok) {
        res.ok = false;
        return res;
    }
    res.tables.len = len.table;
    return res;
}

coding_tables seed_coding_tables(void)
{
    coding_tables t = {
        .len = {
            .minval = 2,
            .num_buckets = 9,
            .index_bits = 0,
        },
    };
    for (uint32_t c = 0; c < num_contexts; c++) {
        t.off[c] = (table) {
            .minval = 1,
            .num_buckets = 16,
            .index_bits = off_index_bits,
        };
        for (uint32_t i = 0; i < t.off[c].num_buckets; i++) {
            t.off[c].width[i] = i;
        }
        table_build_starts(&t.off[c]);
    }
    for (uint32_t i = 0; i < t.len.num_buckets; i++) {
        t.len.width[i] = i;
    }
    table_build_starts(&t.len);
    return t;
}

uint32_t len1_transmit_bits(const len1_tables *t)
{
    uint32_t bits = table_transmit_bits(&t->off1) + table_transmit_bits(&t->len);
    for (uint32_t c = 0; c < num_contexts; c++) {
        bits += table_transmit_bits(&t->off[c]);
    }
    return bits;
}

void write_len1_tables(bitwriter *w, const len1_tables *t)
{
    write_table(w, &t->off1);
    for (uint32_t c = 0; c < num_contexts; c++) {
        write_table(w, &t->off[c]);
    }
    write_table(w, &t->len);
}

len1_tables_result read_len1_tables(bitreader *r)
{
    len1_tables_result res = {.ok = true};
    table_result off1 = read_table(r, 1, off1_index_bits, off1_max_buckets);
    if (!off1.ok) {
        res.ok = false;
        return res;
    }
    res.tables.off1 = off1.table;
    for (uint32_t c = 0; c < num_contexts; c++) {
        table_result off = read_table(r, 1, off_index_bits, off_max_buckets);
        if (!off.ok) {
            res.ok = false;
            return res;
        }
        res.tables.off[c] = off.table;
    }
    table_result len = read_table(r, 1, 0, len_max_buckets);
    if (!len.ok) {
        res.ok = false;
        return res;
    }
    res.tables.len = len.table;
    return res;
}

len1_tables seed_len1_tables(void)
{
    coding_tables base = seed_coding_tables();
    len1_tables t = {
        .off1 = {
            .minval = 1,
            .num_buckets = 4,
            .index_bits = off1_index_bits,
            .width = {2, 3, 4, 5},
        },
        .len = {
            .minval = 1,
            .num_buckets = 9,
            .index_bits = 0,
        },
    };
    for (uint32_t c = 0; c < num_contexts; c++) {
        t.off[c] = base.off[c];
    }
    for (uint32_t i = 0; i < t.len.num_buckets; i++) {
        t.len.width[i] = i;
    }
    table_build_starts(&t.off1);
    table_build_starts(&t.len);
    return t;
}

// ------------------------------------------------------------------
//  tests
// ------------------------------------------------------------------

#ifdef ENABLE_TESTS

#include "richc/test.h"

RC_TEST_GROUP_DATA(tables) {
    rc_arena a;
};

RC_TEST_GROUP_INIT(tables, fix)
{
    fix->a = rc_arena_make_default();
}

RC_TEST_GROUP_DEINIT(tables, fix)
{
    rc_arena_deinit(&fix->a);
}

RC_TEST(tables, starts_index_cost)
{
    table t = {
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
    RC_CHECK(table_cost(&t, 16), ==, (uint32_t)table_big_cost);

    // Gamma-indexed variant of the same shape.
    t.index_bits = 0;
    RC_CHECK(table_cost(&t, 1), ==, 1u);            // gamma(1) + 0
    RC_CHECK(table_cost(&t, 3), ==, 4u);            // gamma(2)=3 + 1
    RC_CHECK(table_cost(&t, 9), ==, 8u);            // gamma(4)=5 + 3
}

RC_TEST_STEP(tables, serialize_roundtrip, fix)
{
    table t = {
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
    table_result back = read_table(&r, 2, 0, len_max_buckets);
    RC_CHECK_TRUE(back.ok);
    RC_CHECK(r.fault, ==, bit_fault_ok);
    RC_CHECK(back.table.num_buckets, ==, t.num_buckets);
    for (uint32_t i = 0; i < t.num_buckets; i++) {
        RC_CHECK(back.table.width[i], ==, t.width[i]);
        RC_CHECK(back.table.start[i], ==, t.start[i]);
    }
}

RC_TEST_STEP(tables, rejects_oversized_count, fix)
{
    for (uint32_t count = off_max_buckets + 1; count <= 31; count++) {
        rc_array_bytes out = {0};
        uint32_t mark = fix->a.top;
        bitwriter w = bitwriter_make(&out, &fix->a);
        bitwriter_bits(&w, count, 5);
        bitwriter_bits(&w, 0, 8);
        bitwriter_bits(&w, 0, 8);

        bitreader r = bitreader_make(out.view);
        RC_CHECK_FALSE(read_table(&r, 1, off_index_bits, off_max_buckets).ok);
        rc_arena_free_to(&fix->a, mark);
    }
}

RC_TEST_STEP(tables, value_roundtrip_wide_extras, fix)
{
    // A late bucket with width 15 exercises the split extra-field path.
    table t = {
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

static uint64_t hist_data_bits(rc_view_u32 hist, uint32_t minval, uint32_t maxval,
                               const table *t)
{
    uint64_t bits = 0;
    for (uint32_t v = minval; v <= maxval; v++) {
        uint32_t count = rc_view_u32_get(hist, v);
        if (count > 0) {
            uint32_t c = table_cost(t, v);
            RC_CHECK(c, <, (uint32_t)table_big_cost);
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

    table t = optimize_table(hist, minval, maxval, index_bits, 16, *a);
    if (acc == 0) {
        RC_CHECK(t.num_buckets, ==, 0u);
        return;
    }
    uint64_t got = hist_data_bits(hist, minval, maxval, &t);
    uint64_t want = brute_partition(stats.view, maxval, minval, 0, index_bits);
    RC_CHECK(got, ==, want);
}

RC_TEST_STEP(tables, optimizer_matches_brute_force, fix)
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

#endif // ENABLE_TESTS
