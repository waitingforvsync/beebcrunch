# Compression results

Corpus: the eleven BBC Micro binaries in `corpus/` (provenance in
`corpus/README.md`), 166810 bytes in total.  Ratio = packed size / original
size; lower is better.

## Summary

One row per codec; each has a detailed section below.  Deltas are measured
against exomizer raw, the reference target.

| codec | total packed | ratio | vs exomizer raw |
|---|--:|--:|--:|
| exomizer raw | 96198 | 57.7% | - |
| exomizer raw -c | 96434 | 57.8% | +0.2% |
| v0 | 100588 | 60.3% | +4.6% |

## Exomizer 3.0.2 (raw and raw -c)

The reference target.  Built from bitbucket tag 3.0.2; run as
`exomizer raw -q -o <out> <in>` (forward, default settings, encoding tables
embedded in the output - the bytes a standalone 6502 decruncher would need).
`raw -c` is compatibility mode: it disables literal sequences (long literal
runs copied verbatim without per-byte flag bits), allowing a smaller and
simpler decruncher.  Measured 2026-07-13.

| file | original | raw | ratio | raw -c | ratio |
|---|--:|--:|--:|--:|--:|
| exile-title.bin | 8320 | 4341 | 52.2% | 4341 | 52.2% |
| droid-title.bin | 20480 | 6972 | 34.0% | 6972 | 34.0% |
| ravenskull-title.bin | 20480 | 12766 | 62.3% | 12877 | 62.9% |
| repton3-title.bin | 10240 | 4914 | 48.0% | 4914 | 48.0% |
| boomscreen.bin | 16000 | 4427 | 27.7% | 4427 | 27.7% |
| blurpscreen.bin | 8320 | 2982 | 35.8% | 2982 | 35.8% |
| exileb.bin | 24704 | 20708 | 83.8% | 20744 | 84.0% |
| chuckie.bin | 9984 | 6499 | 65.1% | 6499 | 65.1% |
| frak2.bin | 13567 | 8918 | 65.7% | 8929 | 65.8% |
| blurp.bin | 18331 | 11441 | 62.4% | 11505 | 62.8% |
| basic2.rom | 16384 | 12230 | 74.6% | 12244 | 74.7% |
| **TOTAL** | **166810** | **96198** | **57.7%** | **96434** | **57.8%** |

Notes:

- Literal sequences buy exomizer only 236 bytes overall (+0.2% without
  them), and only five files change: ravenskull-title +111, blurp +64,
  exileb +36, basic2 +14, frak2 +11.  Literal runs long enough to benefit
  (~30+ bytes) are rare in this corpus, so a format without them concedes
  little.
- exileb.bin is the stress case: Exile's code/data is so dense that even
  exomizer only removes 16%.  Per-literal overhead dominates there.
- Graphics span 27.7-62.3%; executables and the ROM 62.4-83.8%.

## v0

The simplest beebcrunch format (src/v0.h documents it): u16 length header,
3 bits holding B-1 (B in 1..8), then flag-bit tokens - literal = a
byte-aligned byte, match = offset then length: gamma(((offset-1) >> B) + 1)
+ low B raw bits of (offset-1) + gamma(length-1).  Everything is 6502 byte-width friendly: match
lengths cap at 256 and every Elias gamma value caps at 256 (both wrap to 0
in a byte, which is invalid, so unambiguous), which also means an offset is
only encodable under a given B when ((offset-1) >> B) + 1 <= 256.  Optimal
forward parse (exact for the format over nearest-offset-per-length match
candidates), run for every B in 1..8 with the jointly best pair kept.
Measured 2026-07-13.

| file | original | exo -c | v0 | ratio | B |
|---|--:|--:|--:|--:|--:|
| exile-title.bin | 8320 | 4341 | 4575 | 55.0% | 7 |
| droid-title.bin | 20480 | 6972 | 7615 | 37.2% | 8 |
| ravenskull-title.bin | 20480 | 12877 | 13333 | 65.1% | 8 |
| repton3-title.bin | 10240 | 4914 | 5212 | 50.9% | 7 |
| boomscreen.bin | 16000 | 4427 | 4725 | 29.5% | 8 |
| blurpscreen.bin | 8320 | 2982 | 3184 | 38.3% | 7 |
| exileb.bin | 24704 | 20744 | 21474 | 86.9% | 8 |
| chuckie.bin | 9984 | 6499 | 6712 | 67.2% | 6 |
| frak2.bin | 13567 | 8929 | 9225 | 68.0% | 6 |
| blurp.bin | 18331 | 11505 | 11903 | 64.9% | 6 |
| basic2.rom | 16384 | 12244 | 12630 | 77.1% | 8 |
| **TOTAL** | **166810** | **96434** | **100588** | **60.3%** | |

Notes:

- 4390 bytes (+4.6%) behind exomizer raw.  Remaining gaps, in rough order:
  a single fixed offset code for all match lengths, universal gamma codes
  instead of per-file learned ones.
- The optimal parse replaced the original greedy parse (105584 = 63.3%), a
  ~5000-byte / 3.0-point improvement for encoder-side work only.
- The 6502 byte-width constraints (match length <= 256, gamma values <=
  256 so the offset high part bounds match distance per B, bit fields <=
  8) cost just 30 bytes across the whole corpus.
- With the optimal parse, chosen B spreads to 6..8 (greedy always picked
  7 or 8): the parser trades some match distance for cheaper offsets, and
  the joint (parse, B) optimization exploits that.
