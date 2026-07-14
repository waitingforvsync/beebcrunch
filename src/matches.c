/*
 *  matches.c - shared breakpoint match scan
 */

#include "matches.h"

#include "richc/arena.h"
#include "richc/macros.h"

// Hash chains keyed on the exact 2 bytes at each position, built forward
// in a single pass over the input.
static uint32_t match_key(rc_view_bytes in, uint32_t pos)
{
    return (uint32_t)rc_view_bytes_get(in, pos)
         | ((uint32_t)rc_view_bytes_get(in, pos + 1) << 8);
}

matches scan_matches(rc_view_bytes in, rc_arena *arena)
{
    uint32_t n = in.num;
    matches m = {0};

    // Fixed-size arrays first so the growing breakpoint pool stays the
    // arena's latest allocation and can grow in place.
    rc_array_u32_resize(&m.head, 1u << 16, arena);
    for (uint32_t k = 0; k < (1u << 16); k++) {
        rc_array_u32_set(&m.head, k, RC_INDEX_NONE);
    }
    rc_array_u32_resize(&m.prev, n ? n : 1, arena);
    rc_array_u32_resize(&m.start, n + 1, arena);
    rc_array_u32_resize(&m.near1, n ? n : 1, arena);
    rc_array_token_reserve(&m.bp, n ? n : 1, arena);

    // The separate length-1 chain: nearest previous occurrence of each
    // single byte, for "repeat the byte from d ago" candidates.
    uint32_t last_seen[256];
    for (uint32_t b = 0; b < 256; b++) {
        last_seen[b] = RC_INDEX_NONE;
    }

    for (uint32_t pos = 0; pos < n; pos++) {
        rc_array_u32_set(&m.start, pos, m.bp.num);

        uint8_t byte = rc_view_bytes_get(in, pos);
        uint32_t seen = last_seen[byte];
        rc_array_u32_set(&m.near1, pos, seen == RC_INDEX_NONE ? 0 : pos - seen);
        last_seen[byte] = pos;

        if (pos + 1 < n) {
            uint32_t key = match_key(in, pos);
            // Never extend past the input or the format's length cap.
            uint32_t limit = n - pos;
            if (limit > max_match) {
                limit = max_match;
            }
            uint32_t best = min_match - 1;
            uint32_t p = rc_array_u32_get(&m.head, key);
            while (p != RC_INDEX_NONE) {
                // Once the cap is reached no further breakpoint can exist.
                if (best >= limit) {
                    break;
                }
                // A candidate that cannot exceed the current best cannot add
                // a breakpoint; reject it on one byte before extending.  This
                // keeps run-heavy data (long chains of the same key) cheap.
                if (rc_view_bytes_get(in, p + best) != rc_view_bytes_get(in, pos + best)) {
                    p = rc_array_u32_get(&m.prev, p);
                    continue;
                }
                // The chain key matches both bytes exactly, so extend from 2.
                uint32_t len = min_match;
                while (len < limit
                       && rc_view_bytes_get(in, p + len) == rc_view_bytes_get(in, pos + len)) {
                    len++;
                }
                // Chains run nearest-first, so the first candidate to reach a
                // new length is automatically the nearest offset for it.
                if (len > best) {
                    rc_array_token_push(&m.bp, (token) {
                        .length = (uint16_t)len,
                        .offset = (uint16_t)(pos - p),
                    }, arena);
                    best = len;
                }
                p = rc_array_u32_get(&m.prev, p);
            }
            // Insert after searching so a position never matches itself.
            rc_array_u32_set(&m.prev, pos, rc_array_u32_get(&m.head, key));
            rc_array_u32_set(&m.head, key, pos);
        }
    }
    rc_array_u32_set(&m.start, n, m.bp.num);
    return m;
}

// ------------------------------------------------------------------
//  tests
// ------------------------------------------------------------------

#ifdef ENABLE_TESTS

#include "richc/arena.h"
#include "richc/test.h"

RC_TEST(matches, token_tagging)
{
    token literal = {
        .length = 1,
        .offset = 0,
    };
    token repeat = {
        .length = 1,
        .offset = 7,
    };
    token match = {
        .length = 5,
        .offset = 7,
    };
    RC_CHECK_TRUE(token_is_literal(literal));
    RC_CHECK_FALSE(token_is_literal(repeat));
    RC_CHECK_FALSE(token_is_literal(match));
}

RC_TEST_GROUP_DATA(matches) {
    rc_arena a;
};

RC_TEST_GROUP_INIT(matches, fix)
{
    fix->a = rc_arena_make_default();
}

RC_TEST_GROUP_DEINIT(matches, fix)
{
    rc_arena_deinit(&fix->a);
}

RC_TEST_STEP(matches, near1_distances, fix)
{
    // a b a a c b: nearest same-byte distances 0,0,2,1,0,4
    uint8_t data[6] = {'a', 'b', 'a', 'a', 'c', 'b'};
    matches m = scan_matches((rc_view_bytes) RC_VIEW(data), &fix->a);
    uint32_t expect[6] = {0, 0, 2, 1, 0, 4};
    for (uint32_t i = 0; i < 6; i++) {
        RC_CHECK(rc_array_u32_get(&m.near1, i), ==, expect[i]);
    }
}

#endif // ENABLE_TESTS
