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
    rc_array_token_reserve(&m.bp, n ? n : 1, arena);

    for (uint32_t pos = 0; pos < n; pos++) {
        rc_array_u32_set(&m.start, pos, m.bp.num);
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
