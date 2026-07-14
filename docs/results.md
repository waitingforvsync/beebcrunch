# Compression results

Corpus: the eleven BBC Micro binaries in `corpus/` (provenance in
`corpus/README.md`), 166810 bytes in total.  Ratio = packed size / original
size; lower is better.

## Summary

One row per codec, one detailed-notes section each below.  Deltas are
measured against exomizer raw, the reference target.

| codec | total packed | ratio | vs exomizer raw |
|---|--:|--:|--:|
| exomizer raw | 96198 | 57.7% | - |
| exomizer raw -c | 96434 | 57.8% | +0.2% |
| v0 | 100588 | 60.3% | +4.6% |
| v1 | 99576 | 59.7% | +3.5% |
| v2 | 98543 | 59.1% | +2.4% |
| v3 | 97701 | 58.6% | +1.6% |
| v4 | 97829 | 58.6% | +1.7% |

## Per-file results

Packed sizes; new codecs add a column.  exo -c is the comparison column
(exomizer without literal sequences, the closest analogue of a small-
decoder format).

| file | original | exo -c | v0 | v1 | v2 | v3 | v4 |
|---|--:|--:|--:|--:|--:|--:|--:|
| exile-title.bin | 8320 | 4341 | 4575 | 4567 | 4465 | 4453 | 4445 |
| droid-title.bin | 20480 | 6972 | 7615 | 7607 | 7172 | 7160 | 7096 |
| ravenskull-title.bin | 20480 | 12877 | 13333 | 13109 | 13074 | 12843 | 13035 |
| repton3-title.bin | 10240 | 4914 | 5212 | 5182 | 5036 | 5013 | 4982 |
| boomscreen.bin | 16000 | 4427 | 4725 | 4575 | 4444 | 4321 | 4450 |
| blurpscreen.bin | 8320 | 2982 | 3184 | 3142 | 3057 | 3014 | 3049 |
| exileb.bin | 24704 | 20744 | 21474 | 21278 | 21204 | 21089 | 21049 |
| chuckie.bin | 9984 | 6499 | 6712 | 6694 | 6674 | 6664 | 6582 |
| frak2.bin | 13567 | 8929 | 9225 | 9067 | 9149 | 9008 | 9027 |
| blurp.bin | 18331 | 11505 | 11903 | 11735 | 11760 | 11625 | 11634 |
| basic2.rom | 16384 | 12244 | 12630 | 12620 | 12508 | 12511 | 12480 |
| **TOTAL** | **166810** | **96434** | **100588** | **99576** | **98543** | **97701** | **97829** |

## Exomizer 3.0.2 (raw and raw -c)

The reference target.  Built from bitbucket tag 3.0.2; run as
`exomizer raw -q -o <out> <in>` (forward, default settings, encoding tables
embedded in the output - the bytes a standalone 6502 decruncher would need).
`raw -c` disables literal sequences (long literal runs copied verbatim
without per-byte flag bits), allowing a smaller and simpler decruncher.
raw totals 96198 (57.7%), -c totals 96434 (57.8%).  Measured 2026-07-13.

Notes:

- Literal sequences buy exomizer only 236 bytes overall, and only five
  files change: ravenskull-title +111, blurp +64, exileb +36, basic2 +14,
  frak2 +11.  Literal runs long enough to benefit (~30+ bytes) are rare in
  this corpus, so a format without them concedes little.
- exileb.bin is the stress case: Exile's code/data is so dense that even
  exomizer only removes 16%.  Per-literal overhead dominates there.

## v0

The simplest beebcrunch format (src/v0.h): u16 length header, 3 bits
holding B-1 (B in 1..8), then flag-bit tokens - 0 = byte-aligned literal,
1 = match: gamma offset high part, B raw low offset bits, gamma(length-1).
6502 byte-width friendly throughout: match lengths and every Elias gamma
value cap at 256 (wrapping to 0 in a byte, which is invalid), so an offset
is encodable under B only when ((offset-1) >> B) + 1 <= 256.  Optimal
forward parse over nearest-offset-per-length match candidates, jointly
optimized with B.  Measured 2026-07-13: 100588 (60.3%).

Notes:

- The optimal parse replaced the original greedy parse (105584 = 63.3%), a
  ~5000-byte improvement for encoder-side work only.
- The 6502 byte-width constraints cost just 30 bytes across the corpus.
- Chosen B spreads over 6..8.

## v1

v0's match representation inside block framing (src/v1.h): instead of a
flag bit per token, alternating blocks of literals and matches - always
starting with literals - each prefixed with Elias gamma(count), count in
[1, 256].  Contiguous same-type runs pay one gamma header instead of one
bit per token.  A run over 256 tokens is not yet representable: the parse
avoids it where any alternative exists (a deliberately wasteful match can
act as an escape valve) and v1_compress reports an error where it cannot.
The optimal forward parse keeps a small Pareto frontier of (run length,
cost-excluding-pending-header) states per position and token type, since
the block header makes token cost depend on run length; exact under the
format's constraints, jointly optimized with B.  Measured 2026-07-13:
99576 (59.7%).

