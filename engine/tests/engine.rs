//! Engine tests that need no C compiler (ports of tests/tests.c plus the
//! API and cancellation behaviour the desktop app relies on).

use std::sync::atomic::Ordering;

use plo5::api::{self, Presets};
use plo5::cards::parse_cards;
use plo5::eval::tables;
use plo5::{Engine, Error, Player};

fn hand(s: &str) -> [usize; 5] {
    parse_cards(s, 5).unwrap().try_into().unwrap()
}
fn ids(s: &str) -> Vec<usize> {
    parse_cards(s, 52).unwrap()
}

#[test]
fn category_counts_over_all_hands() {
    let t = tables();
    let mut cnt = [0u64; 9];
    for a in 0..48 {
        for b in a + 1..49 {
            for c in b + 1..50 {
                for d in c + 1..51 {
                    for e in d + 1..52 {
                        cnt[(t.eval5([a, b, c, d, e]) >> 24) as usize] += 1;
                    }
                }
            }
        }
    }
    assert_eq!(cnt, [1_302_540, 1_098_240, 123_552, 54_912, 10_200, 5_108, 3_744, 624, 40]);
}

#[test]
fn hand_ranking_order() {
    let t = tables();
    let v = |s: &str| t.eval5(hand(s));
    assert!(v("AsKsQsJsTs") > v("9h9d9c9s2d"));
    assert!(v("9h9d9c9s2d") > v("KhKdKc2s2d"));
    assert!(v("2h3h4h5h7h") > v("AsKdQcJhTs"));
    assert!(v("5d4c3h2sAs") < v("6d5c4h3s2s")); // wheel is the lowest straight
    assert_eq!(v("AhKdQc9s7h"), v("AdKcQh9h7s"));
}

#[test]
fn exact_equity_and_omaha_rules() {
    let mut e = Engine::new(2);
    // must use exactly two hole cards: four aces in hand don't make quads
    let r = e
        .equity(&[Player::Fixed(hand("AhAdAcAs2c")), Player::Fixed(hand("KhKd3c4d5h"))], &ids("Kc7h2d9s8c"), None, &[], 0, 1_000_000, 1)
        .unwrap();
    assert!(r.exact);
    assert_eq!(r.equity, vec![0.0, 1.0]); // KK makes trips; AA only a pair
    let r = e
        .equity(&[Player::Fixed(hand("AhAdKcKd7c")), Player::Fixed(hand("9s8s7d6h5c"))], &ids("QhJc2s"), None, &[], 0, 1_000_000, 1)
        .unwrap();
    assert_eq!(r.samples, 741);
    assert!((r.equity[0] + r.equity[1] - 1.0).abs() < 1e-12);
    assert!((r.equity[0] - 0.812416).abs() < 1e-6);
}

#[test]
fn double_board_scoring() {
    let mut e = Engine::new(2);
    // nuts on both boards = whole pot
    let r = e
        .equity(&[Player::Fixed(hand("AhKhQd2c3c")), Player::Fixed(hand("4s4d7c8c9d"))], &ids("JhTh2h5s6d"), Some(&ids("QhJsTd5d6s")), &[], 0, 1000, 1)
        .unwrap();
    assert_eq!(r.equity[0], 1.0);
    assert_eq!(r.win[0], 1.0);
    // turn/turn enumerates every ordered pair of rivers
    let r = e
        .equity(&[Player::Fixed(hand("AhAdKcKd7c")), Player::Fixed(hand("9s8s7d6h5c"))], &ids("QhJc2s3d"), Some(&ids("Tc9d4h5s")), &[], 0, 1_000_000, 1)
        .unwrap();
    assert!(r.exact);
    assert_eq!(r.samples, 34 * 33);
}

#[test]
fn errors() {
    let mut e = Engine::new(1);
    let two = [Player::Fixed(hand("AhAdKcKd7c")), Player::Random];
    assert_eq!(e.equity(&two[..1], &[], None, &[], 10, 0, 1).unwrap_err(), Error::Players);
    assert_eq!(e.equity(&two, &ids("QhJc"), None, &[], 10, 0, 1).unwrap_err(), Error::Board);
    assert_eq!(e.equity(&two, &ids("AhJc2s"), None, &[], 10, 0, 1).unwrap_err(), Error::Dup);
    assert_eq!(e.equity(&two, &[], None, &[], 0, 0, 1).unwrap_err(), Error::Trials);
    let range = [Player::Fixed(hand("AhAdKcKd7c")), Player::Range { lo: 10.0, hi: 60.0, chain: vec![] }];
    assert_eq!(e.equity(&range, &[], None, &[], 10, 0, 1).unwrap_err(), Error::Ranks);
}

