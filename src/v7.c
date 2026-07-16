/*
 *  v7.c - the v7 codec: v6's coding inside block framing
 */

#include "v7.h"

#include "richc/arena.h"
#include "richc/macros.h"

#include "bitreader.h"
#include "bitutils.h"
#include "bitwriter.h"
#include "frontier.h"
#include "matches.h"
#include "tables.h"

enum {
    v7_max_run         = 256,   // cap on the block count
    v7_iterations      = 8,     // parse <-> table refinement rounds
    v7_len_buckets = 13,        // pinned unary length index, bare nibbles
    v7_cnt_buckets = 9,         // pinned unary count index per block type
                                // (sweet spot of the len x cnt sweep; all
                                // neighbours measured worse by 9-55 bytes)
    v7_lit             = 0,
    v7_match           = 1,
};

// v6's five tables plus a learned count table per block type.  A unary
// bucket index with geometric widths costs (i + 1) + i = 2i + 1 bits -
// exactly Elias gamma - so the seed starts at gamma parity and the
// optimizer can only improve per file.  No Elias gamma remains anywhere
// in the v7 stream: unary indices, flat indices and raw bits only.
typedef struct v7_tables {
    len1_tables base;
    table       cnt[2];     // v7_lit / v7_match run counts, [1, 256]
} v7_tables;

static uint32_t v7_transmit_bits(const v7_tables *t)
{
    return len1_transmit_bits(&t->base)
         + table_transmit_bits(&t->cnt[v7_lit])
         + table_transmit_bits(&t->cnt[v7_match]);
}

static table seed_cnt_table(void)
{
    table t = {
        .minval = 1,
        .num_buckets = 9,
        .index_bits = index_unary,
    };
    for (uint32_t i = 0; i < t.num_buckets; i++) {
        t.width[i] = i;
    }
    table_build_starts(&t);
    return t;
}

static v7_tables v7_seed_tables(void)
{
    v7_tables t = {
        .base = seed_len1_tables(),
    };
    t.base.len.index_bits = index_unary;
    t.cnt[v7_lit] = seed_cnt_table();
    t.cnt[v7_match] = seed_cnt_table();
    return t;
}

// ---- optimal forward parse ----
//
// v3's machinery: a Pareto frontier of (run, cost-excluding-pending-
// header) states per (position, arriving token type); see frontier.h.
// Token payloads are v6's: table costs, offset context by length class
// including length 1.  The length-1 edge emits a match token, so it
// extends match runs - a lone repeated byte no longer forces a literal
// block break.  The pending header is a count-table cost, not plain
// gamma; frontier dominance assumes header cost monotone in run length -
// exact for the gamma-equivalent seed, heuristic for refined tables (the
// fixpoint keeps the best true size either way).

typedef struct v7_parse_result {
    rc_array_token tokens;  // filled only when want_tokens
    uint32_t       bits;    // stream bits after the tables, or UINT32_MAX
                            // when a run over v7_max_run was unavoidable
} v7_parse_result;

