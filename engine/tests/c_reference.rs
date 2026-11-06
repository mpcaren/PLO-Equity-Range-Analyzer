//! Differential tests: the Rust engine against the original C engine
//! (src/plo5.c, src/spr.c), compiled in by `--features c-reference`.
//!
//!     cargo test -p plo5 --release --features c-reference
//!     cargo test -p plo5 --release --features c-reference -- --ignored   # + rank-table generation (minutes)
//!
//! Results must be identical, not just close: same evaluator values, same
//! random streams, same floating-point accumulation order.
#![cfg(feature = "c-reference")]

use std::os::raw::{c_char, c_int, c_void};
use std::path::PathBuf;
use std::sync::Mutex;

use plo5::rng::Rng;
use plo5::{Engine, Player};

const THREADS: usize = 4;

#[repr(C)]
#[derive(Clone, Copy)]
struct CPlayer {
    ty: c_int,
    cards: [c_int; 5],
    lo: f64,
    hi: f64,
    chain_n: c_int,
    chain_lo: [f64; 2],
    chain_hi: [f64; 2],
}

#[repr(C)]
struct CResult {
    equity: [f64; 6],
    win: [f64; 6],
    tie: [f64; 6],
    ci95: [f64; 6],
    samples: u64,
    exact: c_int,
    hand_type: [[f64; 9]; 6],
    hand_type_b: [[f64; 9]; 6],
}

#[repr(C)]
#[derive(Default)]
struct CSprRow {
    spr: f64,
    stack: f64,
    pot_final: f64,
    mdf: f64,
    trim_lo: f64,
    trim_hi: f64,
    eq_needed: f64,
    hero_eq: f64,
    ci95: f64,
    profitable_no_fold: c_int,
    breakeven_fold: f64,
    band_widened: c_int,
}

type CProg = Option<extern "C" fn(c_int, c_int, *mut c_void)>;

extern "C" {
    fn plo5_init();
    fn plo5_eval5(c: *const c_int) -> u32;
    #[allow(clippy::too_many_arguments)]
    fn plo5_equity2(p: *const CPlayer, np: c_int, b: *const c_int, nb: c_int, d: *const c_int, nd: c_int,
                    trials: u64, max_enum: u64, seed: u64, nt: c_int, out: *mut CResult) -> c_int;
    #[allow(clippy::too_many_arguments)]
    fn plo5_equity_2b(p: *const CPlayer, np: c_int, b1: *const c_int, nb1: c_int, b2: *const c_int, nb2: c_int,
                      d: *const c_int, nd: c_int, trials: u64, max_enum: u64, seed: u64, nt: c_int,
                      out: *mut CResult) -> c_int;
    fn plo5_ranks_load(path: *const c_char) -> c_int;
    fn plo5_ranks_generate(path: *const c_char, tpc: u32, nt: c_int, seed: u64, prog: CProg, ud: *mut c_void) -> c_int;
    fn plo5_hand_percentile(c: *const c_int) -> f64;
    fn plo5_percentile_hand(pct: f64, out: *mut c_int) -> c_int;
    fn plo5_board_ranks_build(b: *const c_int, nb: c_int, runouts: c_int, nt: c_int, prog: CProg, ud: *mut c_void) -> c_int;
    fn plo5_hand_percentile_on(c: *const c_int, b: *const c_int, nb: c_int) -> f64;
    fn plo5_percentile_hand_on(pct: f64, b: *const c_int, nb: c_int, out: *mut c_int) -> c_int;
    fn plo5_board_ranks_build2(b1: *const c_int, nb1: c_int, b2: *const c_int, nb2: c_int, runouts: c_int, nt: c_int,
                               prog: CProg, ud: *mut c_void) -> c_int;
    fn plo5_hand_percentile_2b(c: *const c_int, b1: *const c_int, nb1: c_int, b2: *const c_int, nb2: c_int) -> f64;
    fn plo5_percentile_hand_2b(pct: f64, b1: *const c_int, nb1: c_int, b2: *const c_int, nb2: c_int, out: *mut c_int) -> c_int;
    fn plo5_chain_rank_build(slot: c_int, b: *const c_int, nb: c_int, chain_n: c_int, lo: *const f64, hi: *const f64,
                             runouts: c_int, nt: c_int, prog: CProg, ud: *mut c_void) -> c_int;
    fn plo5_chain_percentile_hand(slot: c_int, pct: f64, out: *mut c_int) -> c_int;
    #[allow(clippy::too_many_arguments)]
    fn plo5_spr_row_eval(hero: *const c_int, b: *const c_int, nb: c_int, tlo: f64, thi: f64, nopp: c_int, pot: f64,
                         spr: f64, trials: u64, seed: u64, nt: c_int, row: *mut CSprRow) -> c_int;
}

