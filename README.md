# plo5calc — 5-card PLO (PLO5) equity calculator

Fast, zero-dependency C11. Monte Carlo simulation with exact enumeration
when the remaining board is small, for 2–6 players, with correct Omaha
rules (exactly 2 hole cards + exactly 3 board cards, best of the
C(5,2) × C(5,3) = 100 combinations). Includes hand-strength percentile
rankings — preflop and conditional on any flop/turn/river — a native
Windows GUI, an equity quiz trainer, an R package, and a **double-board
split-pot mode** ("bomb pot"): two boards, each pays half the pot, best
hand on both boards scoops.

## Double board (split pot)

Available everywhere as a mode/toggle:

- **GUI**: the Single board / Double board toggle at the top of the panel
  adds a second board row (Bd A / Bd B). Equity = expected pot fraction;
  win% = clean scoops. Percentiles and ranges use the combined two-board
  distribution (hands ranked by the sum of their percentiles on each
  board) once both boards have at least a flop.
- **Quiz**: "Double board · split" toggle deals two flops per question.
- **CLI**: `--double` (preflop) or `--board2 "9c 8c 4h"` (implies double);
  batch tokens `double` and `board2:...`.
- **Library**: `plo5_equity_2b`, `plo5_board_ranks_build2`,
  `plo5_hand_percentile_2b`, `plo5_percentile_hand_2b`.
- **R**: `plo5_equity(hands, board=..., board2=...)` or
  `double_board=TRUE`; `plo5_rank_board(board, board2)` etc.

Exact enumeration covers double boards too (e.g. turn/turn enumerates all
ordered river pairs); scoring is verified by unit tests (nuts on both
boards = 100%, nuts on exactly one board = exactly 50%).

## Layout

```
src/     plo5.h plo5.c   engine library
         main.c          CLI (plo5calc)
         gui.c quiz.c    Win32 GUI + equity quiz
tests/   tests.c         unit tests (40 checks)
rplo5/                   R package (src/ holds a copy of the engine)
```

## Build

- **Windows / MSVC:** `./build.bat` — builds `plo5calc.exe` (CLI),
  `plo5tests.exe` (tests), `plo5gui.exe` (GUI) and `plo5quiz.exe` (quiz).
  Note the `/utf-8` flag is required (Unicode literals in the GUIs).
- **gcc / clang (incl. Rtools):** `make`, `make gui`, `make test`, `make bench`

First run: build the preflop rank table once with
`plo5calc --gen-ranks --threads 0` (~2.5 min; writes `plo5rank.bin` next
to the exe — it is generated, not checked in).

## Percentile rankings

Every possible holding is ranked by strength; percentile 0 = weakest,
100 = strongest. Two kinds of distribution:

- **Preflop:** all 2,598,960 starting hands ranked by heads-up equity vs a
  random hand (134,459 suit-isomorphic classes, Monte Carlo). Built once
  with `plo5calc --gen-ranks` (~2.5 min) into `plo5rank.bin` (~24 MB),
  loaded automatically afterwards.
- **Board-conditional:** given a flop/turn/river, all remaining holdings
  are ranked on that board — river by exact made-hand strength, turn by
  average showdown percentile over all rivers (exact), flop by average
  showdown percentile over sampled runouts (`--runouts`, default 100).
  Built on demand in ~1–5 s (multithreaded, cached per board).

A **range player** is a percentile band of whichever distribution matches
the board in play: `"10-40"` preflop means the 10th–40th percentile
starting hands; the same band with a flop set means the 10th–40th
percentile of holdings *on that flop*.

### Street-narrowing (chained) ranges

A range can also *continue* across streets: "only x% of the hands from
the previous street." `"0-50>0-60>40-100"` means keep the flop's best
50%, of those survivors keep the best 60% re-ranked on the turn, then
take the 40th–100th percentile of *those* survivors on the current board.
Up to two continuations (flop, then turn) ahead of the final band; single-
board mode only. Each stage genuinely re-ranks the surviving subset by the
new street's strength — hands that fail a stage are excluded outright, not
just deprioritized. In the GUI, every Range player shows small "flop" and
"turn" band fields under the main slider (left at 0–100 = no filter at
that street).

## CLI

