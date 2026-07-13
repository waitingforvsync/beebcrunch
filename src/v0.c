/*
 *  v0.c - the v0 codec: optimal forward parse, Elias gamma + fixed-B offsets
 */

#include "v0.h"

#include "richc/arena.h"
#include "richc/macros.h"

#include "bitreader.h"
#include "bitutils.h"
#include "bitwriter.h"
#include "matches.h"

// ---- optimal forward parse ----
//
// Single-source shortest path over positions: cost[i] is the exact minimal
// token-stream bits to encode the first i bytes.  Every edge goes forward,
// so cost[i] is final when position i is processed.  All truncations of
// every encodable breakpoint are relaxed, so the parse is exact for the
// format given the breakpoint lists.

// Token-stream bits for split b; fills arrival (the token that optimally
// reaches each position) when arrival.num != 0
static uint32_t dp_pass(rc_view_bytes in, const matches *m, uint32_t b,
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
        // cost[i] is final here: the literal edge from i-1 guarantees every
        // position is reachable by the time it is processed.
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

        // Match edges: every truncation of every breakpoint.  The offset
        // part of the cost is fixed per breakpoint, so hoist it.
        uint32_t lo = min_match - 1;
        uint32_t end = rc_array_u32_get(&m->start, i + 1);
        for (uint32_t k = rc_array_u32_get(&m->start, i); k < end; k++) {
            token t = rc_array_token_get(&m->bp, k);
            // The gamma cap bounds the offset high part at 256; offsets
            // only grow down the list, so nothing later is encodable
            // under this B either.
            uint32_t high = ((uint32_t)(t.offset - 1) >> b) + 1;
            if (high > 256) {
                break;
            }
            uint32_t base = here + 1 + b + gamma_bits(high);
            for (uint32_t len = lo + 1; len <= t.length; len++) {
                uint32_t c = base + gamma_bits(len - 1);
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

// ---- encoding ----

static rc_array_bytes encode(rc_view_bytes in, rc_view_token tokens, uint32_t b,
                             rc_arena *arena)
{
    rc_array_bytes out = {0};
    rc_array_bytes_reserve(&out, 64, arena);

    // Raw two-byte length header, then everything else through the writer.
    rc_array_bytes_push(&out, (uint8_t)(in.num), arena);
    rc_array_bytes_push(&out, (uint8_t)(in.num >> 8), arena);

    bitwriter w = bitwriter_make(&out, arena);
    bitwriter_bits(&w, b - 1, 3);

    uint32_t total = 0;
    for (uint32_t k = 0; k < tokens.num; k++) {
        token t = rc_view_token_get(tokens, k);
        if (t.length == 1) {
            bitwriter_bits(&w, 0, 1);
            bitwriter_byte(&w, t.literal);
        }
        else {
            // Match: offset first (gamma-coded high part, then the B raw
            // low bits so the 6502 can shift them straight into a 16-bit
            // value), then the length as gamma.
            bitwriter_bits(&w, 1, 1);
            bitwriter_gamma(&w, ((uint32_t)(t.offset - 1) >> b) + 1);
            bitwriter_bits(&w, (uint32_t)(t.offset - 1) & ((1u << b) - 1), b);
            bitwriter_gamma(&w, (uint32_t)t.length - 1);
        }
        total += t.length;
    }
    RC_ASSERT(total == in.num);
    return out;
}

v0_compress_result v0_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    RC_ASSERT(arena);
    RC_ASSERT(in.num <= v0_max_uncompressed);
    RC_ASSERT(arena->base != scratch.base);

    // Find and cache all match candidates once; the DP passes below only
    // read them.
    matches m = scan_matches(in, &scratch);

    // The best B cannot be known without parsing, and the best parse
    // depends on B - so run the (cheap, cached-match) DP for every B and
    // keep the jointly optimal pair.
    uint32_t best_b = 1;
    uint32_t best_bits = UINT32_MAX;
    for (uint32_t b = 1; b <= 8; b++) {
        uint32_t bits = dp_pass(in, &m, b, (rc_span_token) {0}, scratch);
        if (bits < best_bits) {
            best_bits = bits;
            best_b = b;
        }
    }

    // One more pass for the winner records the arrival tokens...
    rc_array_token arrival = {0};
    rc_array_token_resize(&arrival, in.num + 1, &scratch);
    dp_pass(in, &m, best_b, arrival.span, scratch);

    // ...and walking arrivals back from the end recovers the token
    // sequence, reversed into stream order.
    rc_array_token tokens = {0};
    rc_array_token_reserve(&tokens, in.num ? in.num : 1, &scratch);
    uint32_t j = in.num;
    while (j > 0) {
        token t = rc_array_token_get(&arrival, j);
        rc_array_token_push(&tokens, t, &scratch);
        j -= t.length;
    }
    rc_span_token_reverse(tokens.span);

    return (v0_compress_result) {
        .data = encode(in, tokens.view, best_b, arena),
        .b = best_b,
        .num_tokens = tokens.num,
    };
}

static v0_decompress_result fail(void) { return (v0_decompress_result) {.data = {0}, .ok = false}; }

v0_decompress_result v0_decompress(rc_view_bytes comp, rc_arena *arena)
{
    RC_ASSERT(arena);

    // Length header first: it bounds everything that follows.
    if (comp.num < 2) {
        return fail();
    }
    uint32_t len = (uint32_t)rc_view_bytes_get(comp, 0)
                 | ((uint32_t)rc_view_bytes_get(comp, 1) << 8);

    bitreader r = bitreader_make(rc_view_bytes_get_tail(comp, 2));
    uint32_t b = bitreader_bits(&r, 3) + 1;
    if (r.fault != bit_fault_ok) {
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
            // Match: offset first - gamma high part, then B raw low bits
            // shifted in below it - then the length.  Validate both fields
            // against what has actually been produced so far - the stream
            // is untrusted.
            uint32_t high = bitreader_gamma(&r);
            uint32_t low = bitreader_bits(&r, b);
            uint32_t length = bitreader_gamma(&r) + 1;
            if (r.fault != bit_fault_ok) {
                return fail();
            }
            uint32_t offset = (((high - 1) << b) | low) + 1;
            if (offset > out.num || out.num + length > len) {
                return fail();
            }
            // Forward byte-by-byte copy handles overlap (offset < length).
            uint32_t src = out.num - offset;
            for (uint32_t k = 0; k < length; k++) {
                rc_array_bytes_push(&out, rc_array_bytes_get(&out, src + k), arena);
            }
        }
    }
    return (v0_decompress_result) {.data = out, .ok = true};
}

// ------------------------------------------------------------------
//  tests
// ------------------------------------------------------------------

#ifdef ENABLE_TESTS

#include <string.h>

#include "richc/test.h"

RC_TEST_GROUP_DATA(v0) {
    rc_arena a;
    rc_arena scratch;
};

RC_TEST_GROUP_INIT(v0, fix)
{
    fix->a = rc_arena_make_default();
    fix->scratch = rc_arena_make_default();
}

RC_TEST_GROUP_DEINIT(v0, fix)
{
    rc_arena_deinit(&fix->scratch);
    rc_arena_deinit(&fix->a);
}

static uint32_t roundtrip(rc_view_bytes in, struct rc_test_group_data_v0 *fix)
{
    v0_compress_result c = v0_compress(in, &fix->a, fix->scratch);
    RC_CHECK(c.b, >=, 1u);
    RC_CHECK(c.b, <=, 8u);
    RC_CHECK(c.data.num, >=, 2u);
    RC_CHECK(rc_array_bytes_get(&c.data, 0), ==, (uint8_t)in.num);
    RC_CHECK(rc_array_bytes_get(&c.data, 1), ==, (uint8_t)(in.num >> 8));

    v0_decompress_result d = v0_decompress(c.data.view, &fix->a);
    RC_CHECK_TRUE(d.ok);
    RC_CHECK(d.data.num, ==, in.num);
    if (in.num > 0) {
        RC_CHECK_TRUE(memcmp(d.data.data, in.data, in.num) == 0);
    }
    return c.data.num;
}

RC_TEST_STEP(v0, roundtrip_edges, fix)
{
    roundtrip((rc_view_bytes) {0}, fix);

    uint8_t one[1] = {0x42};
    roundtrip((rc_view_bytes) RC_VIEW(one), fix);

    uint8_t two[2] = {7, 7};
    roundtrip((rc_view_bytes) RC_VIEW(two), fix);

    static uint8_t same[4096];
    memset(same, 0xEE, sizeof same);
    roundtrip((rc_view_bytes) {.data = same, .num = 256}, fix);
    uint32_t packed = roundtrip((rc_view_bytes) RC_VIEW(same), fix);
    RC_CHECK(packed, <, 96u);   // collapses to a literal + a few max matches

    static uint8_t alt[512];
    for (uint32_t i = 0; i < sizeof alt; i++) {
        alt[i] = (i & 1) ? 0xAA : 0x55;
    }
    roundtrip((rc_view_bytes) RC_VIEW(alt), fix);
}

RC_TEST_STEP(v0, roundtrip_random_incompressible, fix)
{
    static uint8_t buf[4096];
    uint32_t seed = 0xDECAFBAD;
    for (uint32_t i = 0; i < sizeof buf; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        buf[i] = (uint8_t)(seed >> 24);
    }
    // Correctness only: random data may expand slightly (1 flag bit/byte).
    roundtrip((rc_view_bytes) RC_VIEW(buf), fix);
}

RC_TEST_STEP(v0, roundtrip_text_compresses, fix)
{
    static uint8_t buf[8192];
    const char *phrase = "all work and no play makes jack a dull boy. ";
    uint32_t plen = (uint32_t)strlen(phrase);
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)phrase[i % plen];
    }
    uint32_t packed = roundtrip((rc_view_bytes) RC_VIEW(buf), fix);
    RC_CHECK(packed, <, (uint32_t)(sizeof buf) / 10);
}

