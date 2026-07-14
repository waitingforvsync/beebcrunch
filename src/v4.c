/*
 *  v4.c - the v4 codec: v2's shape plus length-1 matches
 */

#include "v4.h"

#include "richc/arena.h"
#include "richc/macros.h"

#include "bitreader.h"
#include "bitwriter.h"
#include "matches.h"
#include "tables.h"

enum {
    v4_iterations = 8,      // parse <-> table refinement rounds
};

// ---- optimal forward parse ----
//
// v2's exact forward DP plus one edge per position: the nearest previous
// occurrence of the current byte (matches.near1) as a length-1 match.
// Same exactness caveat as v2 over nearest-offset candidates.

static uint32_t dp_pass(rc_view_bytes in, const matches *m, const len1_tables *tables,
                        rc_span_token arrival, rc_arena scratch)
{
    uint32_t n = in.num;
    rc_array_u32 cost = {0};
    rc_array_u32_resize(&cost, n + 1, &scratch);
    rc_array_u32_set(&cost, 0, 0);
    for (uint32_t j = 1; j <= n; j++) {
        rc_array_u32_set(&cost, j, UINT32_MAX);
    }

    // Hoist the length-value-1 cost: every length-1 edge pays it.
    uint32_t len1_cost = table_cost(&tables->len, 1);

    for (uint32_t i = 0; i < n; i++) {
        uint32_t here = rc_array_u32_get(&cost, i);

        // Literal edge: flag bit + aligned byte.
        uint32_t lit = here + 1 + 8;
        if (lit < rc_array_u32_get(&cost, i + 1)) {
            rc_array_u32_set(&cost, i + 1, lit);
            if (arrival.num) {
                rc_span_token_set(arrival, i + 1, (token) {
                    .length = 1,
                    .offset = 0,
                });
            }
        }

        // Length-1 match edge: repeat the byte from near1[i] ago.
        uint32_t d = rc_array_u32_get(&m->near1, i);
        if (d != 0 && len1_cost < table_big_cost) {
            uint32_t oc = table_cost(&tables->off1, d);
            if (oc < table_big_cost) {
                uint32_t c = here + 1 + len1_cost + oc;
                if (c < rc_array_u32_get(&cost, i + 1)) {
                    rc_array_u32_set(&cost, i + 1, c);
                    if (arrival.num) {
                        rc_span_token_set(arrival, i + 1, (token) {
                            .length = 1,
                            .offset = (uint16_t)d,
                        });
                    }
                }
            }
        }

        // Match edges: every truncation of every breakpoint the tables can
        // encode, as v2.
        uint32_t lo = min_match - 1;
        uint32_t end = rc_array_u32_get(&m->start, i + 1);
        for (uint32_t k = rc_array_u32_get(&m->start, i); k < end; k++) {
            token t = rc_array_token_get(&m->bp, k);
            uint32_t off_cost[num_contexts];
            bool any_covered = false;
            for (uint32_t c = 0; c < num_contexts; c++) {
                off_cost[c] = table_cost(&tables->off[c], t.offset);
                any_covered = any_covered || off_cost[c] < table_big_cost;
            }
            if (!any_covered) {
                break;
            }
            for (uint32_t len = lo + 1; len <= t.length; len++) {
                uint32_t len_cost = table_cost(&tables->len, len);
                if (len_cost >= table_big_cost) {
                    break;
                }
                uint32_t oc = off_cost[len_ctx(len)];
                if (oc >= table_big_cost) {
                    continue;
                }
                uint32_t c = here + 1 + oc + len_cost;
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
static uint32_t data_bits(rc_view_token tokens, const len1_tables *tables)
{
    uint32_t bits = 0;
    for (uint32_t k = 0; k < tokens.num; k++) {
        token t = rc_view_token_get(tokens, k);
        if (token_is_literal(t)) {
            bits += 1 + 8;
        }
        else {
            bits += 1 + table_cost(&tables->len, t.length)
                  + table_cost(len1_off_table(tables, t.length), t.offset);
        }
    }
    return bits;
}

// ---- encoding ----

static rc_array_bytes encode(rc_view_bytes in, rc_view_token tokens,
                             const len1_tables *tables, rc_arena *arena)
{
    rc_array_bytes out = {0};
    rc_array_bytes_reserve(&out, 64, arena);

    // Raw two-byte length header, then everything else through the writer.
    rc_array_bytes_push(&out, (uint8_t)(in.num), arena);
    rc_array_bytes_push(&out, (uint8_t)(in.num >> 8), arena);

    bitwriter w = bitwriter_make(&out, arena);
    write_len1_tables(&w, tables);

    uint32_t total = 0;
    for (uint32_t k = 0; k < tokens.num; k++) {
        token t = rc_view_token_get(tokens, k);
        if (token_is_literal(t)) {
            bitwriter_bits(&w, 0, 1);
            bitwriter_byte(&w, rc_view_bytes_get(in, total));
        }
        else {
            // Match: length first (the decoder needs it to pick the offset
            // context, including length 1), then the offset.
            bitwriter_bits(&w, 1, 1);
            write_value(&w, &tables->len, t.length);
            write_value(&w, len1_off_table(tables, t.length), t.offset);
        }
        total += t.length;
    }
    RC_ASSERT(total == in.num);
    return out;
}

v4_compress_result v4_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    RC_ASSERT(arena);
    RC_ASSERT(in.num <= v4_max_uncompressed);
    RC_ASSERT(arena->base != scratch.base);

    uint32_t n = in.num;

    // Find and cache all match candidates once (near1 included).
    matches m = scan_matches(in, &scratch);

    // v2's refinement fixpoint, now over five tables.
    len1_tables tables = seed_len1_tables();
    len1_tables best_tables = tables;
    rc_array_token best_tokens = {0};
    uint32_t best_bits = UINT32_MAX;

    rc_array_token arrival = {0};
    rc_array_token_resize(&arrival, n + 1, &scratch);

    for (uint32_t iter = 0; iter < v4_iterations; iter++) {
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
        rc_array_u32 off1_hist = {0};
        rc_array_u32_resize(&off1_hist, n + 1, &scratch);
        for (uint32_t v = 0; v <= n; v++) {
            rc_array_u32_set(&off1_hist, v, 0);
        }
        rc_array_u32 off_hist[num_contexts];
        for (uint32_t c = 0; c < num_contexts; c++) {
            off_hist[c] = (rc_array_u32) {0};
            rc_array_u32_resize(&off_hist[c], n + 1, &scratch);
            for (uint32_t v = 0; v <= n; v++) {
                rc_array_u32_set(&off_hist[c], v, 0);
            }
        }
        uint32_t max_len = 0;
        uint32_t max_off1 = 0;
        uint32_t max_off[num_contexts] = {0};
        for (uint32_t k = 0; k < tokens.num; k++) {
            token t = rc_view_token_get(tokens.view, k);
            if (token_is_literal(t)) {
                continue;
            }
            rc_array_u32_set(&len_hist, t.length, rc_array_u32_get(&len_hist, t.length) + 1);
            if (t.length > max_len) {
                max_len = t.length;
            }
            if (t.length == 1) {
                rc_array_u32_set(&off1_hist, t.offset, rc_array_u32_get(&off1_hist, t.offset) + 1);
                if (t.offset > max_off1) {
                    max_off1 = t.offset;
                }
            }
            else {
                uint32_t c = len_ctx(t.length);
                rc_array_u32_set(&off_hist[c], t.offset, rc_array_u32_get(&off_hist[c], t.offset) + 1);
                if (t.offset > max_off[c]) {
                    max_off[c] = t.offset;
                }
            }
        }

        // ...and rebuild optimal tables for exactly that usage.
        len1_tables next = {
            .off1 = optimize_table(off1_hist.view, 1, max_off1,
                                   off1_index_bits, off1_max_buckets, scratch),
            .len = optimize_table(len_hist.view, 1, max_len, 0,
                                  len_max_buckets, scratch),
        };
        for (uint32_t c = 0; c < num_contexts; c++) {
            next.off[c] = optimize_table(off_hist[c].view, 1, max_off[c],
                                         off_index_bits, off_max_buckets, scratch);
        }

        uint32_t bits = len1_transmit_bits(&next) + data_bits(tokens.view, &next);
        if (bits < best_bits) {
            best_bits = bits;
            best_tokens = tokens;
            best_tables = next;
        }
        tables = next;
    }

    return (v4_compress_result) {
        .data = encode(in, best_tokens.view, &best_tables, arena),
        .num_tokens = best_tokens.num,
    };
}

// ---- decoding ----

static v4_decompress_result fail(void) { return (v4_decompress_result) {.data = {0}, .ok = false}; }

v4_decompress_result v4_decompress(rc_view_bytes comp, rc_arena *arena)
{
    RC_ASSERT(arena);

    // Length header first: it bounds everything that follows.
    if (comp.num < 2) {
        return fail();
    }
    uint32_t len = (uint32_t)rc_view_bytes_get(comp, 0)
                 | ((uint32_t)rc_view_bytes_get(comp, 1) << 8);

    bitreader r = bitreader_make(rc_view_bytes_get_tail(comp, 2));
    len1_tables_result tables = read_len1_tables(&r);
    if (!tables.ok || r.fault != bit_fault_ok) {
        return fail();
    }

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
            // Match: length first (it selects the offset context, now
            // including length 1), then the offset, both validated - the
            // stream is untrusted.
            value_result length = read_value(&r, &tables.tables.len);
            if (!length.ok || r.fault != bit_fault_ok) {
                return fail();
            }
            value_result offset = read_value(&r, len1_off_table(&tables.tables, length.value));
            if (!offset.ok || r.fault != bit_fault_ok) {
                return fail();
            }
            if (offset.value > out.num || out.num + length.value > len) {
                return fail();
            }
            // Forward byte-by-byte copy handles overlap; a length-1 match
            // is simply one iteration.
            uint32_t src = out.num - offset.value;
            for (uint32_t k = 0; k < length.value; k++) {
                rc_array_bytes_push(&out, rc_array_bytes_get(&out, src + k), arena);
            }
        }
    }
    return (v4_decompress_result) {.data = out, .ok = true};
}

// ------------------------------------------------------------------
//  tests
// ------------------------------------------------------------------

#ifdef ENABLE_TESTS

#include <string.h>

#include "richc/test.h"

#include "v2.h"

RC_TEST_GROUP_DATA(v4) {
    rc_arena a;
    rc_arena scratch;
};

RC_TEST_GROUP_INIT(v4, fix)
{
    fix->a = rc_arena_make(1u << 30);
    fix->scratch = rc_arena_make(1u << 30);
}

RC_TEST_GROUP_DEINIT(v4, fix)
{
    rc_arena_deinit(&fix->scratch);
    rc_arena_deinit(&fix->a);
}

static uint32_t roundtrip(rc_view_bytes in, struct rc_test_group_data_v4 *fix)
{
    v4_compress_result c = v4_compress(in, &fix->a, fix->scratch);
    RC_CHECK(c.data.num, >=, 2u);
    RC_CHECK(rc_array_bytes_get(&c.data, 0), ==, (uint8_t)in.num);
    RC_CHECK(rc_array_bytes_get(&c.data, 1), ==, (uint8_t)(in.num >> 8));

    v4_decompress_result d = v4_decompress(c.data.view, &fix->a);
    RC_CHECK_TRUE(d.ok);
    RC_CHECK(d.data.num, ==, in.num);
    if (in.num > 0) {
        RC_CHECK_TRUE(memcmp(d.data.data, in.data, in.num) == 0);
    }
    return c.data.num;
}

RC_TEST_STEP(v4, roundtrip_edges, fix)
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

RC_TEST_STEP(v4, roundtrip_random_incompressible, fix)
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

RC_TEST_STEP(v4, roundtrip_text_beats_v2, fix)
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

    // Length-1 matches are a strict superset of v2's toolkit.
    v2_compress_result v2 = v2_compress(in, &fix->a, fix->scratch);
    RC_CHECK(packed, <=, v2.data.num);
}

