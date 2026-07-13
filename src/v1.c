/*
 *  v1.c - the v1 codec: block-framed tokens, Pareto-frontier optimal parse
 */

#include "v1.h"

#include "richc/arena.h"
#include "richc/macros.h"

#include "bitreader.h"
#include "bitutils.h"
#include "bitwriter.h"
#include "matches.h"

enum {
    v1_max_run = 256,   // Elias gamma cap on the block count
    v1_lit     = 0,
    v1_match   = 1,
};

// ---- optimal forward parse ----
//
// The block header gamma(count) makes a token's cost depend on the length
// of the same-type run it sits in, so a single cost per position is not
// enough state.  Each (position, arriving token type) instead keeps a
// Pareto frontier of states (run, cost), where run is the current run
// length and cost excludes the current run's still-pending gamma header;
// a state's realized bits are cost + gamma_bits(run).  A state survives
// only if no other state at the same position and type has both run <=
// and cost <= : a smaller run is never worse, because the pending header
// is monotone in run length and its future step-ups come no sooner.
// Frontiers stay small because gamma_bits(run) takes only 9 distinct
// values for run <= 256.
//
// Back-links are safe against dominance pruning: every edge into position
// i comes from an earlier position, so a frontier is complete before it
// is processed, and only states alive at processing time can acquire
// children.

typedef struct v1_node {
    uint32_t next;          // next state in this frontier's list
    uint32_t from_index;    // predecessor position (RC_INDEX_NONE at seed)
    uint32_t cost;          // bits excluding the pending run header
    uint16_t run;           // current run length, 1..256
    uint16_t from_run;      // predecessor state's run length
    uint8_t  from_type;     // predecessor state's token type (v1_lit/v1_match)
    token    tok;           // token taken to reach this state
} v1_node;

#define RC_ARRAY_TYPE v1_node
#define RC_ARRAY_NAME node
#include "richc/template/array.h"

// Insert state (run, cost) into the frontier at heads[pos] unless it is
// dominated; removes any states it dominates.  Lists sorted by run.
static void frontier_insert(rc_array_node *pool, rc_array_u32 *heads, uint32_t pos,
                            uint32_t run, uint32_t cost,
                            uint32_t from_index, uint32_t from_type, uint32_t from_run,
                            token tok, rc_arena *arena)
{
    uint32_t head = rc_array_u32_get(heads, pos);

    // Dominated by an existing state: nothing to do.
    for (uint32_t c = head; c != RC_INDEX_NONE; ) {
        const v1_node *node = rc_array_node_at(pool, c);
        if (node->run <= run && node->cost <= cost) {
            return;
        }
        c = node->next;
    }

    // Unlink every state the new one dominates.
    uint32_t prev = RC_INDEX_NONE;
    for (uint32_t c = head; c != RC_INDEX_NONE; ) {
        v1_node *node = rc_array_node_at(pool, c);
        uint32_t next = node->next;
        if (node->run >= run && node->cost >= cost) {
            if (prev == RC_INDEX_NONE) {
                head = next;
            }
            else {
                rc_array_node_at(pool, prev)->next = next;
            }
        }
        else {
            prev = c;
        }
        c = next;
    }

    // Link the new state in ascending run order.
    uint32_t ni = rc_array_node_push(pool, (v1_node) {
        .next = RC_INDEX_NONE,
        .from_index = from_index,
        .cost = cost,
        .run = (uint16_t)run,
        .from_run = (uint16_t)from_run,
        .from_type = (uint8_t)from_type,
        .tok = tok,
    }, arena);
    prev = RC_INDEX_NONE;
    uint32_t c = head;
    while (c != RC_INDEX_NONE && rc_array_node_at(pool, c)->run < run) {
        prev = c;
        c = rc_array_node_at(pool, c)->next;
    }
    rc_array_node_at(pool, ni)->next = c;
    if (prev == RC_INDEX_NONE) {
        head = ni;
    }
    else {
        rc_array_node_at(pool, prev)->next = ni;
    }
    rc_array_u32_set(heads, pos, head);
}

