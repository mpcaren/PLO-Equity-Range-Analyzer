# plo5 — the PLO5 engine in Rust

A port of the C engine in `../src` (`plo5.c`, `spr.c`, `api.c`): the
5-card evaluator, Monte Carlo and exact equity (single and double board,
fixed / random / range players, street-narrowing chained ranges), the
preflop rank table, board-conditional rankings, stack-off (SPR) analysis,
and the JSON API the web and desktop UIs speak.

It keeps the C engine's algorithms, random streams and floating-point
accumulation order, so **results are bit-identical**: the same seed gives
the same equities, rankings give the same hands, and `plo5rank.bin` files
are interchangeable (a table generated here matches the C tools' file byte
for byte). On top of the C version it adds cooperative cancellation
(`Engine::cancel_flag`) and keeps its caches in an `Engine` value instead
of globals.

```rust
use plo5::{Engine, Player};
use plo5::cards::parse_cards;

let mut e = Engine::new(0); // 0 = all cores
let hand = |s| parse_cards(s, 5).unwrap().try_into().unwrap();
let board = parse_cards("Kh7d2s", 5).unwrap();
let r = e.equity(
    &[Player::Fixed(hand("AhAdKcKd7c")), Player::Range { lo: 70.0, hi: 100.0, chain: vec![] }],
    &board, None, &[], 1_000_000, 1_000_000, 42,
).unwrap();
println!("{:.2}%", r.equity[0] * 100.0);
```

## Tests

```bash
cargo test -p plo5 --release                                   # engine + API tests
cargo test -p plo5 --release --features c-reference            # + compare against the C engine
cargo test -p plo5 --release --features c-reference -- --ignored   # + full rank-table generation (minutes)
cargo run  -p plo5 --release --example bench [threads]         # timings
```

`c-reference` compiles `../src/plo5.c` and `../src/spr.c` with the `cc`
crate and checks the evaluator, exact and seeded Monte Carlo equity, board,
two-board and chained rankings, preflop percentiles, SPR rows, error codes
and rank-table generation against the C engine, requiring exact equality.
Tests that need `plo5rank.bin` use the one in the repository root when
present.

Speed matches the GCC-built C engine for equity and rank-table generation.
Building a board ranking is about 15% slower; the same C code built with
clang (LLVM, like rustc) is slower by the same amount, so it is a code
generation difference rather than extra work.