/// the C engine keeps global caches: one test at a time
static C_LOCK: Mutex<()> = Mutex::new(());

fn ids(s: &str) -> Vec<usize> {
    plo5::cards::parse_cards(s, 52).unwrap_or_else(|| panic!("bad cards {s}"))
}
fn cids(v: &[usize]) -> Vec<c_int> {
    v.iter().map(|&c| c as c_int).collect()
}
fn c5(v: &[usize]) -> [usize; 5] {
    v.try_into().unwrap()
}

fn to_c(p: &Player) -> CPlayer {
    let mut c = CPlayer { ty: 1, cards: [0; 5], lo: 0.0, hi: 0.0, chain_n: 0, chain_lo: [0.0; 2], chain_hi: [0.0; 2] };
    match p {
        Player::Fixed(h) => {
            c.ty = 0;
            c.cards = h.map(|x| x as c_int);
        }
        Player::Random => c.ty = 1,
        Player::Range { lo, hi, chain } => {
            c.ty = 2;
            c.lo = *lo;
            c.hi = *hi;
            c.chain_n = chain.len() as c_int;
            for (i, &(l, h)) in chain.iter().enumerate() {
                c.chain_lo[i] = l;
                c.chain_hi[i] = h;
            }
        }
    }
    c
}

fn spec(s: &str) -> Player {
    plo5::api::parse_player(s).unwrap_or_else(|| panic!("bad spec {s}"))
}

struct Case<'a> {
    players: &'a [&'a str],
    board: &'a str,
    board2: Option<&'a str>,
    dead: &'a str,
    trials: u64,
    max_enum: u64,
    seed: u64,
}

fn compare_equity(rs: &mut Engine, case: &Case) {
    let players: Vec<Player> = case.players.iter().map(|s| spec(s)).collect();
    let cp: Vec<CPlayer> = players.iter().map(to_c).collect();
    let (b, d) = (ids(case.board), ids(case.dead));
    let b2 = case.board2.map(ids);
    let (cb, cd) = (cids(&b), cids(&d));
    let mut out: CResult = unsafe { std::mem::zeroed() };
    let rc = unsafe {
        match &b2 {
            Some(b2) => {
                let cb2 = cids(b2);
                plo5_equity_2b(cp.as_ptr(), cp.len() as c_int, cb.as_ptr(), cb.len() as c_int, cb2.as_ptr(),
                               cb2.len() as c_int, cd.as_ptr(), cd.len() as c_int, case.trials, case.max_enum,
                               case.seed, THREADS as c_int, &mut out)
            }
            None => plo5_equity2(cp.as_ptr(), cp.len() as c_int, cb.as_ptr(), cb.len() as c_int, cd.as_ptr(),
                                 cd.len() as c_int, case.trials, case.max_enum, case.seed, THREADS as c_int, &mut out),
        }
    };
    let r = rs.equity(&players, &b, b2.as_deref(), &d, case.trials, case.max_enum, case.seed);
    let label = format!("{:?} board {} {:?}", case.players, case.board, case.board2);
    match r {
        Err(e) => assert_eq!(rc, e.code(), "{label}: error code differs"),
        Ok(r) => {
            assert_eq!(rc, 0, "{label}: C failed with {rc}, Rust succeeded");
            assert_eq!(r.exact, out.exact != 0, "{label}: exact");
            assert_eq!(r.samples, out.samples, "{label}: samples");
            for p in 0..players.len() {
                assert_eq!(r.equity[p].to_bits(), out.equity[p].to_bits(), "{label}: equity P{p}");
                assert_eq!(r.win[p].to_bits(), out.win[p].to_bits(), "{label}: win P{p}");
                assert_eq!(r.tie[p].to_bits(), out.tie[p].to_bits(), "{label}: tie P{p}");
                assert_eq!(r.ci95[p].to_bits(), out.ci95[p].to_bits(), "{label}: ci95 P{p}");
                assert_eq!(r.hand_type[p], out.hand_type[p], "{label}: hand types P{p}");
                assert_eq!(r.hand_type_b[p], out.hand_type_b[p], "{label}: hand types B P{p}");
            }
            eprintln!("  ok  {label}: {:.4} ({} {})", r.equity[0] * 100.0, r.samples, if r.exact { "exact" } else { "MC" });
        }
    }
}

fn rank_path() -> Option<PathBuf> {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..").join("plo5rank.bin");
    p.exists().then_some(p)
}