// All working memory - and the tokens - come from arena; the caller
// brackets each pass with an arena mark.
static v7_parse_result v7_parse(rc_view_bytes in, const matches *m,
                                const v7_tables *tables, bool want_tokens,
                                rc_arena *arena)
{
    uint32_t n = in.num;
    RC_ASSERT(n > 0);

    // Frontier heads per type, then the growing node pool last so it can
    // grow in place.
    rc_array_u32 heads[2];
    for (uint32_t t = 0; t < 2; t++) {
        heads[t] = (rc_array_u32) {0};
        rc_array_u32_resize(&heads[t], n + 1, arena);
        for (uint32_t j = 0; j <= n; j++) {
            rc_array_u32_set(&heads[t], j, RC_INDEX_NONE);
        }
    }
    rc_array_node pool = {0};
    rc_array_node_reserve(&pool, n + 1, arena);

    // Hoist the length-value-1 cost: every length-1 edge pays it.
    uint32_t len1_cost = table_cost(&tables->base.len, 1);

    // The stream always opens with a literal block: seed the state after
    // the first literal.
    frontier_insert(&pool, &heads[v7_lit], 1, 1, 8, RC_INDEX_NONE, v7_lit, 0, (token) {
        .length = 1,
        .offset = 0,
    }, arena);

    for (uint32_t i = 1; i < n; i++) {
        for (uint32_t type = 0; type < 2; type++) {
            uint32_t idx = rc_array_u32_get(&heads[type], i);
            while (idx != RC_INDEX_NONE) {
                // Copy the node: the pool grows while this state fans out.
                frontier_node s = rc_array_node_get(&pool, idx);

                // Realized bits if this run's header were paid now; a run
                // the count table cannot code can still extend, but never
                // switch type or finish.
                uint32_t header = table_cost(&tables->cnt[type], s.run);
                bool can_close = header < table_big_cost;
                uint32_t actual = can_close ? s.cost + header : UINT32_MAX;

                // Literal edge: extend the literal run (if the block cap
                // allows) or realize the match run's header and open a
                // fresh literal block.
                token lit = {
                    .length = 1,
                    .offset = 0,
                };
                if (type == v7_lit) {
                    if (s.run < v7_max_run) {
                        frontier_insert(&pool, &heads[v7_lit], i + 1, s.run + 1,
                                        s.cost + 8, i, v7_lit, s.run, lit, arena);
                    }
                }
                else if (can_close) {
                    frontier_insert(&pool, &heads[v7_lit], i + 1, 1,
                                    actual + 8, i, v7_match, s.run, lit, arena);
                }

                // Length-1 match edge: repeat the byte from near1[i] ago.
                // A match token, with the same run-or-new-run split as the
                // longer matches below.
                uint32_t d = rc_array_u32_get(&m->near1, i);
                if (d != 0 && len1_cost < table_big_cost) {
                    uint32_t oc = table_cost(&tables->base.off[0], d);
                    if (oc < table_big_cost) {
                        uint32_t payload = len1_cost + oc;
                        token rt = {
                            .length = 1,
                            .offset = (uint16_t)d,
                        };
                        if (type == v7_match) {
                            if (s.run < v7_max_run) {
                                frontier_insert(&pool, &heads[v7_match], i + 1,
                                                s.run + 1, s.cost + payload,
                                                i, v7_match, s.run, rt, arena);
                            }
                        }
                        else if (can_close) {
                            frontier_insert(&pool, &heads[v7_match], i + 1,
                                            1, actual + payload,
                                            i, v7_lit, s.run, rt, arena);
                        }
                    }
                }

                // Match edges: every truncation of every breakpoint the
                // tables can encode.  Offset cost depends on the length
                // class, so hoist all three per breakpoint; when every
                // context leaves this offset uncovered, every farther
                // breakpoint is uncovered too (contiguous coverage).
                uint32_t lo = min_match - 1;
                uint32_t end = rc_array_u32_get(&m->start, i + 1);
                for (uint32_t k = rc_array_u32_get(&m->start, i); k < end; k++) {
                    token t = rc_array_token_get(&m->bp, k);
                    uint32_t off_cost[num_off_tables];
                    bool any_covered = false;
                    for (uint32_t c = 1; c < num_off_tables; c++) {
                        off_cost[c] = table_cost(&tables->base.off[c], t.offset);
                        any_covered = any_covered || off_cost[c] < table_big_cost;
                    }
                    if (!any_covered) {
                        break;
                    }
                    for (uint32_t len = lo + 1; len <= t.length; len++) {
                        uint32_t len_cost = table_cost(&tables->base.len, len);
                        if (len_cost >= table_big_cost) {
                            break;
                        }
                        uint32_t oc = off_cost[off_ctx(len)];
                        if (oc >= table_big_cost) {
                            continue;
                        }
                        uint32_t payload = len_cost + oc;
                        token mt = {
                            .length = (uint16_t)len,
                            .offset = t.offset,
                        };
                        if (type == v7_match) {
                            if (s.run < v7_max_run) {
                                frontier_insert(&pool, &heads[v7_match], i + len,
                                                s.run + 1, s.cost + payload,
                                                i, v7_match, s.run, mt, arena);
                            }
                        }
                        else if (can_close) {
                            frontier_insert(&pool, &heads[v7_match], i + len,
                                            1, actual + payload,
                                            i, v7_lit, s.run, mt, arena);
                        }
                    }
                    lo = t.length;
                }
                idx = s.next;
            }
        }
    }

    // Best realized cost over both frontiers at the end.
    uint32_t best_bits = UINT32_MAX;
    uint32_t best_idx = RC_INDEX_NONE;
    for (uint32_t type = 0; type < 2; type++) {
        uint32_t idx = rc_array_u32_get(&heads[type], n);
        while (idx != RC_INDEX_NONE) {
            frontier_node s = rc_array_node_get(&pool, idx);
            uint32_t header = table_cost(&tables->cnt[type], s.run);
            if (header >= table_big_cost) {
                idx = s.next;
                continue;
            }
            uint32_t bits = s.cost + header;
            if (bits < best_bits) {
                best_bits = bits;
                best_idx = idx;
            }
            idx = s.next;
        }
    }
    v7_parse_result res = {
        .tokens = {0},
        .bits = best_bits,
    };
    if (best_idx == RC_INDEX_NONE) {
        res.bits = UINT32_MAX;
        return res;
    }

    if (want_tokens) {
        // Walk the back-links (position, type, run identify a unique live
        // state) and reverse into stream order.
        rc_array_token_reserve(&res.tokens, n, arena);
        uint32_t idx = best_idx;
        for (;;) {
            frontier_node s = rc_array_node_get(&pool, idx);
            rc_array_token_push(&res.tokens, s.tok, arena);
            if (s.from_index == RC_INDEX_NONE) {
                break;
            }
            uint32_t head = rc_array_u32_get(&heads[s.from_type], s.from_index);
            idx = frontier_find(pool.view, head, s.from_run);
            RC_PANIC(idx != RC_INDEX_NONE);
        }
        rc_span_token_reverse(res.tokens.span);
    }
    return res;
}

