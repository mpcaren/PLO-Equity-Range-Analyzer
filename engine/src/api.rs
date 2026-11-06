//! The engine's JSON API — the same endpoints, parameters and response
//! shapes as the C API (src/api.c), so any client of plo5web works with it.
//! A request is an endpoint plus a URL-encoded query string:
//!
//! ```text
//! /api/equity   players=SPEC,SPEC[,..]&board=..&board2=..&double=0/1
//!               &dead=..&trials=N&maxenum=N&seed=N&buildranks=0/1
//! /api/lookup   pct=30&board=..&board2=..        -> hand at that pct
//! /api/rankof   hand=..&board=..&board2=..       -> pct of a hand
//! /api/findeq   target=55&board=..&board2=..&double=0/1
//! /api/random   street=3..5&count=1/2&used=..    -> random board(s)
//! /api/sprquiz  board=..&spr=..&opp=N&offset=PP&pot=..&trials=..
//! /api/status
//! ```
//!
//! A player SPEC is five cards (`AhAdKcKd7c`), `random`, or a percentile
//! band `lo-hi`, optionally chained street by street: `0-50>0-60>40-100`.

use std::fmt::Write as _;

use crate::cards::{cards_str, hand_str_sorted, CATEGORY_NAMES};
use crate::rng::splitmix64;
use crate::{spr, Engine, Error, Player, CHAIN_MAX, MAX_PLAYERS};

/// progress for slow steps: (what, done, total)
pub type ApiProgress<'a> = &'a mut dyn FnMut(&str, u32, u32);

/// Fast / Balanced / Precise trial presets reported by /api/status
#[derive(Clone, Copy, Debug)]
pub struct Presets(pub [u64; 3]);

impl Default for Presets {
    fn default() -> Self {
        Presets([200_000, 1_000_000, 10_000_000])
    }
}