fn cstr(p: &std::path::Path) -> std::ffi::CString {
    std::ffi::CString::new(p.to_str().unwrap()).unwrap()
}

#[test]
fn evaluator_matches_c() {
    let _g = C_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    unsafe { plo5_init() };
    let t = plo5::eval::tables();
    let mut rng = Rng::new(99);
    for _ in 0..300_000 {
        let mut deck: Vec<usize> = (0..52).collect();
        for i in 0..5 {
            let j = i + rng.below((52 - i) as u32) as usize;
            deck.swap(i, j);
        }
        let h = c5(&deck[..5]);
        let ch = cids(&h);
        assert_eq!(t.eval5(h), unsafe { plo5_eval5(ch.as_ptr()) }, "{h:?}");
    }
    assert_eq!(t.vals.len(), 7462, "distinct 5-card values");
}

#[test]
fn equity_matches_c() {
    let _g = C_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    unsafe { plo5_init() };
    let mut rs = Engine::new(THREADS);
    if let Some(p) = rank_path() {
        rs.ranks_load(&p).unwrap();
        assert_eq!(unsafe { plo5_ranks_load(cstr(&p).as_ptr()) }, 0);
    }
    let have_ranks = rs.ranks_loaded();
    let m = 1_000_000;
    let cases = [
        // exact: flop / turn / river, 2 and 3 players
        Case { players: &["AhAdKcKd7c", "9s8s7d6h5c"], board: "QhJc2s", board2: None, dead: "", trials: 0, max_enum: m, seed: 1 },
        Case { players: &["AhAdKcKd7c", "9s8s7d6h5c", "QcQsJdTd3h"], board: "Kh7d2s9c", board2: None, dead: "", trials: 0, max_enum: m, seed: 1 },
        Case { players: &["AhAdKcKd7c", "9s8s7d6h5c"], board: "QhJc2s9c4d", board2: None, dead: "", trials: 0, max_enum: m, seed: 1 },
        Case { players: &["AhAdKcKd7c", "9s8s7d6h5c"], board: "QhJc2s", board2: None, dead: "2c3c4c", trials: 0, max_enum: m, seed: 1 },
        // preflop: exact with a big enumeration budget, and seeded MC
        Case { players: &["AhAdKhKd7s", "9s8s7d6h5c"], board: "", board2: None, dead: "", trials: 0, max_enum: 2_000_000, seed: 1 },
        Case { players: &["AhAdKhKd7s", "9s8s7d6h5c"], board: "", board2: None, dead: "", trials: 300_000, max_enum: 0, seed: 7 },
        // random opponents, multiway
        Case { players: &["AhAdKcKd7c", "random", "random"], board: "Kh7d2s", board2: None, dead: "", trials: 250_001, max_enum: m, seed: 42 },
        Case { players: &["random", "random", "random", "random", "random", "random"], board: "", board2: None, dead: "", trials: 120_000, max_enum: m, seed: 3 },
        // board-conditional ranges and chained ranges
        Case { players: &["AhAdKcKd7c", "70-100"], board: "Kh7d2s", board2: None, dead: "", trials: 200_000, max_enum: m, seed: 11 },
        Case { players: &["AhAdKcKd7c", "0-50>40-100", "20-90"], board: "Kh7d2s9c", board2: None, dead: "", trials: 150_000, max_enum: m, seed: 12 },
        Case { players: &["QsQhJsJh2c", "0-50>0-60>40-100"], board: "Kh7d2s9c3h", board2: None, dead: "", trials: 150_000, max_enum: m, seed: 13 },
        // double board: exact, MC with random players, ranges on both flops
        Case { players: &["AhAdKcKd7c", "9s8s7d6h5c"], board: "QhJc2s3d", board2: Some("Tc9d4h5s"), dead: "", trials: 0, max_enum: m, seed: 1 },
        Case { players: &["AhAdKcKd7c", "random", "random"], board: "QhJc2s", board2: Some("Tc9d4h"), dead: "", trials: 150_000, max_enum: m, seed: 5 },
        Case { players: &["AhAdKcKd7c", "60-100"], board: "QhJc2s", board2: Some("Tc9d4h"), dead: "", trials: 100_000, max_enum: m, seed: 6 },
        // errors must agree
        Case { players: &["AhAdKcKd7c", "AhKs2c3c4c"], board: "", board2: None, dead: "", trials: 1000, max_enum: m, seed: 1 },
        Case { players: &["AhAdKcKd7c", "9s8s7d6h5c"], board: "QhJc", board2: None, dead: "", trials: 1000, max_enum: m, seed: 1 },
        Case { players: &["AhAdKcKd7c", "random"], board: "", board2: None, dead: "", trials: 0, max_enum: m, seed: 1 },
    ];
    for c in &cases {
        compare_equity(&mut rs, c);
    }
    if have_ranks {
        for c in [
            Case { players: &["AhAdKhKd7s", "10-60", "70-100"], board: "", board2: None, dead: "", trials: 200_000, max_enum: m, seed: 21 },
            Case { players: &["AhAdKhKd7s", "50-100"], board: "", board2: Some(""), dead: "", trials: 100_000, max_enum: m, seed: 22 },
        ] {
            compare_equity(&mut rs, &c);
        }
    } else {
        eprintln!("  (no plo5rank.bin in the repo root: preflop range cases skipped)");
    }
}