static uint32_t frontier_find(rc_view_node pool, uint32_t head, uint32_t run)
{
    for (uint32_t c = head; c != RC_INDEX_NONE; ) {
        const v1_node *node = rc_view_node_at(pool, c);
        if (node->run == run) {
            return c;
        }
        c = node->next;
    }
    return RC_INDEX_NONE;
}

typedef struct v1_parse_result {
    rc_array_token tokens;  // filled only when want_tokens
    uint32_t       bits;    // stream bits after the B field, or UINT32_MAX
                            // when a run over v1_max_run was unavoidable
} v1_parse_result;

// All working memory - and the tokens - come from arena; the caller
// brackets each pass with an arena mark.
static v1_parse_result v1_parse(rc_view_bytes in, const matches *m, uint32_t b,
                                bool want_tokens, rc_arena *arena)
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

    // The stream always opens with a literal block: seed the state after
    // the first literal.
    frontier_insert(&pool, &heads[v1_lit], 1, 1, 8, RC_INDEX_NONE, v1_lit, 0, (token) {
        .length = 1,
        .literal = rc_view_bytes_get(in, 0),
    }, arena);

    for (uint32_t i = 1; i < n; i++) {
        for (uint32_t type = 0; type < 2; type++) {
            uint32_t idx = rc_array_u32_get(&heads[type], i);
            while (idx != RC_INDEX_NONE) {
                // Copy the node: the pool may grow (and move nothing, but
                // stay disciplined) while this state fans out.
                v1_node s = rc_array_node_get(&pool, idx);
                uint32_t actual = s.cost + gamma_bits(s.run);

                // Literal edge: extend the literal run (if the block cap
                // allows) or realize the match run's header and open a
                // fresh literal block.
                token lit = {
                    .length = 1,
                    .literal = rc_view_bytes_get(in, i),
                };
                if (type == v1_lit) {
                    if (s.run < v1_max_run) {
                        frontier_insert(&pool, &heads[v1_lit], i + 1, s.run + 1,
                                        s.cost + 8, i, v1_lit, s.run, lit, arena);
                    }
                }
                else {
                    frontier_insert(&pool, &heads[v1_lit], i + 1, 1,
                                    actual + 8, i, v1_match, s.run, lit, arena);
                }

                // Match edges: every truncation of every encodable
                // breakpoint, same run-or-new-run split as literals.
                uint32_t lo = min_match - 1;
                uint32_t end = rc_array_u32_get(&m->start, i + 1);
                for (uint32_t k = rc_array_u32_get(&m->start, i); k < end; k++) {
                    token t = rc_array_token_get(&m->bp, k);
                    // The gamma cap bounds the offset high part; offsets only
                    // grow down the list, so stop at the first violation.
                    uint32_t high = ((uint32_t)(t.offset - 1) >> b) + 1;
                    if (high > 256) {
                        break;
                    }
                    uint32_t base = b + gamma_bits(high);
                    for (uint32_t len = lo + 1; len <= t.length; len++) {
                        uint32_t payload = base + gamma_bits(len - 1);
                        token mt = {
                            .length = (uint16_t)len,
                            .offset = t.offset,
                        };
                        if (type == v1_match) {
                            if (s.run < v1_max_run) {
                                frontier_insert(&pool, &heads[v1_match], i + len,
                                                s.run + 1, s.cost + payload,
                                                i, v1_match, s.run, mt, arena);
                            }
                        }
                        else {
                            frontier_insert(&pool, &heads[v1_match], i + len,
                                            1, actual + payload,
                                            i, v1_lit, s.run, mt, arena);
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
            v1_node s = rc_array_node_get(&pool, idx);
            uint32_t bits = s.cost + gamma_bits(s.run);
            if (bits < best_bits) {
                best_bits = bits;
                best_idx = idx;
            }
            idx = s.next;
        }
    }
    v1_parse_result res = {
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
            v1_node s = rc_array_node_get(&pool, idx);
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

    // Emit maximal same-type runs: gamma(count) then the tokens.  The
    // parse guarantees the block cap and the leading literal block.
    uint32_t total = 0;
    uint32_t i = 0;
    while (i < tokens.num) {
        bool is_lit = rc_view_token_get(tokens, i).length == 1;
        RC_ASSERT(i > 0 || is_lit);
        uint32_t j = i;
        while (j < tokens.num && (rc_view_token_get(tokens, j).length == 1) == is_lit) {
            j++;
        }
        RC_ASSERT(j - i <= v1_max_run);
        bitwriter_gamma(&w, j - i);
        for (uint32_t k = i; k < j; k++) {
            token t = rc_view_token_get(tokens, k);
            if (is_lit) {
                bitwriter_byte(&w, t.literal);
            }
            else {
                bitwriter_gamma(&w, ((uint32_t)(t.offset - 1) >> b) + 1);
                bitwriter_bits(&w, (uint32_t)(t.offset - 1) & ((1u << b) - 1), b);
                bitwriter_gamma(&w, (uint32_t)t.length - 1);
            }
            total += t.length;
        }
        i = j;
    }
    RC_ASSERT(total == in.num);
    return out;
}

v1_compress_result v1_compress(rc_view_bytes in, rc_arena *arena, rc_arena scratch)
{
    RC_ASSERT(arena);
    RC_ASSERT(in.num <= v1_max_uncompressed);
    RC_ASSERT(arena->base != scratch.base);

    // Empty input: header plus the B field, no blocks at all.
    if (in.num == 0) {
        return (v1_compress_result) {
            .data = encode(in, (rc_view_token) {0}, 1, arena),
            .b = 1,
            .num_tokens = 0,
            .ok = true,
        };
    }

    // Find and cache all match candidates once; the parse passes below
    // only read them.
    matches m = scan_matches(in, &scratch);

    // Jointly optimize (parse, B), bracketing each pass with an arena mark
    // so the frontier pools are reclaimed.
    uint32_t best_b = 1;
    uint32_t best_bits = UINT32_MAX;
    for (uint32_t b = 1; b <= 8; b++) {
        uint32_t mark = scratch.top;
        uint32_t bits = v1_parse(in, &m, b, false, &scratch).bits;
        rc_arena_free_to(&scratch, mark);
        if (bits < best_bits) {
            best_bits = bits;
            best_b = b;
        }
    }
    if (best_bits == UINT32_MAX) {
        // Some run over v1_max_run tokens had no alternative under any B.
        return (v1_compress_result) {.ok = false};
    }

    // Rerun the winner to reconstruct its token sequence.
    v1_parse_result parsed = v1_parse(in, &m, best_b, true, &scratch);
    RC_ASSERT(parsed.bits == best_bits);

    return (v1_compress_result) {
        .data = encode(in, parsed.tokens.view, best_b, arena),
        .b = best_b,
        .num_tokens = parsed.tokens.num,
        .ok = true,
    };
}

static v1_decompress_result fail(void) { return (v1_decompress_result) {.data = {0}, .ok = false}; }

v1_decompress_result v1_decompress(rc_view_bytes comp, rc_arena *arena)
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
        // Literal block: gamma count, then that many aligned bytes, each
        // bounded by the declared length - the stream is untrusted.
        uint32_t count = bitreader_gamma(&r);
        if (r.fault != bit_fault_ok) {
            return fail();
        }
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

        // Match block: gamma count, then count matches in the v0 token
        // representation, with the same offset/length validation.
        count = bitreader_gamma(&r);
        if (r.fault != bit_fault_ok) {
            return fail();
        }
        for (uint32_t k = 0; k < count; k++) {
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
            for (uint32_t c = 0; c < length; c++) {
                rc_array_bytes_push(&out, rc_array_bytes_get(&out, src + c), arena);
            }
        }
    }
    return (v1_decompress_result) {.data = out, .ok = true};
}

// ------------------------------------------------------------------
//  tests
// ------------------------------------------------------------------

#ifdef ENABLE_TESTS

#include <string.h>

#include "richc/test.h"

#include "v0.h"

RC_TEST_GROUP_DATA(v1) {
    rc_arena a;
    rc_arena scratch;
};

RC_TEST_GROUP_INIT(v1, fix)
{
    // The frontier pool can outgrow the default reserve on large inputs;
    // reservation is virtual, pages commit on demand.
    fix->a = rc_arena_make(1u << 30);
    fix->scratch = rc_arena_make(1u << 30);
}

RC_TEST_GROUP_DEINIT(v1, fix)
{
    rc_arena_deinit(&fix->scratch);
    rc_arena_deinit(&fix->a);
}

static uint32_t roundtrip(rc_view_bytes in, struct rc_test_group_data_v1 *fix)
{
    v1_compress_result c = v1_compress(in, &fix->a, fix->scratch);
    RC_CHECK_TRUE(c.ok);
    RC_CHECK(c.b, >=, 1u);
    RC_CHECK(c.b, <=, 8u);
    RC_CHECK(c.data.num, >=, 2u);
    RC_CHECK(rc_array_bytes_get(&c.data, 0), ==, (uint8_t)in.num);
    RC_CHECK(rc_array_bytes_get(&c.data, 1), ==, (uint8_t)(in.num >> 8));

    v1_decompress_result d = v1_decompress(c.data.view, &fix->a);
    RC_CHECK_TRUE(d.ok);
    RC_CHECK(d.data.num, ==, in.num);
    if (in.num > 0) {
        RC_CHECK_TRUE(memcmp(d.data.data, in.data, in.num) == 0);
    }
    return c.data.num;
}

// Round trip an input that might legitimately be unencodable: assert only
// that the verdict is definite and an ok stream decodes byte-identically.
static void roundtrip_if_encodable(rc_view_bytes in, struct rc_test_group_data_v1 *fix)
{
    v1_compress_result c = v1_compress(in, &fix->a, fix->scratch);
    if (!c.ok) {
        return;
    }
    v1_decompress_result d = v1_decompress(c.data.view, &fix->a);
    RC_CHECK_TRUE(d.ok);
    RC_CHECK(d.data.num, ==, in.num);
    RC_CHECK_TRUE(memcmp(d.data.data, in.data, in.num) == 0);
}

RC_TEST_STEP(v1, roundtrip_edges, fix)
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

RC_TEST_STEP(v1, roundtrip_text_beats_v0, fix)
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

    // Block framing must not lose to per-token flags on repetitive text.
    v0_compress_result v0 = v0_compress(in, &fix->a, fix->scratch);
    RC_CHECK(packed, <=, v0.data.num);
}

RC_TEST_STEP(v1, roundtrip_far_offsets, fix)
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

RC_TEST_STEP(v1, random_and_mixed_never_trap, fix)
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

    static uint8_t mixed[v1_max_uncompressed];
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

RC_TEST_STEP(v1, unencodable_returns_error, fix)
{
    // 0,1,0,2,0,3,...: every adjacent 2-byte pair is unique, so there are
    // no matches at all and the whole input is one literal run - longer
    // than the 256 block cap, hence unrepresentable.
    uint8_t buf[301];
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (i & 1) ? (uint8_t)(i / 2 + 1) : 0;
    }
    v1_compress_result c = v1_compress((rc_view_bytes) RC_VIEW(buf), &fix->a, fix->scratch);
    RC_CHECK_FALSE(c.ok);

    // The same content one byte shorter than the cap is fine.
    v1_compress_result c2 = v1_compress((rc_view_bytes) {.data = buf, .num = 256},
                                        &fix->a, fix->scratch);
    RC_CHECK_TRUE(c2.ok);
}