RC_TEST_STEP(v0, roundtrip_far_offsets, fix)
{
    // Repeating structure at large, varied distances so the offset high
    // gamma, the encodability limit, and several B splits get exercised.
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

RC_TEST_STEP(v0, roundtrip_max_size, fix)
{
    static uint8_t buf[v0_max_uncompressed];
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

RC_TEST_STEP(v0, decompress_rejects_prefixes, fix)
{
    static uint8_t buf[600];
    const char *phrase = "abracadabra alakazam ";
    uint32_t plen = (uint32_t)strlen(phrase);
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)phrase[i % plen];
    }
    v0_compress_result c = v0_compress((rc_view_bytes) RC_VIEW(buf), &fix->a, fix->scratch);
    RC_CHECK_TRUE(v0_decompress(c.data.view, &fix->a).ok);

    // Every strict prefix loses bits the final tokens need, so it must be
    // rejected - the fault checks make silent zero-fill impossible.
    for (uint32_t n = 0; n < c.data.num; n++) {
        uint32_t mark = fix->a.top;
        rc_view_bytes prefix = {.data = c.data.view.data, .num = n};
        RC_CHECK_FALSE(v0_decompress(prefix, &fix->a).ok);
        rc_arena_free_to(&fix->a, mark);
    }
}

RC_TEST_STEP(v0, decompress_garbage_never_traps, fix)
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
        v0_decompress_result d = v0_decompress((rc_view_bytes) {.data = buf, .num = n},
                                               &fix->a);
        // Garbage may legitimately decode (tiny declared lengths); the
        // contract is termination with a definite verdict, no traps.
        if (d.ok && n >= 2) {
            uint32_t declared = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
            RC_CHECK(d.data.num, ==, declared);
        }
        rc_arena_free_to(&fix->a, mark);
    }
}