// Exact stream bits of a token sequence: count-table block headers over
// maximal same-type runs + table-coded payloads (excludes table headers).
// Length-1 matches group as matches, so blocks split on token_is_literal.
static uint32_t data_bits(rc_view_token tokens, const v7_tables *tables)
{
    uint32_t bits = 0;
    uint32_t i = 0;
    while (i < tokens.num) {
        bool is_lit = token_is_literal(rc_view_token_get(tokens, i));
        uint32_t j = i;
        while (j < tokens.num && token_is_literal(rc_view_token_get(tokens, j)) == is_lit) {
            j++;
        }
        bits += table_cost(&tables->cnt[is_lit ? v7_lit : v7_match], j - i);
        for (uint32_t k = i; k < j; k++) {
            token t = rc_view_token_get(tokens, k);
            if (is_lit) {
                bits += 8;
            }
            else {
                bits += table_cost(&tables->base.len, t.length)
                      + table_cost(len1_off_table(&tables->base, t.length), t.offset);
            }
        }
        i = j;
    }
    return bits;
}

// ---- encoding ----

static rc_array_bytes encode(rc_view_bytes in, rc_view_token tokens,
                             const v7_tables *tables, rc_arena *arena)
{
    rc_array_bytes out = {0};
    rc_array_bytes_reserve(&out, 64, arena);

    // Raw two-byte length header, then everything else through the writer.
    rc_array_bytes_push(&out, (uint8_t)(in.num), arena);
    rc_array_bytes_push(&out, (uint8_t)(in.num >> 8), arena);

    bitwriter w = bitwriter_make(&out, arena);
    for (uint32_t c = 0; c < num_off_tables; c++) {
        write_fixed_table(&w, &tables->base.off[c], off_ctx_buckets(c));
    }
    write_fixed_table(&w, &tables->base.len, v7_len_buckets);
    write_fixed_table(&w, &tables->cnt[v7_lit], v7_cnt_buckets);
    write_fixed_table(&w, &tables->cnt[v7_match], v7_cnt_buckets);

    // Emit maximal same-type runs: gamma(count) then the tokens.  The
    // parse guarantees the block cap and the leading literal block.
    uint32_t total = 0;
    uint32_t i = 0;
    while (i < tokens.num) {
        bool is_lit = token_is_literal(rc_view_token_get(tokens, i));
        RC_ASSERT(i > 0 || is_lit);
        uint32_t j = i;
        while (j < tokens.num && token_is_literal(rc_view_token_get(tokens, j)) == is_lit) {
            j++;
        }
        RC_ASSERT(j - i <= v7_max_run);
        write_value(&w, &tables->cnt[is_lit ? v7_lit : v7_match], j - i);
        for (uint32_t k = i; k < j; k++) {
            token t = rc_view_token_get(tokens, k);
            if (is_lit) {
                bitwriter_byte(&w, rc_view_bytes_get(in, total));
            }
            else {
                // Length first (the decoder needs it to pick the offset
                // context, including length 1), then the offset.
                write_value(&w, &tables->base.len, t.length);
                write_value(&w, len1_off_table(&tables->base, t.length), t.offset);
            }
            total += t.length;
        }
        i = j;
    }
    RC_ASSERT(total == in.num);
    return out;
}

