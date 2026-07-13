/*
 *  bitreader.h - bit stream reader with a byte-aligned quick path
 *
 *  Mirrors bitwriter: multi-bit values read most significant bit first,
 *  stream bits unpacked from each byte starting at bit 0 (the 6502's LSR
 *  order).  Bits come from a cache byte refilled from the next
 *  stream position when empty; bitreader_byte reads the next stream byte
 *  directly, bypassing the cache.  The compressed stream is untrusted input:
 *  reading past the end yields zero bits/bytes and latches a fault, and an
 *  Elias gamma code outside [1, 256] (nothing this project encodes exceeds
 *  a 6502 byte, with 256 wrapping to 0) is malformed.  The first fault
 *  sticks; callers check the fault field at their convenience.
 */

#ifndef BITREADER_H_
#define BITREADER_H_

#include "richc/bytes.h"

typedef enum bit_fault {
    bit_fault_ok = 0,
    bit_fault_truncated,        // read past the end of the stream
    bit_fault_malformed,        // Elias gamma value out of range
} bit_fault;

typedef struct bitreader {
    rc_view_bytes in;
    uint32_t      next_index;   // next unconsumed stream byte
    uint8_t       cached_byte;  // current bit-cache byte, bit 0 next out
    uint8_t       bits;         // bits remaining in the cached byte
    uint8_t       fault;        // bit_fault
} bitreader;

bitreader bitreader_make(rc_view_bytes in);

// Read n bits, most significant first, n <= 8
uint32_t bitreader_bits(bitreader *r, uint32_t n);

// Read an Elias gamma value; in [1, 256] when fault == bit_fault_ok
uint32_t bitreader_gamma(bitreader *r);

// Byte-aligned quick path: read the next whole stream byte
uint8_t bitreader_byte(bitreader *r);

#endif // BITREADER_H_