// ---- optimality ----

// Minimal token-stream bits for the suffix from pos, brute-forced over the
// same breakpoint lists the DP uses.  Tiny inputs only (exponential).
static uint32_t brute_suffix_bits(rc_view_bytes in, const matches *m,
                                  uint32_t pos, uint32_t b)
{
    uint32_t n = in.num;
    if (pos == n) {
        return 0;
    }
    uint32_t best = 9 + brute_suffix_bits(in, m, pos + 1, b);
    uint32_t lo = min_match - 1;
    uint32_t end = rc_array_u32_get(&m->start, pos + 1);
    for (uint32_t k = rc_array_u32_get(&m->start, pos); k < end; k++) {
        token t = rc_array_token_get(&m->bp, k);
        uint32_t high = ((uint32_t)(t.offset - 1) >> b) + 1;
        if (high > 256) {
            break;
        }
        uint32_t base = 1 + b + gamma_bits(high);
        for (uint32_t len = lo + 1; len <= t.length; len++) {
            uint32_t c = base + gamma_bits(len - 1)
                       + brute_suffix_bits(in, m, pos + len, b);
            if (c < best) {
                best = c;
            }
        }
        lo = t.length;
    }
    return best;
}

RC_TEST_STEP(v0, parse_is_optimal_for_tiny_inputs, fix)
{
    uint32_t seed = 0xFEED;
    for (uint32_t trial = 0; trial < 80; trial++) {
        uint8_t data[12];
        uint32_t n = 4 + trial % 9;
        for (uint32_t i = 0; i < n; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            data[i] = (uint8_t)('a' + seed % 3);
        }
        rc_view_bytes in = {.data = data, .num = n};

        // Brute-force the true minimum over every parse and every B, then
        // check the DP's byte count matches exactly.
        uint32_t mark = fix->scratch.top;
        matches m = scan_matches(in, &fix->scratch);
        uint32_t best_bits = UINT32_MAX;
        for (uint32_t b = 1; b <= 8; b++) {
            uint32_t bits = brute_suffix_bits(in, &m, 0, b);
            if (bits < best_bits) {
                best_bits = bits;
            }
        }
        rc_arena_free_to(&fix->scratch, mark);

        v0_compress_result c = v0_compress(in, &fix->a, fix->scratch);
        RC_CHECK(c.data.num, ==, 2 + (3 + best_bits + 7) / 8);
    }
}

#endif // ENABLE_TESTS
