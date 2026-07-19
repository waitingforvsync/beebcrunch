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
| v2 | 97951 | 58.7% | +1.8% |
| v3 | 97134 | 58.2% | +1.0% |
| v4 | 96899 | 58.1% | +0.7% |
| v5 | 97076 | 58.2% | +0.9% |
| v6 | 96626 | 57.9% | +0.4% |
| v7 | 96810 | 58.0% | +0.6% |

## Per-file results

Packed sizes; new codecs add a column.  exo -c is the comparison column
(exomizer without literal sequences, the closest analogue of a small-
decoder format).

| file | original | exo -c | v0 | v1 | v2 | v3 | v4 | v5 | v6 | v7 |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| exile-title.bin | 8320 | 4341 | 4575 | 4567 | 4439 | 4426 | 4378 | 4426 | 4361 | 4411 |
| droid-title.bin | 20480 | 6972 | 7615 | 7607 | 7137 | 7131 | 7061 | 7083 | 7043 | 7076 |
| ravenskull-title.bin | 20480 | 12877 | 13333 | 13109 | 13000 | 12777 | 12945 | 12829 | 12871 | 12810 |
| repton3-title.bin | 10240 | 4914 | 5212 | 5182 | 5011 | 4986 | 4970 | 4978 | 4952 | 4973 |
| boomscreen.bin | 16000 | 4427 | 4725 | 4575 | 4430 | 4306 | 4445 | 4308 | 4425 | 4307 |
| blurpscreen.bin | 8320 | 2982 | 3184 | 3142 | 3031 | 2988 | 3016 | 2980 | 3007 | 2977 |
| exileb.bin | 24704 | 20744 | 21474 | 21278 | 21052 | 20941 | 20780 | 20940 | 20739 | 20869 |
| chuckie.bin | 9984 | 6499 | 6712 | 6694 | 6636 | 6628 | 6525 | 6554 | 6500 | 6536 |
| frak2.bin | 13567 | 8929 | 9225 | 9067 | 9105 | 8964 | 8964 | 8960 | 8938 | 8954 |
| blurp.bin | 18331 | 11505 | 11903 | 11735 | 11713 | 11578 | 11553 | 11555 | 11531 | 11539 |
| basic2.rom | 16384 | 12244 | 12630 | 12620 | 12397 | 12409 | 12262 | 12463 | 12259 | 12358 |
| **TOTAL** | **166810** | **96434** | **100588** | **99576** | **97951** | **97134** | **96899** | **97076** | **96626** | **96810** |

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

## v5

v4's five-table match coding (length-1 matches included) carried by v1's
block framing (src/v5.h): alternating literal/match blocks with Elias
gamma counts, a length-1 match being a match token that sits in - and
extends - match blocks.  The frontier parse gains one edge per position
(near1 through the tiny off1 table).  Measured 2026-07-14: 97919 (58.7%)
- WORSE than both parents (v3 97701, v4 97829).  The expected combination
did not stack.

Notes:

- Why length-1 matches pay less here: under flag bits a literal costs a
  full 9 bits, so a ~5-7 bit length-1 match saves ~2-4; under blocks a
  literal inside a literal run costs ~8 bits plus a fractional header, so
  the same match saves ~1 bit or nothing unless it extends a match block.
  Meanwhile the costs stay: the length table's minval drops to 1 for
  every file (bucket space all lengths pay for), plus the off1 header.
  v5 beats v3 only where matches are dense (blurpscreen -9, exileb would
  lose, chuckie etc. go to v4) and loses the graphics wins that made v3
  best (ravenskull +80, boomscreen +10) and basic2.rom (+94, fixpoint
  converging worse).
- Length-1 matches do fix v3's pathology: inputs with no repeated pair
  but nearby repeated bytes (unencodable under v3's 256-token literal
  run cap) become representable - test-enforced.
