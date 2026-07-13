/*
 *  bitreader.c - bit stream reader with a byte-aligned quick path
 */

#include "bitreader.h"

#include "richc/macros.h"

bitreader bitreader_make(rc_view_bytes in)
{
    return (bitreader) {
        .in = in,
        .next_index = 0,
        .cached_byte = 0,
        .bits = 0,
        .fault = bit_fault_ok,
    };
}

// First fault sticks so the earliest cause survives later knock-on reads
static void latch(bitreader *r, bit_fault fault)
{
    if (r->fault == bit_fault_ok) {
        r->fault = fault;
    }
}

static uint8_t next_byte(bitreader *r)
{
    // Past the end: keep returning zeros so callers can defer their fault
    // check to a convenient boundary instead of testing every read.
    if (r->next_index >= r->in.num) {
        latch(r, bit_fault_truncated);
        return 0;
    }
    return rc_view_bytes_get(r->in, r->next_index++);
}

static uint32_t get_bit(bitreader *r)
{
    // Refill the cache from the stream exactly when its 8 bits are spent;
    // this mirrors the writer's lazy cache-byte reservation, keeping the
    // two sides' bit/byte interleave aligned.
    if (r->bits == 0) {
        r->cached_byte = next_byte(r);
        r->bits = 8;
    }
    // Bit 0 comes out first - the 6502's LSR drops it straight into carry.
    uint32_t bit = r->cached_byte & 1;
    r->cached_byte >>= 1;
    r->bits--;
    return bit;
}

uint32_t bitreader_bits(bitreader *r, uint32_t n)
{
    RC_ASSERT(r);
    RC_ASSERT(n <= 8);
    uint32_t v = 0;
    for (uint32_t k = 0; k < n; k++) {
        v = (v << 1) | get_bit(r);
    }
    return v;
}

uint32_t bitreader_gamma(bitreader *r)
{
    RC_ASSERT(r);
    // Count leading zeros up to the terminating 1, then read that many
    // payload bits below it.  Valid values are 1..256 (a 6502 byte with
    // 256 wrapping to 0), so more than 8 zeros - or a payload that pushes
    // the value past 256 - can only be corrupt input.
    uint32_t zeros = 0;
    while (get_bit(r) == 0) {
        if (++zeros > 8) {
            latch(r, bit_fault_malformed);
            return 1;
        }
    }
    uint32_t v = (1u << zeros) | bitreader_bits(r, zeros);
    if (v > 256) {
        latch(r, bit_fault_malformed);
        return 1;
    }
    return v;
}

uint8_t bitreader_byte(bitreader *r)
{
    RC_ASSERT(r);
    return next_byte(r);
}

// ------------------------------------------------------------------
//  tests
// ------------------------------------------------------------------

#ifdef ENABLE_TESTS

#include "richc/arena.h"
#include "richc/test.h"

#include "bitwriter.h"

RC_TEST_GROUP_DATA(bitreader) {
    rc_arena a;
};

RC_TEST_GROUP_INIT(bitreader, fix)
{
    fix->a = rc_arena_make_default();
}

RC_TEST_GROUP_DEINIT(bitreader, fix)
{
    rc_arena_deinit(&fix->a);
}

RC_TEST_STEP(bitreader, bits_roundtrip_all_widths, fix)
{
    rc_array_bytes out = {0};
    bitwriter w = bitwriter_make(&out, &fix->a);
    for (uint32_t phase = 0; phase < 8; phase++) {
        bitwriter_bits(&w, 1, phase);
        for (uint32_t n = 0; n <= 8; n++) {
            bitwriter_bits(&w, 0xA5, n);
        }
    }

    bitreader r = bitreader_make(out.view);
    for (uint32_t phase = 0; phase < 8; phase++) {
        RC_CHECK(bitreader_bits(&r, phase), ==, (phase ? 1u : 0u));
        for (uint32_t n = 0; n <= 8; n++) {
            uint32_t expect = 0xA5u & ((n == 8) ? 0xFF : ((1u << n) - 1));
            RC_CHECK(bitreader_bits(&r, n), ==, expect);
        }
    }
    RC_CHECK(r.fault, ==, bit_fault_ok);
}

