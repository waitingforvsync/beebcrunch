# Compression results

Corpus: the eleven BBC Micro binaries in `corpus/` (provenance in
`corpus/README.md`).  Ratio = packed size / original size; lower is better.

## Exomizer 3.0.2 baseline

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

- Literal sequences buy exomizer only 236 bytes overall (+0.3% without
  them), and only five files change: ravenskull-title +111, blurp +64,
  exileb +36, basic2 +14, frak2 +11.  Literal runs long enough to benefit (~30+ bytes)
  are rare in this corpus, so a format without them concedes little.
- exileb.bin is the stress case: Exile's code/data is so dense that even
  exomizer only removes 16%.  Per-literal overhead dominates there.
- Graphics span 27.7-62.3%; executables and the ROM 62.4-83.8%.