v7_compress_result v7_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    RC_ASSERT(arena);
    RC_ASSERT(in.num <= v7_max_uncompressed);
    RC_ASSERT(arena->base != scratch.base);

    uint32_t n = in.num;

    // Empty input: header plus the (empty) tables, no blocks at all.
    if (n == 0) {
        v7_tables empty = {
            .base = {
                .len = {.minval = 1},
            },
            .cnt = {{.minval = 1}, {.minval = 1}},
        };
        for (uint32_t c = 0; c < num_off_tables; c++) {
            empty.base.off[c] = (table) {.minval = 1};
        }
        return (v7_compress_result) {
            .data = encode(in, (rc_view_token) {0}, &empty, arena),
            .num_tokens = 0,
            .ok = true,
        };
    }

    // Find and cache all match candidates once (near1 included).
    matches m = scan_matches(in, &scratch);

    // Best tokens live in a fixed-size buffer allocated before the loop,
    // so each iteration's (large) frontier pool can be reclaimed while
    // the winner survives.
    rc_array_token best_tokens = {0};
    rc_array_token_resize(&best_tokens, n, &scratch);
    uint32_t best_num = 0;
    v7_tables best_tables;
    uint32_t best_bits = UINT32_MAX;

    // v3's refinement fixpoint, now over seven tables.  Refined tables can
    // shrink coverage enough to make a later parse fail; the best earlier
    // result still stands.  If even the tuned seed parse fails (a run
    // over 256 literals whose only escape valves lie beyond the seed's
    // off1 coverage), retry with a full-coverage length-1 seed: at most
    // 256 positions in a file are first occurrences of their byte, so a
    // 257-literal run always contains a length-1 escape, and every input
    // is representable.
    for (uint32_t attempt = 0; attempt < 2 && best_bits == UINT32_MAX; attempt++) {
    v7_tables tables = v7_seed_tables();
    if (attempt == 1) {
        for (uint32_t i = 0; i < off1_max_buckets; i++) {
            tables.base.off[0].width[i] = 15;
        }
        table_build_starts(&tables.base.off[0]);
    }
    for (uint32_t iter = 0; iter < v7_iterations; iter++) {
        uint32_t mark = scratch.top;
        v7_parse_result parsed = v7_parse(in, &m, &tables, true, &scratch);
        if (parsed.bits == UINT32_MAX) {
            rc_arena_free_to(&scratch, mark);
            break;
        }

        // Histogram the values this parse actually used...
        rc_array_u32 len_hist = {0};
        rc_array_u32_resize(&len_hist, max_match + 1, &scratch);
        for (uint32_t v = 0; v <= max_match; v++) {
            rc_array_u32_set(&len_hist, v, 0);
        }
        rc_array_u32 off_hist[num_off_tables];
        for (uint32_t c = 0; c < num_off_tables; c++) {
            off_hist[c] = (rc_array_u32) {0};
            rc_array_u32_resize(&off_hist[c], n + 1, &scratch);
            for (uint32_t v = 0; v <= n; v++) {
                rc_array_u32_set(&off_hist[c], v, 0);
            }
        }
        rc_array_u32 cnt_hist[2];
        for (uint32_t t = 0; t < 2; t++) {
            cnt_hist[t] = (rc_array_u32) {0};
            rc_array_u32_resize(&cnt_hist[t], v7_max_run + 1, &scratch);
            for (uint32_t v = 0; v <= v7_max_run; v++) {
                rc_array_u32_set(&cnt_hist[t], v, 0);
            }
        }
        uint32_t max_len = 0;
        uint32_t max_off[num_off_tables] = {0};
        uint32_t max_cnt[2] = {0};

        // Block runs for the count histograms.
        uint32_t bi = 0;
        while (bi < parsed.tokens.num) {
            bool is_lit = token_is_literal(rc_view_token_get(parsed.tokens.view, bi));
            uint32_t bj = bi;
            while (bj < parsed.tokens.num
                   && token_is_literal(rc_view_token_get(parsed.tokens.view, bj)) == is_lit) {
                bj++;
            }
            uint32_t t = is_lit ? v7_lit : v7_match;
            rc_array_u32_set(&cnt_hist[t], bj - bi, rc_array_u32_get(&cnt_hist[t], bj - bi) + 1);
            if (bj - bi > max_cnt[t]) {
                max_cnt[t] = bj - bi;
            }
            bi = bj;
        }

        for (uint32_t k = 0; k < parsed.tokens.num; k++) {
            token t = rc_view_token_get(parsed.tokens.view, k);
            if (token_is_literal(t)) {
                continue;
            }
            rc_array_u32_set(&len_hist, t.length, rc_array_u32_get(&len_hist, t.length) + 1);
            if (t.length > max_len) {
                max_len = t.length;
            }
            uint32_t c = off_ctx(t.length);
            rc_array_u32_set(&off_hist[c], t.offset, rc_array_u32_get(&off_hist[c], t.offset) + 1);
            if (t.offset > max_off[c]) {
                max_off[c] = t.offset;
            }
        }

        // ...and rebuild optimal tables for exactly that usage.
        v7_tables next = {
            .base = {
                .len = optimize_table(len_hist.view, 1, max_len, index_unary,
                                      v7_len_buckets, scratch),
            },
        };
        for (uint32_t c = 0; c < num_off_tables; c++) {
            next.base.off[c] = optimize_table(off_hist[c].view, 1, max_off[c],
                                              off_ctx_index_bits(c), off_ctx_buckets(c), scratch);
        }
        for (uint32_t t = 0; t < 2; t++) {
            next.cnt[t] = optimize_table(cnt_hist[t].view, 1, max_cnt[t],
                                         index_unary, v7_cnt_buckets, scratch);
        }

        // v7_transmit_bits overstates the pinned-count offset headers by
        // a constant 20 bits; a constant shift cannot change the argmin.
        uint32_t bits = v7_transmit_bits(&next) + data_bits(parsed.tokens.view, &next);
        if (bits < best_bits) {
            best_bits = bits;
            best_num = parsed.tokens.num;
            for (uint32_t k = 0; k < best_num; k++) {
                rc_array_token_set(&best_tokens, k, rc_view_token_get(parsed.tokens.view, k));
            }
            best_tables = next;
        }
        tables = next;
        rc_arena_free_to(&scratch, mark);
    }
    }

    if (best_bits == UINT32_MAX) {
        // Even the full-coverage seed parse found no representable path.
        return (v7_compress_result) {.ok = false};
    }

    rc_view_token winner = {
        .data = best_tokens.view.data,
        .num = best_num,
    };

    return (v7_compress_result) {
        .data = encode(in, winner, &best_tables, arena),
        .num_tokens = best_num,
        .ok = true,
    };
}