// Every legal Elias gamma value, exhaustively, including the 256 cap.
RC_TEST_STEP(bitreader, gamma_roundtrip, fix)
{
    rc_array_bytes out = {0};
    bitwriter w = bitwriter_make(&out, &fix->a);
    for (uint32_t v = 1; v <= 256; v++) {
        bitwriter_gamma(&w, v);
    }

    bitreader r = bitreader_make(out.view);
    for (uint32_t v = 1; v <= 256; v++) {
        RC_CHECK(bitreader_gamma(&r), ==, v);
    }
    RC_CHECK(r.fault, ==, bit_fault_ok);
}

// Bits and aligned bytes interleave identically on both sides.
RC_TEST_STEP(bitreader, aligned_byte_interleave_roundtrip, fix)
{
    rc_array_bytes out = {0};
    bitwriter w = bitwriter_make(&out, &fix->a);
    uint32_t seed = 0x1234ABCD;
    for (uint32_t k = 0; k < 500; k++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        if (seed & 1) {
            bitwriter_byte(&w, (uint8_t)(seed >> 8));
        }
        else if (seed & 2) {
            bitwriter_gamma(&w, 1 + ((seed >> 8) & 0xFF));
        }
        else {
            bitwriter_bits(&w, seed >> 16, (seed >> 4) & 7);
        }
    }

    bitreader r = bitreader_make(out.view);
    seed = 0x1234ABCD;
    for (uint32_t k = 0; k < 500; k++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        if (seed & 1) {
            RC_CHECK(bitreader_byte(&r), ==, (uint8_t)(seed >> 8));
        }
        else if (seed & 2) {
            RC_CHECK(bitreader_gamma(&r), ==, 1 + ((seed >> 8) & 0xFF));
        }
        else {
            uint32_t n = (seed >> 4) & 7;
            uint32_t expect = (seed >> 16) & ((n == 0) ? 0 : ((1u << n) - 1));
            RC_CHECK(bitreader_bits(&r, n), ==, expect);
        }
    }
    RC_CHECK(r.fault, ==, bit_fault_ok);
}

RC_TEST(bitreader, truncation_latches_and_yields_zeros)
{
    uint8_t data[1] = {0xFF};
    bitreader r = bitreader_make((rc_view_bytes) RC_VIEW(data));
    RC_CHECK(bitreader_bits(&r, 8), ==, 0xFFu);
    RC_CHECK(r.fault, ==, bit_fault_ok);
    RC_CHECK(bitreader_bits(&r, 4), ==, 0u);
    RC_CHECK(r.fault, ==, bit_fault_truncated);
    RC_CHECK(bitreader_byte(&r), ==, 0u);
    RC_CHECK(r.fault, ==, bit_fault_truncated);   // first fault sticks
}

RC_TEST(bitreader, empty_view)
{
    bitreader r = bitreader_make((rc_view_bytes) {0});
    RC_CHECK(bitreader_bits(&r, 1), ==, 0u);
    RC_CHECK(r.fault, ==, bit_fault_truncated);
}

RC_TEST(bitreader, gamma_zero_run_capped)
{
    // 4 zero bytes: an in-bounds Elias gamma this long is malformed
    // (values cap at 256) and must not read unbounded bits.
    uint8_t data[4] = {0, 0, 0, 0};
    bitreader r = bitreader_make((rc_view_bytes) RC_VIEW(data));
    (void)bitreader_gamma(&r);
    RC_CHECK(r.fault, ==, bit_fault_malformed);
}

RC_TEST_STEP(bitreader, gamma_over_256_rejected, fix)
{
    // Hand-build the code for 257 (8 zeros, terminator, payload 1): a
    // 9-bit-class value the format never produces.
    rc_array_bytes out = {0};
    bitwriter w = bitwriter_make(&out, &fix->a);
    bitwriter_bits(&w, 0, 8);
    bitwriter_bits(&w, 1, 1);
    bitwriter_bits(&w, 1, 8);
    bitreader r = bitreader_make(out.view);
    (void)bitreader_gamma(&r);
    RC_CHECK(r.fault, ==, bit_fault_malformed);
}

RC_TEST(bitreader, gamma_truncated_reports_truncation)
{
    uint8_t data[1] = {0};
    bitreader r = bitreader_make((rc_view_bytes) RC_VIEW(data));
    (void)bitreader_gamma(&r);
    RC_CHECK(r.fault, ==, bit_fault_truncated);
}

#endif // ENABLE_TESTS
