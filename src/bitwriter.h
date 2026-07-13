/*
 *  bitwriter.h - bit stream writer with a byte-aligned quick path
 *
 *  Multi-bit values are written most significant bit first, and stream
 *  bits pack into each byte from bit 0 upward (so a value's bits appear
 *  reversed within the byte) - the order the 6502's LSR consumes.
 *
 *  Bits stream through a cache byte that is reserved in the output at the
 *  position where its first bit is written and filled in place; whole bytes
 *  (bitwriter_byte) bypass the cache and are appended directly.  Because a
 *  reader consumes the same sequence of bit/byte operations, the two
 *  interleave deterministically, and on the 6502 the aligned path becomes a
 *  plain LDA (src),Y instead of eight shifts.  A partly-filled cache byte
 *  is already zero-padded in place, so there is no flush step.
 */

#ifndef BITWRITER_H_
#define BITWRITER_H_

#include "richc/bytes.h"

typedef struct rc_arena rc_arena;

typedef struct bitwriter {
    rc_array_bytes *out;
    rc_arena       *arena;
    uint32_t        cache_index;    // RC_INDEX_NONE when no partial byte
    uint32_t        bit;            // next bit position in the cache byte, 0..8
} bitwriter;

bitwriter bitwriter_make(rc_array_bytes *out, rc_arena *arena);

// Write the low n bits of val, most significant first, n <= 8
void bitwriter_bits(bitwriter *w, uint32_t val, uint32_t n);

// Write the Elias gamma code for v, v in [1, 256]
void bitwriter_gamma(bitwriter *w, uint32_t v);

// Byte-aligned quick path: append a whole byte, bypassing the bit cache
void bitwriter_byte(bitwriter *w, uint8_t v);

#endif // BITWRITER_H_