- Block counts through learned interval tables (two extra tables, one
  per block type) measured WORSE still: 98260 (+341 vs gamma counts,
  worse on every file).  The table family cannot express plain gamma:
  its bucket index is itself gamma coded, so a table-coded count pays
  gamma(index) + extras where raw gamma pays gamma(count) directly (a
  count of 2 costs 4 bits vs 3), and two more headers on top.  Gamma
  counts kept.
- Standing best remains v3.  The three mechanisms do not stack into one
  format for free; a possible reconciliation is making length-1 support
  per-file (one header bit choosing len table minval 1 vs 2, off1 table
  present or absent), which would give max(v3, v5) per file at ~1 bit
  cost - the corpus split suggests roughly v3's total minus v5's few
  graphics wins.

## v6

v4 with a unary length bucket index (src/v6.h): the length table's Elias
gamma index is replaced by exomizer's unary code - index i costs i + 1
bits - capped at 16 buckets (indices 0-15).  Everything else is v4
byte-for-byte: flag bits, five learned tables, length-1 matches, same
parse and fixpoint.  Measured 2026-07-15: 96874 (58.1%) - NEW BEST,
-811 vs v3, -941 vs v4, +0.7% vs exomizer raw, +0.5% vs raw -c.
Length bucket pinning (below) later took it to 96869.

Notes:

- Beats v4 on every file.  The win is where lengths live: hot bucket
  indices 1 and 3 cost 2 and 4 bits under unary against gamma's 3 and 5;
  the optimizer reshapes the buckets around the steeper index and the
  whole length distribution gets cheaper.  Dense executables gain most
  (exileb -307, basic2 -247, chuckie -138 vs v3).
- This was the last unverified structural difference from exomizer's
  format (confirmed against the 3.0.2 source: unary length index, flat
  2/4-bit offset indices, fixed 52-nibble table header).
- The 16-bucket cap is not the driver: 31 vs 16 gamma length buckets was
  previously measured at only ~21 bytes.
- v3 keeps only the graphics wins (boomscreen -124, ravenskull -102 vs
  v6): block framing still pays on long literal runs.  A unary length
  index inside the block chassis (v5 + unary) is the natural next
  measurement; unary is also the cheapest index loop on the 6502.

## Fixpoint and optimizer probes (2026-07-15)

The parse <-> tables loop measured and characterized; one refinement
adopted, the rest answered questions.

- ADOPTED: the partition-DP table optimizer now charges each bucket its
  4 header bits (the serialized width nibble), so it optimizes true size
  rather than data bits and no longer splits a bucket to save less than
  it costs.  Every table codec improved: v2 -17, v3 -16, v4 -14, v5 -13,
  v6 -9 (96908 -> 96899).  With this, a converged fixpoint is a genuine
  coordinate-descent local optimum of the full objective.
- Scoring each parse against the tables it ran under - the pair
  (tokens_i, tables_i), never previously a candidate - changed nothing
  (the alternation is effectively monotone); not kept.
- Seeding matters enormously: a uniform-width seed with identical
  coverage converged ~8% worse (v4 97815 -> 105998).  The gamma-like
  seed encodes the correct prior (short offsets/lengths dominate) into
  the first parse; bad first parses histogram diffusely and the loop
  cannot recover.  Multiple local minima confirmed.
- Among reasonable seeds the basins nearly coincide: restarting from
  three gamma-like seeds (+-1 offset width) and keeping the global best
  gained only 7 bytes corpus-wide for 3x encode time; not kept.
- Previously measured, still true: convergence completes within 8
  iterations (32 identical), and the per-histogram optimizer is exact
  (brute-force validated, now including the header term).

## Offset bucket geometry probes (2026-07-15)

Exomizer-inspired sweep of per-context offset index widths on v6; one
refinement adopted.