// ---- decoding ----

static v7_decompress_result fail(void) { return (v7_decompress_result) {.data = {0}, .ok = false}; }

v7_decompress_result v7_decompress(rc_view_bytes comp, rc_arena *arena)
{
    RC_ASSERT(arena);

    // Length header first: it bounds everything that follows.
    if (comp.num < 2) {
        return fail();
    }
    uint32_t len = (uint32_t)rc_view_bytes_get(comp, 0)
                 | ((uint32_t)rc_view_bytes_get(comp, 1) << 8);

    bitreader r = bitreader_make(rc_view_bytes_get_tail(comp, 2));
    v7_tables tables;
    for (uint32_t c = 0; c < num_off_tables; c++) {
        tables.base.off[c] = read_fixed_table(&r, 1, off_ctx_index_bits(c), off_ctx_buckets(c));
    }
    tables.base.len = read_fixed_table(&r, 1, index_unary, v7_len_buckets);
    for (uint32_t t = 0; t < 2; t++) {
        tables.cnt[t] = read_fixed_table(&r, 1, index_unary, v7_cnt_buckets);
    }
    if (r.fault != bit_fault_ok) {
        return fail();
    }

    rc_array_bytes out = {0};
    rc_array_bytes_reserve(&out, len ? len : 1, arena);
    while (out.num < len) {
        // Literal block: table-coded count, then that many aligned bytes,
        // each bounded by the declared length - the stream is untrusted.
        value_result lit_count = read_value(&r, &tables.cnt[v7_lit]);
        if (!lit_count.ok || r.fault != bit_fault_ok) {
            return fail();
        }
        uint32_t count = lit_count.value;
        for (uint32_t k = 0; k < count; k++) {
            if (out.num >= len) {
                return fail();
            }
            uint8_t v = bitreader_byte(&r);
            if (r.fault != bit_fault_ok) {
                return fail();
            }
            rc_array_bytes_push(&out, v, arena);
        }
        if (out.num >= len) {
            break;
        }

        // Match block: table-coded count, then count matches - length
        // first (it selects the offset context, including length 1), then
        // the offset, both validated.
        value_result match_count = read_value(&r, &tables.cnt[v7_match]);
        if (!match_count.ok || r.fault != bit_fault_ok) {
            return fail();
        }
        count = match_count.value;
        for (uint32_t k = 0; k < count; k++) {
            value_result length = read_value(&r, &tables.base.len);
            if (!length.ok || r.fault != bit_fault_ok) {
                return fail();
            }
            value_result offset = read_value(&r, len1_off_table(&tables.base, length.value));
            if (!offset.ok || r.fault != bit_fault_ok) {
                return fail();
            }
            if (offset.value > out.num || out.num + length.value > len) {
                return fail();
            }
            // Forward byte-by-byte copy handles overlap; a length-1 match
            // is simply one iteration.
            uint32_t src = out.num - offset.value;
            for (uint32_t c = 0; c < length.value; c++) {
                rc_array_bytes_push(&out, rc_array_bytes_get(&out, src + c), arena);
            }
        }
    }
    return (v7_decompress_result) {.data = out, .ok = true};
}