#[test]
fn board_rankings_match_c() {
    let _g = C_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    unsafe { plo5_init() };
    let mut rs = Engine::new(THREADS);
    let mut rng = Rng::new(5);
    let rand_hand = |rng: &mut Rng, used: &[usize]| -> [usize; 5] {
        let mut deck: Vec<usize> = (0..52).filter(|c| !used.contains(c)).collect();
        for i in 0..5 {
            let j = i + rng.below((deck.len() - i) as u32) as usize;
            deck.swap(i, j);
        }
        c5(&deck[..5])
    };
    let none: CProg = None;
    for board in ["Kh7d2s", "QhJcTs9c", "AhKd7c7s2d"] {
        let b = ids(board);
        let cb = cids(&b);
        assert_eq!(unsafe { plo5_board_ranks_build(cb.as_ptr(), cb.len() as c_int, 100, THREADS as c_int, none, std::ptr::null_mut()) }, 0);
        rs.board_ranks_build(&b, 100, &mut |_, _| {}).unwrap();
        for k in 0..=400 {
            let pct = k as f64 * 0.25;
            let mut out = [0 as c_int; 5];
            unsafe { plo5_percentile_hand_on(pct, cb.as_ptr(), cb.len() as c_int, out.as_mut_ptr()) };
            let r = rs.percentile_hand_on(pct, &b).unwrap();
            assert_eq!(cids(&r), out.to_vec(), "{board} pct {pct}");
        }
        for _ in 0..3000 {
            let h = rand_hand(&mut rng, &b);
            let ch = cids(&h);
            let c = unsafe { plo5_hand_percentile_on(ch.as_ptr(), cb.as_ptr(), cb.len() as c_int) };
            assert_eq!(rs.hand_percentile_on(&h, &b).unwrap().to_bits(), c.to_bits(), "{board} {h:?}");
        }
        eprintln!("  ok  board ranking {board}");
    }

    // two-board distribution
    let (b1, b2) = (ids("QhJc2s"), ids("Tc9d4h"));
    let (c1, c2) = (cids(&b1), cids(&b2));
    assert_eq!(unsafe { plo5_board_ranks_build2(c1.as_ptr(), 3, c2.as_ptr(), 3, 100, THREADS as c_int, none, std::ptr::null_mut()) }, 0);
    rs.board_ranks_build2(&b1, &b2, 100, &mut |_, _| {}).unwrap();
    for k in 0..=200 {
        let pct = k as f64 * 0.5;
        let mut out = [0 as c_int; 5];
        unsafe { plo5_percentile_hand_2b(pct, c1.as_ptr(), 3, c2.as_ptr(), 3, out.as_mut_ptr()) };
        assert_eq!(cids(&rs.percentile_hand_2b(pct, &b1, &b2).unwrap()), out.to_vec(), "2b pct {pct}");
    }
    let both: Vec<usize> = b1.iter().chain(&b2).copied().collect();
    for _ in 0..2000 {
        let h = rand_hand(&mut rng, &both);
        let ch = cids(&h);
        let c = unsafe { plo5_hand_percentile_2b(ch.as_ptr(), c1.as_ptr(), 3, c2.as_ptr(), 3) };
        assert_eq!(rs.hand_percentile_2b(&h, &b1, &b2).unwrap().to_bits(), c.to_bits());
    }
    eprintln!("  ok  two-board ranking");

    // chained ranking (flop and turn filters) on a river
    let b = ids("Kh7d2s9c3h");
    let cb = cids(&b);
    let (lo, hi) = ([0.0, 0.0], [50.0, 60.0]);
    assert_eq!(unsafe { plo5_chain_rank_build(2, cb.as_ptr(), 5, 2, lo.as_ptr(), hi.as_ptr(), 100, THREADS as c_int, none, std::ptr::null_mut()) }, 0);
    rs.chain_rank_build(2, &b, &lo, &hi, 100, &mut |_, _| {}).unwrap();
    let list = rs.chain_table(2).unwrap();
    for k in 0..=200 {
        let pct = k as f64 * 0.5;
        let mut out = [0 as c_int; 5];
        unsafe { plo5_chain_percentile_hand(2, pct, out.as_mut_ptr()) };
        assert_eq!(cids(&list.hand_at(pct)), out.to_vec(), "chain pct {pct}");
    }
    eprintln!("  ok  chained ranking ({} survivors)", list.len());
}

