# beebcrunch test corpus

Typical BBC Micro binaries for evaluating compression (target: ratios comparable
to Exomizer 3.0, with a small/simple 6502 decompressor).

Each `.bin` has a companion `.inf` (DFS name, load addr, exec addr, length in hex)
used to build `data/corpus.ssd`.

## Title screens

| File | Bytes | Format | Source |
|---|---|---|---|
| `exile-title.bin` | 8320 | MODE 5 layout, 26 char rows (6 blank bottom rows trimmed), 4 col (black/magenta/blue/white via VDU19), &5800 | Exile (Superior), STH `Superior/Exile.zip`. Stored compressed on disk; captured from RAM &5800-&7FFF in beebjit after the loader unpacked it |
| `droid-title.bin` | 20480 | MODE 1, &3000 | Codename Droid (Superior), file `$.CDroid2`, STH `Superior/CodenameDroid.zip` |
| `ravenskull-title.bin` | 20480 | MODE 1, &3000 | Ravenskull (Superior), file `$.RPIC`, STH `Superior/Ravenskull.zip` |
| `repton3-title.bin` | 10240 | MODE 5, &5800 | Repton 3 (Superior), screen portion of `$.SPEAKSC` (file loads at &4700; screen = offset &1100), STH `Superior/Repton3.zip` |
| `boomscreen.bin` | 16000 | MODE 2, &3000, 25 char rows (160x200) | Converted from `data/boomscreen.png` |
| `blurpscreen.bin` | 8320 | MODE 5, &5800, 26 char rows | Blurp title (Retro Software), pre-existing file |

## Executables

| File | Bytes | Load/Exec | Source |
|---|---|---|---|
| `exileb.bin` | 24704 | &1200 / &7200 | `$.ExileB`, Exile (Superior), STH |
| `chuckie.bin` | 9984 | &1100 / &29AB | `$.CH_EGG`, Chuckie Egg (A&F), STH `AnF/ChuckieEgg.zip` |
| `frak2.bin` | 13567 | &2800 / &2950 | `$.FRAK2`, Frak! (Aardvark), STH `Aardvark/Frak.zip` |
| `blurp.bin` | 18331 | &1200 / &5858 | `$.Blurp`, Onslaught.ssd (Retro Software) |
| `basic2.rom` | 16384 | &8000 (sideways ROM) | BBC BASIC II, from github.com/tom-seddon/b2 etc/roms |

## corpus.ssd

All of the above on one DFS disk. Shift-Break boot runs a slideshow: each title
screen is shown in its proper mode/palette; press any key to advance
(CDROID2 -> RPIC -> TREP3 -> TEXILE -> BOOM -> BLURPS). Executables and the
BASIC2 ROM are on the disk as plain files (they are not run).

STH = https://stairwaytohell.com/bbc/archive/diskimages/