RC_TEST_STEP(v1, decompress_rejects_prefixes, fix)
{
    static uint8_t buf[600];
    const char *phrase = "abracadabra alakazam ";
    uint32_t plen = (uint32_t)strlen(phrase);
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)phrase[i % plen];
    }
    v1_compress_result c = v1_compress((rc_view_bytes) RC_VIEW(buf), &fix->a, fix->scratch);
    RC_CHECK_TRUE(c.ok);
    RC_CHECK_TRUE(v1_decompress(c.data.view, &fix->a).ok);

    for (uint32_t n = 0; n < c.data.num; n++) {
        uint32_t mark = fix->a.top;
        rc_view_bytes prefix = {.data = c.data.view.data, .num = n};
        RC_CHECK_FALSE(v1_decompress(prefix, &fix->a).ok);
        rc_arena_free_to(&fix->a, mark);
    }
}

RC_TEST_STEP(v1, decompress_garbage_never_traps, fix)
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
        v1_decompress_result d = v1_decompress((rc_view_bytes) {.data = buf, .num = n},
                                               &fix->a);
        if (d.ok && n >= 2) {
            uint32_t declared = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
            RC_CHECK(d.data.num, ==, declared);
        }
        rc_arena_free_to(&fix->a, mark);
    }
}

// ---- optimality ----