#[test]
fn preflop_table_and_spr_match_c() {
    let _g = C_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    unsafe { plo5_init() };
    let Some(path) = rank_path() else {
        eprintln!("  (no plo5rank.bin in the repo root: skipped)");
        return;
    };
    assert_eq!(unsafe { plo5_ranks_load(cstr(&path).as_ptr()) }, 0);
    let mut rs = Engine::new(THREADS);
    rs.ranks_load(&path).unwrap();
    let mut rng = Rng::new(17);
    for _ in 0..20_000 {
        let mut deck: Vec<usize> = (0..52).collect();
        for i in 0..5 {
            let j = i + rng.below((52 - i) as u32) as usize;
            deck.swap(i, j);
        }
        let h = c5(&deck[..5]);
        let ch = cids(&h);
        assert_eq!(rs.hand_percentile(&h).unwrap().to_bits(), unsafe { plo5_hand_percentile(ch.as_ptr()) }.to_bits());
    }
    for k in 0..=1000 {
        let pct = k as f64 * 0.1;
        let mut out = [0 as c_int; 5];
        unsafe { plo5_percentile_hand(pct, out.as_mut_ptr()) };
        assert_eq!(cids(&rs.percentile_hand(pct).unwrap()), out.to_vec());
    }
    eprintln!("  ok  preflop percentiles");

    // SPR rows (hero vs MDF-trimmed ranges)
    let b = ids("Kh7d2s");
    let cb = cids(&b);
    let hero = c5(&ids("AhAdKcKd7c"));
    for (nopp, sprv) in [(1usize, 3.0), (2, 5.0), (3, 8.0)] {
        let (tlo, thi) = plo5::spr::mdf_trim_band(0.0, 100.0, plo5::spr::mdf(sprv));
        let mut row = CSprRow::default();
        let ch = cids(&hero);
        let rc = unsafe { plo5_spr_row_eval(ch.as_ptr(), cb.as_ptr(), 3, tlo, thi, nopp as c_int, 10.0, sprv, 60_000, 31, THREADS as c_int, &mut row) };
        let r = rs.spr_row_eval(hero, &b, tlo, thi, nopp, 10.0, sprv, 60_000, 31);
        match r {
            Ok(r) => {
                assert_eq!(rc, 0);
                assert_eq!(r.hero_eq.to_bits(), row.hero_eq.to_bits(), "spr {sprv} x{nopp}");
                assert_eq!(r.eq_needed.to_bits(), row.eq_needed.to_bits());
                assert_eq!(r.breakeven_fold.to_bits(), row.breakeven_fold.to_bits());
                assert_eq!(r.pot_final.to_bits(), row.pot_final.to_bits());
            }
            Err(e) => assert_eq!(rc, e.code()),
        }
    }
    eprintln!("  ok  SPR rows");
}

/// Builds the full preflop table with both engines (same seed, trials and
/// threads) and compares the files byte for byte. Slow: run with --ignored.
#[test]
#[ignore]
fn rank_generation_matches_c() {
    let _g = C_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    unsafe { plo5_init() };
    let dir = std::env::temp_dir().join(format!("plo5-gen-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let (cpath, rpath) = (dir.join("c.bin"), dir.join("rust.bin"));
    let trials = 1000;
    let t0 = std::time::Instant::now();
    assert_eq!(unsafe { plo5_ranks_generate(cstr(&cpath).as_ptr(), trials, THREADS as c_int, 20260704, None, std::ptr::null_mut()) }, 0);
    let tc = t0.elapsed();
    let mut rs = Engine::new(THREADS);
    let t1 = std::time::Instant::now();
    rs.ranks_generate(trials, 20260704, &mut |_, _| {}).unwrap();
    let tr = t1.elapsed();
    rs.ranks_save(&rpath, trials).unwrap();
    let (a, b) = (std::fs::read(&cpath).unwrap(), std::fs::read(&rpath).unwrap());
    eprintln!("  C {:.1}s, Rust {:.1}s, {} bytes", tc.as_secs_f64(), tr.as_secs_f64(), a.len());
    assert!(a == b, "rank tables differ");
    let _ = std::fs::remove_dir_all(&dir);
}
