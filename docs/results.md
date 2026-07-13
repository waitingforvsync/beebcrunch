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

## Per-file results

Packed sizes; new codecs add a column.  exo -c is the comparison column
(exomizer without literal sequences, the closest analogue of a small-
decoder format).

| file | original | exo -c | v0 | v1 |
|---|--:|--:|--:|--:|
| exile-title.bin | 8320 | 4341 | 4575 | 4567 |
| droid-title.bin | 20480 | 6972 | 7615 | 7607 |
| ravenskull-title.bin | 20480 | 12877 | 13333 | 13109 |
| repton3-title.bin | 10240 | 4914 | 5212 | 5182 |
| boomscreen.bin | 16000 | 4427 | 4725 | 4575 |
| blurpscreen.bin | 8320 | 2982 | 3184 | 3142 |
| exileb.bin | 24704 | 20744 | 21474 | 21278 |
| chuckie.bin | 9984 | 6499 | 6712 | 6694 |
| frak2.bin | 13567 | 8929 | 9225 | 9067 |
| blurp.bin | 18331 | 11505 | 11903 | 11735 |
| basic2.rom | 16384 | 12244 | 12630 | 12620 |
| **TOTAL** | **166810** | **96434** | **100588** | **99576** |

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