/// Run one request. Returns an HTTP-style status (200, 400 with
/// `{"error":..}`, or 404) and the JSON body. A cancelled request returns
/// `Err(Error::Cancelled)`.
pub fn call(engine: &mut Engine, presets: &Presets, endpoint: &str, query: &str, progress: ApiProgress) -> Result<(u16, String), Error> {
    let r = match endpoint {
        "/api/equity" => api_equity(engine, query, progress),
        "/api/lookup" => api_lookup(engine, query, progress),
        "/api/rankof" => api_rankof(engine, query, progress),
        "/api/findeq" => api_findeq(engine, query, progress),
        "/api/random" => api_random(query),
        "/api/sprquiz" => api_sprquiz(engine, query, progress),
        "/api/status" => Ok(api_status(engine, presets)),
        _ => return Ok((404, r#"{"error":"no such api"}"#.into())),
    };
    match r {
        Ok(body) => Ok((200, body)),
        Err(ApiErr::Engine(Error::Cancelled)) => Err(Error::Cancelled),
        Err(e) => Ok((400, format!("{{\"error\":{}}}", json_str(&e.message())))),
    }
}

enum ApiErr {
    Engine(Error),
    Msg(&'static str),
}

impl From<Error> for ApiErr {
    fn from(e: Error) -> Self {
        ApiErr::Engine(e)
    }
}

impl ApiErr {
    fn message(&self) -> String {
        match self {
            ApiErr::Engine(e) => e.message().to_string(),
            ApiErr::Msg(m) => m.to_string(),
        }
    }
}

type R = Result<String, ApiErr>;

/* ---------------- query parsing ---------------- */

/// url-decoded value of `key` in a query string
pub fn qget(query: &str, key: &str) -> Option<String> {
    for seg in query.split('&') {
        let Some((k, v)) = seg.split_once('=') else { continue };
        if k != key {
            continue;
        }
        let b = v.as_bytes();
        let mut out = Vec::with_capacity(b.len());
        let mut i = 0;
        while i < b.len() {
            match b[i] {
                b'+' => out.push(b' '),
                b'%' if i + 2 < b.len() => {
                    match (hex(b[i + 1]), hex(b[i + 2])) {
                        (Some(h), Some(l)) => {
                            out.push(h * 16 + l);
                            i += 2;
                        }
                        _ => out.push(b'%'),
                    }
                }
                c => out.push(c),
            }
            i += 1;
        }
        return Some(String::from_utf8_lossy(&out).into_owned());
    }
    None
}

fn hex(c: u8) -> Option<u8> {
    (c as char).to_digit(16).map(|d| d as u8)
}

fn qnum(query: &str, key: &str, dflt: f64) -> f64 {
    match qget(query, key) {
        Some(v) if !v.is_empty() => atof(&v),
        _ => dflt,
    }
}

/// C atof: the longest numeric prefix, 0 if there is none
fn atof(s: &str) -> f64 {
    let s = s.trim_start();
    let mut end = s.find(|c: char| !(c.is_ascii_digit() || "+-.eE".contains(c))).unwrap_or(s.len());
    while end > 0 {
        if let Ok(v) = s[..end].parse::<f64>() {
            return v;
        }
        end -= 1;
    }
    0.0
}

fn cards_param(query: &str, key: &str, max: usize) -> Result<Vec<usize>, ApiErr> {
    let v = qget(query, key).unwrap_or_default();
    crate::cards::parse_cards(&v, max).ok_or(ApiErr::Msg("bad card list"))
}

fn parse_band(s: &str) -> Option<(f64, f64)> {
    if !s.as_bytes().first()?.is_ascii_digit() {
        return None;
    }
    let (a, b) = s.split_once('-')?;
    let b = b.strip_suffix('%').unwrap_or(b);
    let (lo, hi): (f64, f64) = (a.parse().ok()?, b.parse().ok()?);
    if lo < 0.0 || hi > 100.0 || hi <= lo {
        return None;
    }
    Some((lo, hi))
}

/// player spec: five cards, `random` / `*`, or `lo-hi[>lo-hi..]`
pub fn parse_player(s: &str) -> Option<Player> {
    if s.eq_ignore_ascii_case("random") || s == "*" {
        return Some(Player::Random);
    }
    if s.as_bytes().first()?.is_ascii_digit() && s.contains('-') {
        let bands: Vec<(f64, f64)> = s.split('>').map(parse_band).collect::<Option<_>>()?;
        if bands.len() > CHAIN_MAX + 1 {
            return None;
        }
        let (&(lo, hi), chain) = bands.split_last()?;
        return Some(Player::Range { lo, hi, chain: chain.to_vec() });
    }
    let c = crate::cards::parse_cards(s, 5)?;
    let c: [usize; 5] = c.try_into().ok()?;
    Some(Player::Fixed(c))
}

/* ---------------- JSON helpers ---------------- */

pub fn json_str(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 2);
    o.push('"');
    for ch in s.chars() {
        match ch {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if (c as u32) < 0x20 => {
                let _ = write!(o, "\\u{:04x}", c as u32);
            }
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

fn arr(v: &[f64], scale: f64) -> String {
    let parts: Vec<String> = v.iter().map(|x| format!("{:.6}", x * scale)).collect();
    format!("[{}]", parts.join(","))
}

fn hand5(c: &[usize; 5]) -> String {
    hand_str_sorted(c)
}

/// a fresh nonzero seed from the clock
pub fn fresh_seed() -> u64 {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0);
    let mut s = nanos ^ ((std::process::id() as u64) << 32);
    let v = splitmix64(&mut s);
    if v == 0 { 0x9E37_79B9_7F4A_7C15 } else { v }
}

fn street_label(n: usize) -> &'static str {
    match n {
        3 => "flop ranking",
        4 => "turn ranking",
        _ => "river ranking",
    }
}

fn build_single(e: &mut Engine, board: &[usize], progress: ApiProgress) -> Result<(), Error> {
    let what = street_label(board.len());
    e.board_ranks_build(board, 100, &mut |d, t| progress(what, d, t))
}

fn build_double(e: &mut Engine, b1: &[usize], b2: &[usize], progress: ApiProgress) -> Result<(), Error> {
    e.board_ranks_build2(b1, b2, 100, &mut |d, t| progress("two-board ranking", d, t))
}

/* ---------------- handlers ---------------- */

fn api_equity(e: &mut Engine, q: &str, progress: ApiProgress) -> R {
    let pspec = qget(q, "players").unwrap_or_default();
    if pspec.is_empty() {
        return Err(ApiErr::Msg("players parameter required"));
    }
    let mut players = Vec::new();
    for tok in pspec.split(',').filter(|t| !t.is_empty()) {
        if players.len() >= MAX_PLAYERS {
            return Err(ApiErr::Msg("too many players"));
        }
        players.push(parse_player(tok).ok_or(ApiErr::Msg("cannot parse a player spec"))?);
    }
    let board = cards_param(q, "board", 5)?;
    let board2 = cards_param(q, "board2", 5)?;
    let dead = cards_param(q, "dead", 40)?;
    let dbl = qnum(q, "double", 0.0) as i64 != 0 || !board2.is_empty();
    let trials = qnum(q, "trials", 1_000_000.0) as u64;
    let maxen = qnum(q, "maxenum", 1_000_000.0) as u64;
    let mut seed = qnum(q, "seed", 0.0) as u64;
    if seed == 0 {
        seed = fresh_seed();
    }

    // optionally (re)build board rankings so percentiles come back filled
    if qnum(q, "buildranks", 0.0) as i64 != 0 {
        let r = if dbl && board.len() >= 3 && board2.len() >= 3 {
            build_double(e, &board, &board2, progress)
        } else if !dbl && board.len() >= 3 {
            build_single(e, &board, progress)
        } else {
            Ok(())
        };
        if r == Err(Error::Cancelled) {
            return Err(Error::Cancelled.into());
        }
    }

    let t0 = std::time::Instant::now();
    let r = e.equity(&players, &board, dbl.then_some(&board2[..]), &dead, trials, maxen, seed)?;
    let ms = t0.elapsed().as_secs_f64() * 1000.0;

    let mut s = format!(
        "{{\"samples\":{},\"exact\":{},\"ms\":{:.1},\"double\":{},",
        r.samples, r.exact, ms, dbl
    );
    let _ = write!(
        s,
        "\"equity\":{},\"win\":{},\"tie\":{},\"ci95\":{}",
        arr(&r.equity, 100.0),
        arr(&r.win, 100.0),
        arr(&r.tie, 100.0),
        arr(&r.ci95, 100.0)
    );
    let ht: Vec<String> = r.hand_type.iter().map(|h| arr(h, 100.0)).collect();
    let _ = write!(s, ",\"handType\":[{}]", ht.join(","));
    if dbl {
        let hb: Vec<String> = r.hand_type_b.iter().map(|h| arr(h, 100.0)).collect();
        let _ = write!(s, ",\"handTypeB\":[{}]", hb.join(","));
    }
    // percentile of each fixed hand, when the matching table is available
    let pct: Vec<String> = players
        .iter()
        .map(|p| {
            let pc = match p {
                Player::Fixed(c) => {
                    if dbl {
                        if board.len() >= 3 && board2.len() >= 3 {
                            e.hand_percentile_2b(c, &board, &board2)
                        } else if board.is_empty() && board2.is_empty() {
                            e.hand_percentile(c)
                        } else {
                            None
                        }
                    } else {
                        e.hand_percentile_on(c, &board)
                    }
                }
                _ => None,
            };
            pc.map(|v| format!("{v:.1}")).unwrap_or_else(|| "null".into())
        })
        .collect();
    let _ = write!(s, ",\"pct\":[{}]}}", pct.join(","));
    Ok(s)
}

fn api_lookup(e: &mut Engine, q: &str, progress: ApiProgress) -> R {
    let board = cards_param(q, "board", 5)?;
    let board2 = cards_param(q, "board2", 5)?;
    let pct = qnum(q, "pct", 50.0);
    let (hand, street) = if board2.len() >= 3 && board.len() >= 3 {
        build_double(e, &board, &board2, progress)?;
        (e.percentile_hand_2b(pct, &board, &board2)?, "both boards")
    } else if board.len() >= 3 {
        build_single(e, &board, progress)?;
        let st = match board.len() {
            3 => "flop",
            4 => "turn",
            _ => "river",
        };
        (e.percentile_hand_on(pct, &board)?, st)
    } else {
        (e.percentile_hand(pct)?, "preflop")
    };
    Ok(format!("{{\"hand\":\"{}\",\"street\":\"{}\"}}", hand5(&hand), street))
}

fn api_rankof(e: &mut Engine, q: &str, progress: ApiProgress) -> R {
    let h = qget(q, "hand").ok_or(ApiErr::Msg("hand required"))?;
    let hand: [usize; 5] = crate::cards::parse_cards(&h, 5)
        .and_then(|v| v.try_into().ok())
        .ok_or(ApiErr::Msg("bad hand"))?;
    let board = cards_param(q, "board", 5)?;
    let board2 = cards_param(q, "board2", 5)?;
    let pc = if board.len() >= 3 && board2.len() >= 3 {
        let r = build_double(e, &board, &board2, progress);
        if r == Err(Error::Cancelled) {
            return Err(Error::Cancelled.into());
        }
        e.hand_percentile_2b(&hand, &board, &board2)
    } else if board.len() >= 3 {
        let r = build_single(e, &board, progress);
        if r == Err(Error::Cancelled) {
            return Err(Error::Cancelled.into());
        }
        e.hand_percentile_on(&hand, &board)
    } else {
        e.hand_percentile(&hand)
    };
    let pc = pc.ok_or(ApiErr::Msg("percentile unavailable (table missing or card conflict)"))?;
    Ok(format!("{{\"pct\":{pc:.2}}}"))
}

/// binary-search the strength distribution for a hand with the target
/// equity vs one random opponent
fn api_findeq(e: &mut Engine, q: &str, progress: ApiProgress) -> R {
    let board = cards_param(q, "board", 5)?;
    let board2 = cards_param(q, "board2", 5)?;
    let target = qnum(q, "target", 55.0) / 100.0;
    let dbl = qnum(q, "double", 0.0) as i64 != 0 || !board2.is_empty();

    if dbl && board.len() >= 3 && board2.len() >= 3 {
        build_double(e, &board, &board2, progress)?;
    } else if !dbl && board.len() >= 3 {
        build_single(e, &board, progress)?;
    } else if !e.ranks_loaded() {
        return Err(Error::Ranks.into());
    }

    let mut hand = [0usize; 5];
    let (mut eq, mut lo, mut hi) = (-1.0, 0.0, 100.0);
    for it in 0..12u64 {
        let mid = (lo + hi) * 0.5;
        hand = if dbl && board.len() >= 3 {
            e.percentile_hand_2b(mid, &board, &board2)?
        } else {
            e.percentile_hand_on(mid, if board.len() >= 3 { &board } else { &[] })?
        };
        let players = [Player::Fixed(hand), Player::Random];
        let r = e.equity(&players, &board, dbl.then_some(&board2[..]), &[], 200_000, 0, 777 + it)?;
        eq = r.equity[0];
        progress("searching", it as u32 + 1, 12);
        if (eq - target).abs() < 0.004 {
            break;
        }
        if eq < target { lo = mid } else { hi = mid }
    }
    Ok(format!("{{\"hand\":\"{}\",\"equity\":{:.2}}}", hand5(&hand), eq * 100.0))
}

fn api_random(q: &str) -> R {
    let street = qnum(q, "street", 3.0) as i64;
    let count = qnum(q, "count", 1.0) as i64;
    if !(3..=5).contains(&street) || !(1..=2).contains(&count) {
        return Err(ApiErr::Msg("bad street/count"));
    }
    let used = crate::cards::parse_cards(&qget(q, "used").unwrap_or_default(), 52).ok_or(ApiErr::Msg("bad used list"))?;
    let mask = used.iter().fold(0u64, |m, &c| m | 1u64 << c);
    let mut pool: Vec<usize> = (0..52).filter(|&c| mask >> c & 1 == 0).collect();
    let (street, count) = (street as usize, count as usize);
    let need = street * count;
    if pool.len() < need {
        return Err(ApiErr::Msg("not enough cards"));
    }
    let mut s = fresh_seed();
    let n = pool.len();
    for i in 0..need {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17; // xorshift64
        let j = i + (s % (n - i) as u64) as usize;
        pool.swap(i, j);
    }
    let b1 = cards_str(&pool[..street]);
    let b2 = if count == 2 { cards_str(&pool[street..2 * street]) } else { String::new() };
    Ok(format!("{{\"board\":\"{b1}\",\"board2\":\"{b2}\"}}"))
}

/// deal a hero hand whose equity vs `opp` MDF-trimmed stack-off ranges
/// sits `offset` points from the no-fold-equity threshold
fn api_sprquiz(e: &mut Engine, q: &str, progress: ApiProgress) -> R {
    let board = cards_param(q, "board", 5)?;
    if board.len() < 3 {
        return Err(ApiErr::Msg("board (3-5 cards) required"));
    }
    let sprv = qnum(q, "spr", 5.0);
    let nopp = qnum(q, "opp", 1.0) as i64;
    let offset = qnum(q, "offset", 5.0) / 100.0;
    let pot = qnum(q, "pot", 10.0);
    let trials = qnum(q, "trials", 60_000.0) as u64;
    if sprv <= 0.0 || nopp < 1 || nopp > MAX_PLAYERS as i64 - 1 || pot <= 0.0 {
        return Err(ApiErr::Msg("bad spr/opp/pot"));
    }
    let nopp = nopp as usize;
    build_single(e, &board, progress)?;

    let eqn = spr::eq_needed(sprv, nopp + 1);
    let mdf = spr::mdf(sprv);
    let (mut tlo, thi) = spr::mdf_trim_band(0.0, 100.0, mdf);
    let target = (eqn + offset).clamp(0.03, 0.95);

    // keep the best hand seen; widen the band floor when nopp disjoint
    // tight-band hands cannot be dealt
    let mut widened = false;
    let (mut lo, mut hi, mut best_gap) = (0.0, 100.0, 1e9);
    let mut best = [0usize; 5];
    let eval = |e: &mut Engine, hand: [usize; 5], tlo: &mut f64, widened: &mut bool, trials: u64, seed: u64| loop {
        match e.spr_row_eval(hand, &board, *tlo, thi, nopp, pot, sprv, trials, seed) {
            Err(Error::Range) if *tlo > 1e-9 => {
                *tlo = (*tlo - 5.0).max(0.0);
                *widened = true;
            }
            r => return r,
        }
    };
    for it in 0..12u64 {
        let mid = (lo + hi) * 0.5;
        let hand = e.percentile_hand_on(mid, &board)?;
        let row = eval(e, hand, &mut tlo, &mut widened, trials, 777 + it)?;
        let gap = (row.hero_eq - target).abs();
        if gap < best_gap {
            best_gap = gap;
            best = hand;
        }
        progress("searching", it as u32 + 1, 12);
        if gap < 0.004 {
            break;
        }
        if row.hero_eq < target { lo = mid } else { hi = mid }
    }
    // confirm the final hand with more trials
    let row = eval(e, best, &mut tlo, &mut widened, 250_000, 424_242)?;

    let mut s = format!(
        "{{\"hand\":\"{}\",\"board\":\"{}\",\"spr\":{:.2},\"opp\":{},\"pot\":{:.2},\"stack\":{:.2},\"potFinal\":{:.2},\
         \"equity\":{:.2},\"eqNeeded\":{:.2},\"mdf\":{:.2},\"bandLo\":{:.1},\"bandHi\":{:.1},\"bandWidened\":{},",
        hand5(&best),
        cards_str(&board),
        sprv,
        nopp,
        pot,
        row.stack,
        row.pot_final,
        row.hero_eq * 100.0,
        row.eq_needed * 100.0,
        mdf * 100.0,
        tlo,
        thi,
        widened
    );
    if row.profitable_no_fold {
        s.push_str("\"breakevenFold\":null}");
    } else {
        let _ = write!(s, "\"breakevenFold\":{:.1}}}", row.breakeven_fold * 100.0);
    }
    Ok(s)
}

fn api_status(e: &Engine, presets: &Presets) -> String {
    const NAMES: [&str; 3] = ["Fast", "Balanced", "Precise"];
    let cats: Vec<String> = CATEGORY_NAMES.iter().map(|c| json_str(c)).collect();
    let ps: Vec<String> = (0..3)
        .map(|i| format!("{{\"name\":\"{}\",\"trials\":{},\"maxenum\":{}}}", NAMES[i], presets.0[i], presets.0[i] / 5))
        .collect();
    format!(
        "{{\"ranks\":{},\"ncpu\":{},\"categories\":[{}],\"presets\":[{}]}}",
        e.ranks_loaded(),
        e.threads,
        cats.join(","),
        ps.join(",")
    )
}