// Exact v1 stream bits (excluding the 3-bit B field) for a token
// sequence: gamma block headers over maximal same-type runs + payloads.
static uint32_t sequence_bits(rc_view_token toks, uint32_t b)
{
    uint32_t bits = 0;
    uint32_t i = 0;
    while (i < toks.num) {
        bool is_lit = rc_view_token_get(toks, i).length == 1;
        uint32_t j = i;
        while (j < toks.num && (rc_view_token_get(toks, j).length == 1) == is_lit) {
            j++;
        }
        bits += gamma_bits(j - i);
        for (uint32_t k = i; k < j; k++) {
            token t = rc_view_token_get(toks, k);
            if (is_lit) {
                bits += 8;
            }
            else {
                bits += gamma_bits(((uint32_t)(t.offset - 1) >> b) + 1) + b
                      + gamma_bits((uint32_t)t.length - 1);
            }
        }
        i = j;
    }
    return bits;
}

// Enumerate every token sequence over the breakpoint lists (tiny inputs
// only) and track the minimal exact stream bits.
static void brute_parses(rc_view_bytes in, const matches *m, uint32_t b,
                         rc_array_token *seq, uint32_t pos, uint32_t *best,
                         rc_arena *arena)
{
    if (pos == in.num) {
        uint32_t bits = sequence_bits(seq->view, b);
        if (bits < *best) {
            *best = bits;
        }
        return;
    }
    // Literal step.
    rc_array_token_push(seq, (token) {
        .length = 1,
        .literal = rc_view_bytes_get(in, pos),
    }, arena);
    brute_parses(in, m, b, seq, pos + 1, best, arena);
    rc_array_token_pop(seq);

    // Every truncation of every encodable breakpoint.
    uint32_t lo = min_match - 1;
    uint32_t end = rc_array_u32_get(&m->start, pos + 1);
    for (uint32_t k = rc_array_u32_get(&m->start, pos); k < end; k++) {
        token t = rc_array_token_get(&m->bp, k);
        uint32_t high = ((uint32_t)(t.offset - 1) >> b) + 1;
        if (high > 256) {
            break;
        }
        for (uint32_t len = lo + 1; len <= t.length; len++) {
            rc_array_token_push(seq, (token) {
                .length = (uint16_t)len,
                .offset = t.offset,
            }, arena);
            brute_parses(in, m, b, seq, pos + len, best, arena);
            rc_array_token_pop(seq);
        }
        lo = t.length;
    }
}

RC_TEST_STEP(v1, parse_is_optimal_for_tiny_inputs, fix)
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
        // check the frontier DP's byte count matches exactly.
        uint32_t mark = fix->scratch.top;
        matches m = scan_matches(in, &fix->scratch);
        uint32_t best_bits = UINT32_MAX;
        for (uint32_t b = 1; b <= 8; b++) {
            rc_array_token seq = {0};
            rc_array_token_reserve(&seq, n + 1, &fix->scratch);
            brute_parses(in, &m, b, &seq, 0, &best_bits, &fix->scratch);
        }
        rc_arena_free_to(&fix->scratch, mark);

        v1_compress_result c = v1_compress(in, &fix->a, fix->scratch);
        RC_CHECK_TRUE(c.ok);
        RC_CHECK(c.data.num, ==, 2 + (3 + best_bits + 7) / 8);
    }
}

#endif // ENABLE_TESTS
