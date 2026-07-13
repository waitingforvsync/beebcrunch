/*
 *  bitwriter.c - bit stream writer with a byte-aligned quick path
 */

#include "bitwriter.h"

#include "richc/arena.h"
#include "richc/macros.h"

#include "bitutils.h"

bitwriter bitwriter_make(rc_array_bytes *out, rc_arena *arena)
{
    RC_ASSERT(out);
    RC_ASSERT(arena);
    return (bitwriter) {
        .out = out,
        .arena = arena,
        .cache_index = RC_INDEX_NONE,
        .bit = 0,
    };
}

static void put_bit(bitwriter *w, uint32_t bit)
{
    // Reserve the cache byte in the output at the moment its first bit is
    // written, so aligned bytes written later land after it in the stream.
    if (w->cache_index == RC_INDEX_NONE || w->bit == 8) {
        w->cache_index = rc_array_bytes_push(w->out, 0, w->arena);
        w->bit = 0;
    }
    // Bits fill the cache byte from bit 0 upward, matching the 6502's LSR
    // idiom (each shift right drops the next stream bit into carry).  The
    // byte was pushed as zero, so only set bits need touching; a partial
    // cache byte is therefore already zero-padded and needs no flush.
    if (bit) {
        uint8_t b = rc_array_bytes_get(w->out, w->cache_index);
        b |= (uint8_t)(1u << w->bit);
        rc_array_bytes_set(w->out, w->cache_index, b);
    }
    w->bit++;
}

void bitwriter_bits(bitwriter *w, uint32_t val, uint32_t n)
{
    RC_ASSERT(w);
    RC_ASSERT(n <= 8);
    for (uint32_t k = 0; k < n; k++) {
        put_bit(w, (val >> (n - 1 - k)) & 1);
    }
}

void bitwriter_gamma(bitwriter *w, uint32_t v)
{
    // 256 is the largest value any codec may write: a 6502 decoder keeps
    // gamma values in one byte (256 wraps to 0, which no field uses).
    RC_ASSERT(v >= 1 && v <= 256);
    // ilog2(v) leading zeros, a 1 terminator, then the payload bits below
    // the top set bit.  Emitted in three <= 8-bit pieces: gamma(256) has a
    // 9-bit tail, which would not fit one bits() call.
    uint32_t nb = ilog2_u32(v);
    bitwriter_bits(w, 0, nb);
    bitwriter_bits(w, 1, 1);
    bitwriter_bits(w, v, nb);   // bits() takes the low nb bits: the payload
}

void bitwriter_byte(bitwriter *w, uint8_t v)
{
    RC_ASSERT(w);
    rc_array_bytes_push(w->out, v, w->arena);
}

// ------------------------------------------------------------------
//  tests
// ------------------------------------------------------------------

#ifdef ENABLE_TESTS

#include "richc/test.h"

RC_TEST_GROUP_DATA(bitwriter) {
    rc_arena a;
};

RC_TEST_GROUP_INIT(bitwriter, fix)
{
    fix->a = rc_arena_make_default();
}

RC_TEST_GROUP_DEINIT(bitwriter, fix)
{
    rc_arena_deinit(&fix->a);
}

// Pin the exact packing: stream bits fill each byte from bit 0 upward
// (the 6502 decoder's LSR idiom depends on it), while values arrive most
// significant bit first, so a value's bits appear reversed in the byte.
RC_TEST_STEP(bitwriter, bit0_first_packing, fix)
{
    rc_array_bytes out = {0};
    bitwriter w = bitwriter_make(&out, &fix->a);
    bitwriter_bits(&w, 1, 1);           // stream bits so far: 1
    bitwriter_bits(&w, 0, 1);           // 1,0
    bitwriter_bits(&w, 0x5, 3);         // 1,0 then 1,0,1
    bitwriter_bits(&w, 0xF, 4);         // ... then 1,1,1,1 -> 9th bit pending
    // First byte holds the first 8 stream bits at positions 0..7:
    // 1,0,1,0,1,1,1,1 -> 0b11110101; the 9th bit lands at bit 0 of byte 2,
    // which is zero-padded above it.
    RC_CHECK(out.num, ==, 2u);
    RC_CHECK(rc_array_bytes_get(&out, 0), ==, 0xF5);
    RC_CHECK(rc_array_bytes_get(&out, 1), ==, 0x01);
}

// The aligned byte lands after the cache byte holding the earlier bits,
// and later bits continue in that same cache byte.
RC_TEST_STEP(bitwriter, aligned_byte_interleave, fix)
{
    rc_array_bytes out = {0};
    bitwriter w = bitwriter_make(&out, &fix->a);
    bitwriter_bits(&w, 1, 1);           // cache byte reserved at [0], bit 0
    bitwriter_byte(&w, 0x42);           // appended at [1]
    bitwriter_bits(&w, 1, 1);           // continues in [0] at bit 1
    bitwriter_byte(&w, 0x99);           // appended at [2]
    bitwriter_bits(&w, 0x3F, 6);        // fills [0] bits 2..7: 11111111
    bitwriter_bits(&w, 1, 1);           // new cache byte at [3], bit 0
    RC_CHECK(out.num, ==, 4u);
    RC_CHECK(rc_array_bytes_get(&out, 0), ==, 0xFF);
    RC_CHECK(rc_array_bytes_get(&out, 1), ==, 0x42);
    RC_CHECK(rc_array_bytes_get(&out, 2), ==, 0x99);
    RC_CHECK(rc_array_bytes_get(&out, 3), ==, 0x01);
}

RC_TEST_STEP(bitwriter, byte_only_stream_is_verbatim, fix)
{
    rc_array_bytes out = {0};
    bitwriter w = bitwriter_make(&out, &fix->a);
    for (uint32_t k = 0; k < 8; k++) {
        bitwriter_byte(&w, (uint8_t)(k * 17));
    }
    RC_CHECK(out.num, ==, 8u);
    for (uint32_t k = 0; k < 8; k++) {
        RC_CHECK(rc_array_bytes_get(&out, k), ==, (uint8_t)(k * 17));
    }
}

// Encoded gamma length must equal gamma_bits for every value the cost
// model will ever price.
RC_TEST_STEP(bitwriter, gamma_length_matches_cost, fix)
{
    for (uint32_t v = 1; v <= 256; v++) {
        rc_array_bytes out = {0};
        uint32_t mark = fix->a.top;
        bitwriter w = bitwriter_make(&out, &fix->a);
        bitwriter_gamma(&w, v);
        uint32_t bits = (out.num - 1) * 8 + w.bit;
        RC_CHECK(bits, ==, gamma_bits(v));
        rc_arena_free_to(&fix->a, mark);
    }
}

#endif // ENABLE_TESTS