// ------------------------------------------------------------------
//  tests
// ------------------------------------------------------------------

#ifdef ENABLE_TESTS

#include <string.h>

#include "richc/test.h"

#include "v3.h"
#include "v6.h"

RC_TEST_GROUP_DATA(v7) {
    rc_arena a;
    rc_arena scratch;
};

RC_TEST_GROUP_INIT(v7, fix)
{
    fix->a = rc_arena_make(1u << 30);
    fix->scratch = rc_arena_make(1u << 30);
}

RC_TEST_GROUP_DEINIT(v7, fix)
{
    rc_arena_deinit(&fix->scratch);
    rc_arena_deinit(&fix->a);
}

static uint32_t roundtrip(rc_view_bytes in, struct rc_test_group_data_v7 *fix)
{
    v7_compress_result c = v7_compress(in, &fix->a, fix->scratch);
    RC_CHECK_TRUE(c.ok);
    RC_CHECK(c.data.num, >=, 2u);
    RC_CHECK(rc_array_bytes_get(&c.data, 0), ==, (uint8_t)in.num);
    RC_CHECK(rc_array_bytes_get(&c.data, 1), ==, (uint8_t)(in.num >> 8));

    v7_decompress_result d = v7_decompress(c.data.view, &fix->a);
    RC_CHECK_TRUE(d.ok);
    RC_CHECK(d.data.num, ==, in.num);
    if (in.num > 0) {
        RC_CHECK_TRUE(memcmp(d.data.data, in.data, in.num) == 0);
    }
    return c.data.num;
}