- Instrumented the converged tables: every corpus file fills every
  offset context to its cap (4 / 16 / 16 / 16 buckets) - under a flat
  index extra buckets are cheap and the optimizer always wants the
  finer geometry.  (Exomizer pins exactly these counts; only its len-1
  table is small.)
- Narrower indices for the short-length contexts measured WORSE despite
  saving a bit per match: len2 at 3-bit/8 buckets +389, len3 at
  3-bit/8 +77.  The 4-bit/16 geometry stands for all len >= 2 contexts
  - now confirmed against flat 3-bit, flat 5-bit, gamma, and per-context
  narrowing.
- ADOPTED: since the counts always converge to the cap, v6 pins them in
  the format and drops the four 5-bit count fields; the offset tables
  serialize as bare width nibbles (unused tail buckets pad as width 0).
  20 bits per file: 96899 -> 96874.  Bonus: the eventual 6502 table
  reader becomes a fixed-length nibble loop, as in exomizer.  The
  length table keeps its count field (it converges to 9-14 buckets,
  varying per file).

## v7

v6's coding inside the block framing (src/v7.h): alternating
literal/match blocks, the five v6 tables (unary length index, pinned
offset counts), plus - the decisive piece - the block counts themselves
coded through two more learned interval tables (literal-run and
match-run), unary-indexed.  A unary index with geometric widths costs
(i+1) + i = 2i+1 bits, exactly Elias gamma, so the count seed starts at
gamma parity and refits only improve - this removes the structural
handicap that sank the gamma-indexed count-table attempt in v5 (98260).
No Elias gamma remains anywhere in the v7 stream: the decoder needs one
unary reader, flat indices and raw bits.  Measured 2026-07-15: 96877
(58.1%), 3 bytes behind v6 (96874).

Notes:

- Ladder within v7: v5's chassis with unary lengths = 97048; learned
  unary count tables -171 = 96877.  Block-count distributions measured
  first: run length 1 dominates (67% of literal blocks, 54% of match
  blocks), 90%+ at <= 7, gamma slack vs empirical entropy ~342 bytes -
  the tables recover half net of their two headers.
- The blocks-vs-flags verdict tightens to a 3-byte tie overall, but the
  per-file split persists: v7 wins the graphics and match-dense files
  (boomscreen -134, ravenskull -126, blurpscreen -35, blurp -19,
  frak2 -14 vs v6), v6 wins the executables (basic2 -151, exileb -104,
  exile-title -45 vs v7).  Best-of-both per file = 96546: +0.36% vs
  exomizer raw and just +0.12% vs raw -c - a per-file chassis bit is
  now worth 328 bytes over either single codec.
- Every input is representable in v7 (proved, test-enforced): at most
  256 positions in a file are first occurrences of their byte value, so
  a 257-literal run always contains a length-1 escape valve, and long
  match runs convert a match back to literals.  The encoder retries
  from a full-coverage length-1 seed when the tuned seed cannot reach a
  needed escape (pathological inputs only; no corpus file triggers it).
  v1/v3/v5 keep their unencodable corner; v7 closes it.

## Length bucket count sweep (2026-07-16)

Converged length tables use 9-14 buckets per file (5-bit count field,
cap 16).  Swept the cap and the count-field-vs-pinned choice on v6:

- Cap 8 with the count field kept: +45 - shrinking an exact optimizer's
  search space only hurts.
- Pinning the count (no 5-bit field, bare nibbles like the offsets):
  8 -> 96912, 9 -> 96879, 10 -> 96869, 11 -> 96872, 12 -> 96872.
  ADOPTED for v6: pinned 10 = 96869 (-5); the header saving beats the
  forced tail coarsening exactly there.
- v7 measured the other way (pinned 10/12/16 = +27/+33/+19 vs its
  count-field 96877): block framing shifts its length distribution, so
  v7 keeps the 5-bit count at cap 16.  Best-of-both per file is now
  96543 (+0.36% vs exomizer raw, +0.11% vs raw -c).

