/*
 *  bitutils.h - shared bit-twiddling helpers for the codecs
 */

#ifndef BITUTILS_H_
#define BITUTILS_H_

#include <stdint.h>

#include "richc/ops.h"

// Floor log2, x >= 1
static inline uint32_t ilog2_u32(uint32_t x) { return 31 - rc_clz_u32(x); }

// Encoded size in bits of EliasGamma(x), x >= 1: ilog2(x) zeros then x in
// ilog2(x)+1 bits
static inline uint32_t gamma_bits(uint32_t x) { return 2 * ilog2_u32(x) + 1; }

#endif // BITUTILS_H_