```
plo5calc "AhKhQd Jc Tc" "9s 8s 7d 6h 5c" --board "2h 3h 4d" --trials 1000000
plo5calc "AhAdKcKd7c" "70-100" --board "Kh 7d 2s"      # hand vs range-on-flop
plo5calc "AhAdKhKd7s" random                            # vs random
plo5calc "0-50>0-60>40-100" "0-100" --board "Kh7d2s9c"  # chained (street-narrowing) range
plo5calc --percentile 30                                # 30th pct preflop hand
plo5calc --percentile 95 --board "Kh 7d 2s"             # 95th pct hand on flop
plo5calc --rank-of AhAd2c2d9c --board "Kh 7d 2s"        # hand -> pct on flop
plo5calc --batch matchups.txt --trials 200000           # CSV output
plo5calc --bench
```

Options: `--dead`, `--seed`, `--threads N` (0 = all cores), `--exact`,
`--mc`, `--max-enum`, `--runouts`, `--ranks FILE`, `--rank-trials`.
Monte Carlo output includes a 95% confidence half-width; exact enumeration
kicks in automatically up to 1M runouts (covers heads-up preflop).

## GUI (plo5gui.exe)

Native Win32, one small exe. Per player, pick a mode:

- **Hand** — 5 card slots; click the deck (auto-advances), or type `a` `h`
  for A♥; right-click removes; shows the hand's percentile on the current
  street
- **Range** — dual-handle percentile slider plus numeric fields; the band
  is interpreted on the current board (preflop table when no board).
  Small "flop"/"turn" fields below it narrow the range street by street
  (see "Street-narrowing ranges" above); leave them at 0–100 to skip
- **Rnd** — random hand each trial

Plus: **Calculate** button (Enter) with an **Auto** toggle for live
recalculation; results dim when out of date. **Rank board** builds the
board-conditional ranking for the current board. **Pct hand** box: type a
percentile, see the hand at that percentile on the current street, and
**Deal** it to the selected player. Precision presets Fast/Balanced/Precise;
dead-card slots; Copy puts a text summary on the clipboard.

## Equity quiz (plo5quiz.exe)

Training tool: each question deals you a random 5-card hand, a random flop
and a random number of opponents (1–5, full random ranges). Type your
equity estimate and press Enter; you get the true equity (500k trials),
your error with a grade, win/tie split, and your hand's percentile on
that flop — plus a running average error for the session. The answer is
computed in the background while you think, so the reveal is instant.

## Library (plo5.h + plo5.c)

Key entry points (see header for details):

```c
plo5_equity2(players, np, board, nb, dead, nd, trials, max_enum, seed, threads, &result);
plo5_ranks_generate / plo5_ranks_load / plo5_ranks_loaded            /* preflop */
plo5_board_ranks_build / plo5_board_ranks_state                      /* postflop */
plo5_hand_percentile_on(cards, board, nb)   /* hand -> pct (board-aware) */
plo5_percentile_hand_on(pct, board, nb, out)/* pct -> hand (board-aware) */
plo5_chain_rank_build / plo5_chain_state / plo5_chain_hand_percentile /* chained ranges */
```

`plo5_player` is fixed cards, random, or a `{lo, hi}` percentile band. A
range player can also set `chain_n` (0–2) with `chain_lo/chain_hi[2]` to
narrow progressively across streets — see `plo5_player` in plo5.h for the
exact semantics of each ancestor slot (0 = flop, 1 = turn).
Card id = `rank*4 + suit` (rank 0=`2`…12=`A`; suit 0=c 1=d 2=h 3=s).

Notes: range/random players are Monte Carlo only. With multiple range
players, hands are dealt sequentially with rejection (the standard
approach; joint conditioning is approximated). Flop rankings use sampled
runouts — deterministic per board, tune with `--runouts`.

## R package (rplo5/)

Full wrapper of the engine. Needs Rtools (R's C toolchain) once:
install from https://cran.r-project.org/bin/windows/Rtools/ then

```r
install.packages("C:/PLO software/rplo5", repos = NULL, type = "source")
library(rplo5)

plo5_load_ranks("C:/PLO software/plo5rank.bin")
plo5_equity(c("AhAdKcKd7c", "70-100"), board = "Kh7d2s")
plo5_equity(c("AhAdKhKd7s", "random"))          # equity vs random
plo5_rank_board("Kh7d2s")                        # build flop ranking
plo5_hand_percentile("AhAd2c2d9c", "Kh7d2s")     # -> 94.8
plo5_percentile_hand(30)                         # 30th pct preflop hand
```

`plo5_equity` returns a data.frame (hand, equity, win, tie, ci95) — ready
for ggplot2. When the engine (`src/plo5.c`/`src/plo5.h`) changes, re-copy
them into `rplo5/src/` before reinstalling.