RC_TEST_STEP(v4, len1_wins_on_unique_pairs, fix)
{
    // 0,1,0,2,0,3,...: no 2-byte pair repeats, so v2 can only emit
    // literals; v4 codes every second byte as a length-1 match at
    // distance 2 and must win decisively.
    static uint8_t buf[2048];
    uint32_t k = 0;
    for (uint32_t i = 0; i < sizeof buf; i++) {
        if (i & 1) {
            k = (k % 255) + 1;      // 1..255, cycling; pairs stay unique
            buf[i] = (uint8_t)k;
        }
        else {
            buf[i] = 0;
        }
    }
    rc_view_bytes in = (rc_view_bytes) RC_VIEW(buf);
    uint32_t v4_size = roundtrip(in, fix);
    v2_compress_result v2 = v2_compress(in, &fix->a, fix->scratch);
    RC_CHECK(v4_size + 64, <, v2.data.num);
}

RC_TEST_STEP(v4, roundtrip_far_offsets, fix)
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

RC_TEST_STEP(v4, roundtrip_max_size, fix)
{
    static uint8_t buf[v4_max_uncompressed];
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
                                  const len1_tables *tables, uint32_t pos)
{
    if (pos == in.num) {
        return 0;
    }
    uint32_t best = 9 + brute_suffix_bits(in, m, tables, pos + 1);

    // Length-1 edge.
    uint32_t d = rc_array_u32_get(&m->near1, pos);
    if (d != 0) {
        uint32_t lc = table_cost(&tables->len, 1);
        uint32_t oc = table_cost(&tables->off1, d);
        if (lc < table_big_cost && oc < table_big_cost) {
            uint32_t c = 1 + lc + oc + brute_suffix_bits(in, m, tables, pos + 1);
            if (c < best) {
                best = c;
            }
        }
    }

    uint32_t lo = min_match - 1;
    uint32_t end = rc_array_u32_get(&m->start, pos + 1);
    for (uint32_t k = rc_array_u32_get(&m->start, pos); k < end; k++) {
        token t = rc_array_token_get(&m->bp, k);
        for (uint32_t len = lo + 1; len <= t.length; len++) {
            uint32_t len_cost = table_cost(&tables->len, len);
            if (len_cost >= table_big_cost) {
                break;
            }
            uint32_t off_cost = table_cost(&tables->off[len_ctx(len)], t.offset);
            if (off_cost >= table_big_cost) {
                continue;
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

RC_TEST_STEP(v4, parse_is_exact_for_fixed_tables, fix)
{
    len1_tables tables = seed_len1_tables();
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

RC_TEST_STEP(v4, decompress_rejects_prefixes, fix)
{
    static uint8_t buf[600];
    const char *phrase = "abracadabra alakazam ";
    uint32_t plen = (uint32_t)strlen(phrase);
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)phrase[i % plen];
    }
    v4_compress_result c = v4_compress((rc_view_bytes) RC_VIEW(buf), &fix->a, fix->scratch);
    RC_CHECK_TRUE(v4_decompress(c.data.view, &fix->a).ok);

    for (uint32_t n = 0; n < c.data.num; n++) {
        uint32_t mark = fix->a.top;
        rc_view_bytes prefix = {.data = c.data.view.data, .num = n};
        RC_CHECK_FALSE(v4_decompress(prefix, &fix->a).ok);
        rc_arena_free_to(&fix->a, mark);
    }
}

RC_TEST_STEP(v4, decompress_garbage_never_traps, fix)
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
        v4_decompress_result d = v4_decompress((rc_view_bytes) {.data = buf, .num = n},
                                               &fix->a);
        if (d.ok && n >= 2) {
            uint32_t declared = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
            RC_CHECK(d.data.num, ==, declared);
        }
        rc_arena_free_to(&fix->a, mark);
    }
}

#endif // ENABLE_TESTS