// Round trip an input that might legitimately be unencodable: assert only
// that the verdict is definite and an ok stream decodes byte-identically.
static void roundtrip_if_encodable(rc_view_bytes in, struct rc_test_group_data_v7 *fix)
{
    v7_compress_result c = v7_compress(in, &fix->a, fix->scratch);
    if (!c.ok) {
        return;
    }
    v7_decompress_result d = v7_decompress(c.data.view, &fix->a);
    RC_CHECK_TRUE(d.ok);
    RC_CHECK(d.data.num, ==, in.num);
    RC_CHECK_TRUE(memcmp(d.data.data, in.data, in.num) == 0);
}

RC_TEST_STEP(v7, roundtrip_edges, fix)
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

RC_TEST_STEP(v7, roundtrip_text, fix)
{
    // No dominance claim against v6: v7 pays two count-table headers v6
    // does not, so it can legitimately lose on extreme synthetic data.
    static uint8_t buf[8192];
    const char *phrase = "all work and no play makes jack a dull boy. ";
    uint32_t plen = (uint32_t)strlen(phrase);
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)phrase[i % plen];
    }
    rc_view_bytes in = (rc_view_bytes) RC_VIEW(buf);
    uint32_t packed = roundtrip(in, fix);
    RC_CHECK(packed, <, (uint32_t)(sizeof buf) / 10);
}

RC_TEST_STEP(v7, len1_rescues_v3_unencodable, fix)
{
    // 0,1,0,2,0,3,...: no 2-byte pair repeats, so v3 sees one literal run
    // over the 256 block cap and fails.  v7's length-1 matches (byte 0
    // recurs at distance 2) break the run, so this must encode - and win
    // against v3's chassis by a wide margin.
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
    v3_compress_result v3 = v3_compress(in, &fix->a, fix->scratch);
    RC_CHECK_FALSE(v3.ok);
    roundtrip(in, fix);
}

RC_TEST_STEP(v7, roundtrip_far_offsets, fix)
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

RC_TEST_STEP(v7, random_and_mixed_never_trap, fix)
{
    static uint8_t rnd[4096];
    uint32_t seed = 0xDECAFBAD;
    for (uint32_t i = 0; i < sizeof rnd; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        rnd[i] = (uint8_t)(seed >> 24);
    }
    roundtrip_if_encodable((rc_view_bytes) RC_VIEW(rnd), fix);

    static uint8_t mixed[v7_max_uncompressed];
    seed = 0x5EED;
    for (uint32_t i = 0; i < sizeof mixed; i++) {
        if ((i >> 10) & 1) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            mixed[i] = (uint8_t)(seed >> 16);
        }
        else {
            mixed[i] = (uint8_t)(i >> 6);
        }
    }
    roundtrip_if_encodable((rc_view_bytes) RC_VIEW(mixed), fix);
}

RC_TEST_STEP(v7, far_escape_valves_encode, fix)
{
    // 0..255 then 0,2,4,...: no 2-byte pair ever repeats, and every
    // repeated byte is over 60 away - beyond the tuned off1 seed - so
    // encoding the >256 literal run needs the full-coverage retry.  v3
    // (no length-1 matches) cannot represent this input at all.
    static uint8_t buf[301];
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (i < 256) ? (uint8_t)i : (uint8_t)(2 * (i - 256));
    }
    v3_compress_result v3 = v3_compress((rc_view_bytes) RC_VIEW(buf), &fix->a, fix->scratch);
    RC_CHECK_FALSE(v3.ok);
    roundtrip((rc_view_bytes) RC_VIEW(buf), fix);
}