## Fully fixed table geometry (2026-07-16)

Two structural cleanups, per user direction: the length-1 offset table
was a separate struct member for historical reasons only - it is now
off[0] of a single four-context array (off_ctx: len 1 / 2 / 3 / >= 4),
with per-context index geometry from shared helpers; v4-v7 byte-
identical after the fold.  And no bucket count travels in the stream
anywhere any more: every table's bucket COUNT is a format constant
(bare width nibbles, exomizer-style), while the interval geometry - the
widths - remains fully adaptive per file.

- v6 already had zero count fields (offsets 4/16/16/16, length pinned
  10).
- v7's length and two count tables dropped theirs; converged counts
  measured first (cnt-lit 3-10, cnt-match 7-10, len 9-14), then the
  (len, cnt) pin swept: (16,10) 96903, (14,10) 96892, (16,9) 96894,
  (14,9) 96883, (13,9) 96874 <- adopted, (13,8) 96929, (13,10) 96884,
  (12,9) 96908.  The pinned format now BEATS the count-field one
  (96877) by 3 bytes as well as simplifying the decoder: v7's header is
  exactly 74 nibbles, read by one fixed loop.
- v6 = 96869 and v7 = 96874 both carry zero per-table counts;
  best-of-both per file improves to 96542.

## Gap decomposition probes (2026-07-16)

Where do exomizer's remaining bytes come from, now the formats look
alike?  Two suspects measured, one adopted:

- The 256 length cap is NOT the gap: chained capped matches were counted
  across the corpus - 5 occurrences, all in blurp.bin, ~14 bytes total.
  BBC-sized data has almost no 256+ byte exact repeats, so exomizer's
  16-bit lengths buy it nothing here.
- ADOPTED: v6 merges len3 into a len >= 3 offset context (exomizer's
  split, v6_num_off = 3): 96869 -> 96857.  The len3 context earned +136
  in the v2 era but its 16-nibble header stopped paying once headers
  and length indices got cheap.  v7 measured the opposite (+7) and
  keeps 4 contexts.  v6's header is now a fixed 46 nibbles - SMALLER
  than exomizer's 52.  Length pin 10 re-confirmed under 3 contexts
  (9 +7, 11 +2).
- Standing totals: v6 = 96857, v7 = 96874, best-of-both per file =
  96529 (+331 vs exomizer raw, +95 vs raw -c).

## Steeper offset seeds (2026-07-16)

User suggestion: bias the seed to smaller widths at low indices.  The
geometric seed gives one bucket per octave; a steeper offset seed
({0,0,1,1,2,2,3,4,5,6,8,10,12,14,15,15}, len-1 context {1,2,4,6}) gives
sub-octave resolution at short distances with the same full coverage.
ADOPTED in v6: 96857 -> 96713 (-144), the largest single gain since the
unary length index.  The first parse's histogram inherits the finer
short-offset preferences and the fixpoint keeps the specialization -
more evidence the seed's prior, not just its coverage, picks the basin.

- Attribution: steep offsets alone -144; the steep len-1 seed is worth
  213 of that (baseline off1 seed costs +213); steeper length seeds
  measured WORSE (+114 to +258) - lengths keep the geometric ramp.
  Even-steeper offset variants also lose (+21 to +167).
- The earlier +-1-width restart experiment (-7) perturbed within the
  octave family; changing the seed's SHAPE is what escapes the basin.
- v6 = 96713: +0.5% vs exomizer raw, +279 vs raw -c.  Best-of-both with
  v7 (still on the geometric seed) = 96469.  Applying the same seed
  shape to v7 is the obvious next measurement.

## v7 seed and count-table probes (2026-07-18)

- v6's steep offset seed measured WORSE in v7 (+84): the chassis
  economics differ - blocks make literals cheap, changing which short
  matches the first parse prefers, and v7's geometric-offset basin
  wins.  The two codecs now deliberately seed differently.