Notes:

- All eleven corpus files are representable: forced matches always provide
  an escape valve in practice; the >256-run representation question is
  still open for pathological inputs (no repeated 2-byte pair in a 257+
  stretch).
- Beats v0 on every file, -1012 bytes overall: the flag-bit consolidation
  wins roughly a bit per literal-run token minus the gamma headers.
- Chosen B spreads over 6..8, one lower than v0 on several files: block
  framing changes the literal/match trade-off, and the joint optimization
  follows it.

## v2

v0's framing (flag bit per token, byte-aligned literals) with exomizer's
key idea: per-file learned interval tables for match offsets and lengths
(src/v2.h).  A table is a 5-bit bucket count plus a 4-bit width per
bucket (~2-17 bytes each); bucket starts accumulate from the minimum
value, so only the distribution's shape travels.  Offset bucket indices
are flat 4-bit (up to 16 buckets, indices 0..15), length bucket indices
Elias gamma (up to 31 buckets - the gamma index has no structural pin) -
the measured asymmetry.  Offsets are coded through one of three tables
conditioned on the match length (len 2 / len 3 / len >= 4), so matches
are length-then-offset in the stream.  B is gone: the learned buckets
subsume it.  Encoder iterates exact-parse <-> optimal-tables (partition
DP over the parse's histograms) to a fixpoint, keeping the best true size
including table headers.  Measured 2026-07-14: 98543 (59.1%).

Notes:

- -1033 vs v1, +2.4% vs exomizer raw.  The tables recover more than the
  per-token flag bit costs on most files.
- The length-conditioned offset contexts gained 136 bytes overall but
  cost a little on exile-title and frak2: two extra table headers (~18
  bytes) are not always recovered when the per-class offset distributions
  are similar.
- The offset index width is a measured optimum, not a guess: 3-bit/8
  buckets costs +493 bytes, 5-bit/31 buckets +31, 15 buckets under the
  4-bit index +398.  Every match pays the index, and 16 geometric buckets
  cover the 64K offset range.  Length buckets raised to 31: -21 bytes.
- v1 still beats v2 on frak2 (9067 vs 9138) and blurp (11735 vs 11796):
  match-dense files feel the flag-bit tax most, exactly where block
  framing shines.  The planned length-conditioned offset contexts attack
  the same files from the table side.
- The parse is exact for any fixed tables (test-enforced); the fixpoint
  over tables is a heuristic, as in exomizer.

## v3

v2's learned tables carried by v1's block framing (src/v3.h): alternating
literal/match blocks with Elias gamma counts in [1, 256] replace the
per-token flag bit, and match lengths/offsets go through the shared
per-file tables (three offset contexts by length class, length-then-offset
per match).  The parse combines v1's Pareto frontier (block headers make
token cost run-dependent) with v2's parse <-> tables fixpoint; runs over
256 tokens remain unrepresentable (parse avoids them; ok == false when
impossible, as v1).  Measured 2026-07-14: 97701 (58.6%).

Notes:

- -842 vs v2; +1.6% vs exomizer raw, +1.3% vs raw -c.  Beats v2 on ten of
  eleven files (basic2.rom +3).  All corpus files representable.
- The combination stacks cleanly: blocks remove the flag-bit tax the
  tables could not touch, tables shrink the match fields the blocks could
  not touch.
- Remaining known structural gap to exomizer: length-1 matches (measured
  worth 2605 bytes to exomizer on this corpus) - addressed by v4.

## v4

v2's shape plus length-1 matches (src/v4.h): the length table's minimum
becomes 1, and a fourth, deliberately tiny offset table (flat 2-bit
index, at most 4 buckets) serves "repeat the byte from d ago" matches -
profitable only at small offsets, which the table learns.  Candidates
come from a separate length-1 chain in the shared match scan: the
nearest previous occurrence of each byte (near1).  Decode is v2's loop
with one more context row.  The shared token type dropped its union for
this: {length, offset}, literal iff length == 1 && offset == 0, with
literal bytes read from the input at encode time (v0-v3 byte-identical
after the change).  Measured 2026-07-14: 97829 (58.6%).

Notes:

- Length-1 matches earn 714 bytes over v2 on the same chassis - real,
  but far less than the 2605 they are worth to exomizer, whose economy
  differs (its length index for 1 is a single gamma bit against our
  learned bucket, and its parse can use them more aggressively).
- v4 does NOT beat v3 overall (97829 vs 97701), but the per-file split
  is complementary: v4 wins the dense executables (chuckie -82,
  droid -64, exileb -40 vs v3), v3 wins the long-run graphics
  (boomscreen, blurpscreen, ravenskull) where block framing removes the
  flag-bit tax.  Blocks + tables + length-1 in one codec is the obvious
  next combination.