RC_TEST_STEP(v7, decompress_rejects_prefixes, fix)
{
    static uint8_t buf[600];
    const char *phrase = "abracadabra alakazam ";
    uint32_t plen = (uint32_t)strlen(phrase);
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)phrase[i % plen];
    }
    v7_compress_result c = v7_compress((rc_view_bytes) RC_VIEW(buf), &fix->a, fix->scratch);
    RC_CHECK_TRUE(c.ok);
    RC_CHECK_TRUE(v7_decompress(c.data.view, &fix->a).ok);

    for (uint32_t n = 0; n < c.data.num; n++) {
        uint32_t mark = fix->a.top;
        rc_view_bytes prefix = {.data = c.data.view.data, .num = n};
        RC_CHECK_FALSE(v7_decompress(prefix, &fix->a).ok);
        rc_arena_free_to(&fix->a, mark);
    }
}

RC_TEST_STEP(v7, decompress_garbage_never_traps, fix)
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
        v7_decompress_result d = v7_decompress((rc_view_bytes) {.data = buf, .num = n},
                                               &fix->a);
        if (d.ok && n >= 2) {
            uint32_t declared = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
            RC_CHECK(d.data.num, ==, declared);
        }
        rc_arena_free_to(&fix->a, mark);
    }
}

// ---- parse exactness for fixed tables ----

// Enumerate every token sequence over the breakpoint lists plus the
// length-1 edges (tiny inputs only) and track the minimal exact stream
// bits under fixed tables.
static void brute_parses(rc_view_bytes in, const matches *m,
                         const v7_tables *tables, rc_array_token *seq,
                         uint32_t pos, uint32_t *best, rc_arena *arena)
{
    if (pos == in.num) {
        uint32_t bits = data_bits(seq->view, tables);
        if (bits < *best) {
            *best = bits;
        }
        return;
    }
    // Literal step.
    rc_array_token_push(seq, (token) {
        .length = 1,
        .offset = 0,
    }, arena);
    brute_parses(in, m, tables, seq, pos + 1, best, arena);
    rc_array_token_pop(seq);

    // Length-1 step where a previous occurrence exists and is coded.
    uint32_t d = rc_array_u32_get(&m->near1, pos);
    if (d != 0
        && table_cost(&tables->base.len, 1) < table_big_cost
        && table_cost(&tables->base.off[0], d) < table_big_cost) {
        rc_array_token_push(seq, (token) {
            .length = 1,
            .offset = (uint16_t)d,
        }, arena);
        brute_parses(in, m, tables, seq, pos + 1, best, arena);
        rc_array_token_pop(seq);
    }

    // Every truncation of every encodable breakpoint.
    uint32_t lo = min_match - 1;
    uint32_t end = rc_array_u32_get(&m->start, pos + 1);
    for (uint32_t k = rc_array_u32_get(&m->start, pos); k < end; k++) {
        token t = rc_array_token_get(&m->bp, k);
        for (uint32_t len = lo + 1; len <= t.length; len++) {
            if (table_cost(&tables->base.len, len) >= table_big_cost
                || table_cost(&tables->base.off[off_ctx(len)], t.offset) >= table_big_cost) {
                continue;
            }
            rc_array_token_push(seq, (token) {
                .length = (uint16_t)len,
                .offset = t.offset,
            }, arena);
            brute_parses(in, m, tables, seq, pos + len, best, arena);
            rc_array_token_pop(seq);
        }
        lo = t.length;
    }
}

RC_TEST_STEP(v7, parse_is_exact_for_fixed_tables, fix)
{
    // Exactness holds under the seed: its count costs equal Elias gamma,
    // monotone in run length, so frontier dominance is sound.
    v7_tables tables = v7_seed_tables();
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
        uint32_t want = UINT32_MAX;
        rc_array_token seq = {0};
        rc_array_token_reserve(&seq, n + 1, &fix->scratch);
        brute_parses(in, &m, &tables, &seq, 0, &want, &fix->scratch);

        v7_parse_result got = v7_parse(in, &m, &tables, false, &fix->scratch);
        RC_CHECK(got.bits, ==, want);
        rc_arena_free_to(&fix->scratch, mark);
    }
}

#endif // ENABLE_TESTS