- Steep COUNT seed adopted (-3): run length 1 dominates both block
  types (67% / 54%) far beyond a gamma prior, so the count tables seed
  {0,0,0,1,1,2,3,5,8}.  With their unary index, the old geometric seed
  was exact gamma parity; the steeper shape starts closer to the truth.
- Separate literal-run and match-run count tables are decisively
  justified: forcing one shared table (learned jointly, still written
  twice, isolating the modeling loss) costs +187, against ~50 bytes a
  real merge would save in headers.  Net +137 for merging - the two
  run-length distributions differ too much.
- v7 = 96871.  Best-of-both per file = 96470 (+272 vs exomizer
  raw, +36 vs raw -c).

## Per-file seed portfolio (2026-07-18)

Encoder-only, format unchanged, gains-only: each file's fixpoint now
runs from both seed shapes (steep-offset and geometric) and the best
(tokens, tables) pair is kept.  Seed shape picks the fixpoint basin and
the winner differs per file - the earlier global adoptions were the
majority vote, not unanimous.

- v6: 96713 -> 96691 (-22).  v7: 96871 -> 96810 (-61) - the steep
  offset seed that loses overall in the block chassis still wins on
  several files.  Encode time doubles; v7's full-coverage escape seed
  remains as a third attempt that only runs if neither variant parses.
- v6 = 96691 (+0.5% vs exomizer raw, +257 vs raw -c), v7 = 96810.
  Best-of-both per file = 96443: +245 vs exomizer raw,
  +9 vs raw -c.

## Wider seed portfolio for v6 (2026-07-19)

Five seed shape families per table kind (geometric, steep, flat,
shallow, inverted geometric) swept as 21 combinations - every uniform
assignment plus per-table variations - with each file keeping its best.
v6: 96691 -> 96626 (-65).  Nine different combinations win at least one
file: basic2 prefers the plain geometric seed, boomscreen and blurp
want an INVERTED len >= 3 offset seed, exileb and chuckie want a
shallow len-1 seed, ravenskull shallow lengths.  Per-table shape
matters, per-file - the basin structure is richer than any single
global seed.  The portfolio is pruned to the 8 winning combinations
(reproduces the sweep total exactly); flat never won anywhere.

- v6 = 96626: +0.4% vs exomizer raw, +192 vs raw -c.  Best-of-both
  with v7 = 96417: +219 vs raw, -17 vs raw -c - the
  chassis bit now lands BELOW raw -c.
- Build config fixed to Release (the cache had drifted to Debug;
  ratios unaffected, benches ~10x faster - full 8-codec corpus bench
  now 33 s).

## Unary indices everywhere; gamma index mode removed (2026-07-19)

Two structural decisions, per user direction.  First, the composition
tidy-up: tables.* now holds only the single table struct and its
machinery; the one shared composition (coding_tables: four offset
contexts - length 1 / 2 / 3 / over 3 - plus a length table) lives in
coding.h/.c, parameterized by first context and length minval.  There
is no len1_tables type: length-1 is simply offset context 0, which
v2/v3 never use.  The only remaining length-1 special case is the
near1 candidate chain in matches.c, where it genuinely differs.  All
eight codecs verified byte-identical through that refactor.

Second, the Elias gamma bucket-index mode is gone: index_bits == 0 now
means a unary index, everywhere, and the 31-bucket gamma length cap is
replaced by a 16-bucket unary cap.  v2-v5 are deprecated (to be removed
eventually) so their formats were retrofitted rather than preserved,
and all four improved substantially: v2 97951 (-575), v3 97134 (-551),
v4 96899 (-916), v5 97076 (-830).  v6/v7 already used unary and are
unchanged (96626 / 96810).  Their columns above are re-measured under
the new formats; the historical gamma-era numbers survive in the notes
sections and in git history.  The decoder family now needs no Elias
gamma reader anywhere in the table machinery.
