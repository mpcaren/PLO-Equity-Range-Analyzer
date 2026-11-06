//! Quick timings: `cargo run -p plo5 --release --example bench [threads]`
use std::time::Instant;

use plo5::cards::parse_cards;
use plo5::{Engine, Player};

fn hand(s: &str) -> [usize; 5] {
    parse_cards(s, 5).unwrap().try_into().unwrap()
}

fn main() {
    let threads = std::env::args().nth(1).and_then(|s| s.parse().ok()).unwrap_or(0);
    let mut e = Engine::new(threads);
    println!("threads: {}", e.threads);
    let flop = parse_cards("Kh7d2s", 5).unwrap();

    let t = Instant::now();
    e.board_ranks_build(&flop, std::env::var("RUNOUTS").ok().and_then(|s| s.parse().ok()).unwrap_or(100), &mut |_, _| {}).unwrap();
    println!("flop ranking build          {:>8.0} ms", t.elapsed().as_secs_f64() * 1e3);

    let t = Instant::now();
    let r = e.equity(&[Player::Fixed(hand("AhAdKcKd7c")), Player::Random, Player::Random], &flop, None, &[], 1_000_000, 0, 42).unwrap();
    println!("AA vs 2 random, flop, 1M    {:>8.0} ms  ({:.3}%)", t.elapsed().as_secs_f64() * 1e3, r.equity[0] * 100.0);

    let six = vec![Player::Random; 6];
    let t = Instant::now();
    let r = e.equity(&six, &[], None, &[], 1_000_000, 0, 42).unwrap();
    println!("6 random, preflop, 1M       {:>8.0} ms  ({:.3}%)", t.elapsed().as_secs_f64() * 1e3, r.equity[0] * 100.0);

    let t = Instant::now();
    let r = e.equity(&[Player::Fixed(hand("AhAdKhKd7s")), Player::Fixed(hand("9s8s7d6h5c"))], &[], None, &[], 0, 2_000_000, 1).unwrap();
    println!("heads-up preflop, exact     {:>8.0} ms  ({} runouts)", t.elapsed().as_secs_f64() * 1e3, r.samples);
}