#[test]
fn board_ranking_orders_river_hands() {
    let mut e = Engine::new(2);
    let b = ids("Kh7d2s9c3h");
    e.board_ranks_build(&b, 100, &mut |_, _| {}).unwrap();
    let nuts = e.hand_percentile_on(&hand("KcKd7c7s2c"), &b).unwrap(); // top set (ties with every other KK hand)
    let air = e.hand_percentile_on(&hand("4c5c6d8dQs"), &b).unwrap();
    assert!(nuts > 95.0 && air < 30.0, "{nuts} {air}");
    let h50 = e.percentile_hand_on(50.0, &b).unwrap();
    let back = e.hand_percentile_on(&h50, &b).unwrap();
    assert!((back - 50.0).abs() < 1.0);
    assert!(e.hand_percentile_on(&hand("KhKd7c7s2c"), &b).is_none(), "uses a board card");
}

#[test]
fn cancellation_stops_long_runs() {
    let mut e = Engine::new(2);
    let flag = e.cancel_flag();
    let t = std::thread::spawn(move || {
        std::thread::sleep(std::time::Duration::from_millis(100));
        flag.store(true, Ordering::Relaxed);
    });
    let start = std::time::Instant::now();
    let r = e.equity(&[Player::Random, Player::Random, Player::Random], &[], None, &[], 4_000_000_000, 0, 1);
    t.join().unwrap();
    assert_eq!(r.unwrap_err(), Error::Cancelled);
    assert!(start.elapsed().as_secs_f64() < 3.0);
    e.cancel_flag().store(false, Ordering::Relaxed);
    assert!(e.equity(&[Player::Random, Player::Random], &[], None, &[], 1000, 0, 1).is_ok());
}

#[test]
fn api_shapes() {
    let mut e = Engine::new(2);
    let p = Presets::default();
    let mut none = |_: &str, _: u32, _: u32| {};
    let (st, body) = api::call(&mut e, &p, "/api/status", "", &mut none).unwrap();
    assert_eq!(st, 200);
    assert!(body.starts_with("{\"ranks\":false,\"ncpu\":2,\"categories\":[\"High Card\""), "{body}");

    let mut seen = Vec::new();
    let mut prog = |w: &str, d: u32, t: u32| seen.push((w.to_string(), d, t));
    let (st, body) = api::call(&mut e, &p, "/api/equity", "players=AhAdKcKd7c,9s8s7d6h5c&board=Qh+Jc+2s&buildranks=1", &mut prog).unwrap();
    assert_eq!(st, 200);
    assert!(body.contains("\"samples\":741,\"exact\":true"), "{body}");
    assert!(body.contains("\"equity\":[81.241565,18.758435]"), "{body}");
    assert!(body.contains("\"pct\":["), "{body}");
    assert_eq!(seen.last().map(|s| (s.0.as_str(), s.1, s.2)), Some(("flop ranking", 100, 100)));

    let (st, body) = api::call(&mut e, &p, "/api/equity", "players=%22bad", &mut none).unwrap();
    assert_eq!((st, body.as_str()), (400, "{\"error\":\"cannot parse a player spec\"}"));
    let (st, _) = api::call(&mut e, &p, "/api/nope", "", &mut none).unwrap();
    assert_eq!(st, 404);
    let (st, body) = api::call(&mut e, &p, "/api/random", "street=3&count=2&used=AhAd", &mut none).unwrap();
    assert_eq!(st, 200);
    assert!(!body.contains("Ah") && !body.contains("Ad") && body.len() == "{\"board\":\"xxxxxx\",\"board2\":\"xxxxxx\"}".len());
    let (st, body) = api::call(&mut e, &p, "/api/lookup", "pct=99.9&board=Kh7d2s", &mut none).unwrap();
    assert_eq!(st, 200);
    assert!(body.ends_with("\"street\":\"flop\"}"), "{body}");
}

#[test]
fn player_specs() {
    assert_eq!(api::parse_player("random"), Some(Player::Random));
    assert_eq!(api::parse_player("AhAdKcKd7c"), Some(Player::Fixed(hand("AhAdKcKd7c"))));
    assert_eq!(api::parse_player("0-50>0-60>40-100"), Some(Player::Range { lo: 40.0, hi: 100.0, chain: vec![(0.0, 50.0), (0.0, 60.0)] }));
    assert_eq!(api::parse_player("10-60%"), Some(Player::Range { lo: 10.0, hi: 60.0, chain: vec![] }));
    assert_eq!(api::parse_player("60-10"), None);
    assert_eq!(api::parse_player("0-1>0-2>0-3>4-5"), None);
    assert_eq!(api::parse_player("AhAdKc"), None);
}
