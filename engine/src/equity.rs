//! Equity: Monte Carlo and exact enumeration, single or double board,
//! with fixed, random and range (percentile band) players.
//!
//! The work split, per-thread seeds and sampling order are the same as in
//! the C engine, so a seeded run returns bit-identical numbers.

use std::sync::atomic::Ordering;

use crate::eval::{choose64, make_pairs, make_trips, tables, Combo, NCAT};
use crate::rng::{splitmix64, Rng};
use crate::{take_card, Engine, Error, Result, MAX_PLAYERS};

#[derive(Clone, Debug, PartialEq)]
pub enum Player {
    /// five known cards
    Fixed([usize; 5]),
    /// a random hand every trial
    Random,
    /// percentile band `lo..hi` (0 weakest, 100 strongest) of the
    /// distribution for the board in play; `chain` optionally narrows it
    /// street by street: (lo, hi) of the flop, then the turn, survivors
    Range { lo: f64, hi: f64, chain: Vec<(f64, f64)> },
}

#[derive(Clone, Debug, Default)]
pub struct EquityResult {
    /// pot share per player, 0..1 (sums to 1)
    pub equity: Vec<f64>,
    /// fraction won outright (double board: clean scoop)
    pub win: Vec<f64>,
    /// fraction tied / split
    pub tie: Vec<f64>,
    /// 95% confidence half-width on equity (0 when exact)
    pub ci95: Vec<f64>,
    /// trials run, or runouts enumerated
    pub samples: u64,
    pub exact: bool,
    /// per player, fraction of showdowns ending in each made-hand category
    pub hand_type: Vec<[f64; NCAT]>,
    /// board B, double-board mode only
    pub hand_type_b: Vec<[f64; NCAT]>,
}

#[derive(Clone, Copy, PartialEq)]
enum PType {
    Fixed,
    Random,
    Range,
}

struct Setup<'a> {
    np: usize,
    nboard: usize,
    bkey: [u32; 5],
    db: bool,
    nboard2: usize,
    bkey2: [u32; 5],
    ppairs: [[Combo; 10]; MAX_PLAYERS],
    ptype: [PType; MAX_PLAYERS],
    rlo: [u32; MAX_PLAYERS],
    rhi: [u32; MAX_PLAYERS],
    rtab: [&'a [[u8; 5]]; MAX_PLAYERS],
    nrand: usize,
    nrange: usize,
    avail: Vec<usize>,
    base_used: u64,
}

#[derive(Clone)]
struct Acc {
    trials: u64,
    sum: [f64; MAX_PLAYERS],
    sum2: [f64; MAX_PLAYERS],
    nwin: [u64; MAX_PLAYERS],
    ntie: [u64; MAX_PLAYERS],
    ht: [[u64; NCAT]; MAX_PLAYERS],
    ht_b: [[u64; NCAT]; MAX_PLAYERS],
    failed: bool,
    cancelled: bool,
}

impl Acc {
    fn new(trials: u64) -> Acc {
        Acc {
            trials,
            sum: [0.0; MAX_PLAYERS],
            sum2: [0.0; MAX_PLAYERS],
            nwin: [0; MAX_PLAYERS],
            ntie: [0; MAX_PLAYERS],
            ht: [[0; NCAT]; MAX_PLAYERS],
            ht_b: [[0; NCAT]; MAX_PLAYERS],
            failed: false,
            cancelled: false,
        }
    }
}

impl Setup<'_> {
    #[inline]
    fn hand<'b>(&'b self, p: usize, rp: &'b [[Combo; 10]; MAX_PLAYERS]) -> &'b [Combo; 10] {
        if self.ptype[p] == PType::Fixed { &self.ppairs[p] } else { &rp[p] }
    }

    fn score(&self, bkey: &[u32; 5], rp: &[[Combo; 10]; MAX_PLAYERS], w: &mut Acc) {
        let t = tables();
        let bt = make_trips(bkey);
        let mut bv = [0u32; MAX_PLAYERS];
        let (mut best, mut cnt) = (0u32, 0u32);
        for p in 0..self.np {
            let v = t.best_val(self.hand(p, rp), &bt);
            bv[p] = v;
            w.ht[p][(v >> 24) as usize] += 1;
            if v > best {
                best = v;
                cnt = 1;
            } else if v == best {
                cnt += 1;
            }
        }
        let share = 1.0 / cnt as f64;
        for p in 0..self.np {
            if bv[p] == best {
                w.sum[p] += share;
                w.sum2[p] += share * share;
                if cnt == 1 { w.nwin[p] += 1 } else { w.ntie[p] += 1 }
            }
        }
    }

    /// double board: each board pays half the pot to its best hand(s)
    fn score_db(&self, bka: &[u32; 5], bkb: &[u32; 5], rp: &[[Combo; 10]; MAX_PLAYERS], w: &mut Acc) {
        let t = tables();
        let (bta, btb) = (make_trips(bka), make_trips(bkb));
        let mut va = [0u32; MAX_PLAYERS];
        let mut vb = [0u32; MAX_PLAYERS];
        let (mut besta, mut bestb, mut ca, mut cb) = (0u32, 0u32, 0u32, 0u32);
        for p in 0..self.np {
            let hp = self.hand(p, rp);
            va[p] = t.best_val(hp, &bta);
            vb[p] = t.best_val(hp, &btb);
            w.ht[p][(va[p] >> 24) as usize] += 1;
            w.ht_b[p][(vb[p] >> 24) as usize] += 1;
            if va[p] > besta {
                besta = va[p];
                ca = 1;
            } else if va[p] == besta {
                ca += 1;
            }
            if vb[p] > bestb {
                bestb = vb[p];
                cb = 1;
            } else if vb[p] == bestb {
                cb += 1;
            }
        }
        for p in 0..self.np {
            let (ina, inb) = (va[p] == besta, vb[p] == bestb);
            if !ina && !inb {
                continue;
            }
            let share = (if ina { 0.5 / ca as f64 } else { 0.0 }) + (if inb { 0.5 / cb as f64 } else { 0.0 });
            w.sum[p] += share;
            w.sum2[p] += share * share;
            if ina && inb && ca == 1 && cb == 1 { w.nwin[p] += 1 } else { w.ntie[p] += 1 }
        }
    }

    fn run_mc(&self, w: &mut Acc, seed: u64, cancel: &std::sync::atomic::AtomicBool) {
        let t = tables();
        let mut rng = Rng::new(seed);
        let mut deck = self.avail.clone();
        let n = deck.len();
        let need = 5 - self.nboard;
        let need2 = if self.db { 5 - self.nboard2 } else { 0 };
        let ndraw = need + need2 + 5 * self.nrand;
        let mut bkey = self.bkey;
        let mut bkey2 = self.bkey2;
        let mut rp = [[Combo::default(); 10]; MAX_PLAYERS];

        for tr in 0..w.trials {
            if tr & 4095 == 0 && cancel.load(Ordering::Relaxed) {
                w.cancelled = true;
                return;
            }
            if self.nrange == 0 {
                // partial Fisher-Yates draw
                for i in 0..ndraw {
                    let j = i + rng.below((n - i) as u32) as usize;
                    deck.swap(i, j);
                }
                for i in 0..need {
                    bkey[self.nboard + i] = t.card_key[deck[i]];
                }
                for i in 0..need2 {
                    bkey2[self.nboard2 + i] = t.card_key[deck[need + i]];
                }
                let mut di = need + need2;
                for p in 0..self.np {
                    if self.ptype[p] == PType::Random {
                        let mut hk = [0u32; 5];
                        for k in &mut hk {
                            *k = t.card_key[deck[di]];
                            di += 1;
                        }
                        rp[p] = make_pairs(&hk);
                    }
                }
            } else {
                // range players: rejection-sample a combo from the table
                // slice, then fill boards and random hands by rejection
                let mut used = self.base_used;
                for p in 0..self.np {
                    if self.ptype[p] != PType::Range {
                        continue;
                    }
                    let span = self.rhi[p] - self.rlo[p];
                    let mut tries = 0;
                    loop {
                        let ix = self.rlo[p] + rng.below(span);
                        let cd = &self.rtab[p][ix as usize];
                        let m = cd.iter().fold(0u64, |m, &c| m | 1u64 << c);
                        if m & used == 0 {
                            used |= m;
                            rp[p] = t.pairs(cd);
                            break;
                        }
                        tries += 1;
                        if tries >= 100_000 {
                            w.failed = true;
                            return;
                        }
                    }
                }
                let draw = |rng: &mut Rng, used: &mut u64| loop {
                    let cx = deck[rng.below(n as u32) as usize];
                    if *used >> cx & 1 == 0 {
                        *used |= 1u64 << cx;
                        return cx;
                    }
                };
                for i in 0..need {
                    bkey[self.nboard + i] = t.card_key[draw(&mut rng, &mut used)];
                }
                for i in 0..need2 {
                    bkey2[self.nboard2 + i] = t.card_key[draw(&mut rng, &mut used)];
                }
                for p in 0..self.np {
                    if self.ptype[p] != PType::Random {
                        continue;
                    }
                    let mut hk = [0u32; 5];
                    for k in &mut hk {
                        *k = t.card_key[draw(&mut rng, &mut used)];
                    }
                    rp[p] = make_pairs(&hk);
                }
            }
            if self.db {
                self.score_db(&bkey, &bkey2, &rp, w);
            } else {
                self.score(&bkey, &rp, w);
            }
        }
    }

    fn run_exact(&self, w: &mut Acc, cancel: &std::sync::atomic::AtomicBool) {
        let t = tables();
        let k = 5 - self.nboard;
        let n = self.avail.len();
        let mut bkey = self.bkey;
        let dummy = [[Combo::default(); 10]; MAX_PLAYERS];
        if k == 0 {
            self.score(&bkey, &dummy, w);
            w.trials = 1;
            return;
        }
        let mut idx: Vec<usize> = (0..k).collect();
        let mut count = 0u64;
        loop {
            if count & 4095 == 0 && cancel.load(Ordering::Relaxed) {
                w.cancelled = true;
                return;
            }
            for i in 0..k {
                bkey[self.nboard + i] = t.card_key[self.avail[idx[i]]];
            }
            self.score(&bkey, &dummy, w);
            count += 1;
            if !comb_next(&mut idx, n) {
                break;
            }
        }
        w.trials = count;
    }

    fn run_exact_db(&self, w: &mut Acc, cancel: &std::sync::atomic::AtomicBool) {
        let t = tables();
        let (k1, k2, n) = (5 - self.nboard, 5 - self.nboard2, self.avail.len());
        let mut bka = self.bkey;
        let mut bkb = self.bkey2;
        let dummy = [[Combo::default(); 10]; MAX_PLAYERS];
        let mut count = 0u64;
        let mut i1: Vec<usize> = (0..k1).collect();
        loop {
            if cancel.load(Ordering::Relaxed) {
                w.cancelled = true;
                return;
            }
            for i in 0..k1 {
                bka[self.nboard + i] = t.card_key[self.avail[i1[i]]];
            }
            // remaining deck for board B
            let mut rem = Vec::with_capacity(n);
            let mut s = 0;
            for i in 0..n {
                if s < k1 && i1[s] == i {
                    s += 1;
                    continue;
                }
                rem.push(self.avail[i]);
            }
            let mut i2: Vec<usize> = (0..k2).collect();
            loop {
                for i in 0..k2 {
                    bkb[self.nboard2 + i] = t.card_key[rem[i2[i]]];
                }
                self.score_db(&bka, &bkb, &dummy, w);
                count += 1;
                if !(k2 > 0 && comb_next(&mut i2, rem.len())) {
                    break;
                }
            }
            if !(k1 > 0 && comb_next(&mut i1, n)) {
                break;
            }
        }
        w.trials = count;
    }
}

/// advance a k-combination of [0, n); false when exhausted
fn comb_next(idx: &mut [usize], n: usize) -> bool {
    let k = idx.len();
    let mut i = k as isize - 1;
    while i >= 0 && idx[i as usize] == n - k + i as usize {
        i -= 1;
    }
    if i < 0 {
        return false;
    }
    let i = i as usize;
    idx[i] += 1;
    for j in i + 1..k {
        idx[j] = idx[j - 1] + 1;
    }
    true
}

impl Engine {
    /// Equity on one board (`board2` = None) or split between two boards
    /// (`board2` = Some, double-board mode; pass Some(&[]) for two empty
    /// boards). Exact enumeration is used when every player is fixed and
    /// the runout count is at most `max_enum`; otherwise `trials` Monte
    /// Carlo trials (`seed` makes runs reproducible).
    #[allow(clippy::too_many_arguments)]
    pub fn equity(
        &mut self,
        players: &[Player],
        board: &[usize],
        board2: Option<&[usize]>,
        dead: &[usize],
        trials: u64,
        max_enum: u64,
        seed: u64,
    ) -> Result<EquityResult> {
        let np = players.len();
        if !(2..=MAX_PLAYERS).contains(&np) {
            return Err(Error::Players);
        }
        let db = board2.is_some();
        let board2 = board2.unwrap_or(&[]);
        if !matches!(board.len(), 0 | 3 | 4 | 5) || (db && !matches!(board2.len(), 0 | 3 | 4 | 5)) {
            return Err(Error::Board);
        }
        let t = tables();

        let mut used = 0u64;
        let mut ptype = [PType::Random; MAX_PLAYERS];
        let mut ppairs = [[Combo::default(); 10]; MAX_PLAYERS];
        let mut band = [(0f64, 0f64); MAX_PLAYERS];
        let (mut nrand, mut nrange) = (0, 0);
        for (p, pl) in players.iter().enumerate() {
            match pl {
                Player::Fixed(c) => {
                    ptype[p] = PType::Fixed;
                    let mut hk = [0u32; 5];
                    for i in 0..5 {
                        take_card(c[i], &mut used)?;
                        hk[i] = t.card_key[c[i]];
                    }
                    ppairs[p] = make_pairs(&hk);
                }
                Player::Random => {
                    ptype[p] = PType::Random;
                    nrand += 1;
                }
                Player::Range { lo, hi, chain } => {
                    let (lo, hi) = (lo.max(0.0), hi.min(100.0));
                    if hi <= lo || chain.len() > crate::CHAIN_MAX {
                        return Err(Error::Arg);
                    }
                    ptype[p] = PType::Range;
                    band[p] = (lo, hi);
                    nrange += 1;
                }
            }
        }
        let mut bkey = [0u32; 5];
        for (i, &c) in board.iter().enumerate() {
            take_card(c, &mut used)?;
            bkey[i] = t.card_key[c];
        }
        let mut bkey2 = [0u32; 5];
        for (i, &c) in board2.iter().enumerate() {
            take_card(c, &mut used)?;
            bkey2[i] = t.card_key[c];
        }
        for &c in dead {
            take_card(c, &mut used)?;
        }
        let avail: Vec<usize> = (0..52).filter(|&c| used >> c & 1 == 0).collect();

        // range players: the static preflop table with no board, else the
        // board-conditional table (built on demand and cached); double
        // board needs both boards ranked (or neither). A chained player
        // (single board only) gets its own narrowed ranking.
        let is_chained = |p: usize| matches!(&players[p], Player::Range { chain, .. } if !db && !chain.is_empty() && board.len() >= 3);
        // 0 preflop, 1 board, 2 two-board, 3 the player's chain
        let mut psrc = [0u8; MAX_PLAYERS];
        if nrange > 0 {
            let need_shared = (0..np).any(|p| ptype[p] == PType::Range && !is_chained(p));
            let mut shared = 0u8;
            if need_shared {
                if db {
                    if board.is_empty() && board2.is_empty() {
                        if !self.ranks_loaded() {
                            return Err(Error::Ranks);
                        }
                    } else if board.len() >= 3 && board2.len() >= 3 {
                        self.board_ranks_build2(board, board2, 0, &mut |_, _| {})?;
                        shared = 2;
                    } else {
                        return Err(Error::Ranks);
                    }
                } else if !board.is_empty() {
                    self.board_ranks_build(board, 0, &mut |_, _| {})?;
                    shared = 1;
                } else if !self.ranks_loaded() {
                    return Err(Error::Ranks);
                }
            }
            for p in 0..np {
                if ptype[p] != PType::Range {
                    continue;
                }
                if is_chained(p) {
                    let Player::Range { chain, .. } = &players[p] else { unreachable!() };
                    let lo: Vec<f64> = chain.iter().map(|c| c.0).collect();
                    let hi: Vec<f64> = chain.iter().map(|c| c.1).collect();
                    self.chain_rank_build(p, board, &lo, &hi, 0, &mut |_, _| {})?;
                    psrc[p] = 3;
                } else {
                    psrc[p] = shared;
                }
            }
        }
        if self.cancelled() {
            return Err(Error::Cancelled);
        }

        let mut rtab: [&[[u8; 5]]; MAX_PLAYERS] = [&[]; MAX_PLAYERS];
        let mut rlo = [0u32; MAX_PLAYERS];
        let mut rhi = [0u32; MAX_PLAYERS];
        for p in 0..np {
            if ptype[p] != PType::Range {
                continue;
            }
            let list = match psrc[p] {
                3 => self.chain_table(p),
                2 => self.board_table2(board, board2),
                1 => self.board_table(board),
                _ => self.ranks(),
            }
            .ok_or(Error::Ranks)?;
            let n = list.len();
            rtab[p] = &list.cards;
            rlo[p] = (band[p].0 / 100.0 * n as f64) as u32;
            rhi[p] = ((band[p].1 / 100.0 * n as f64) as u32).min(n);
            if rhi[p] <= rlo[p] {
                rhi[p] = rlo[p] + 1;
            }
        }

        let need = 5 - board.len();
        let need2 = if db { 5 - board2.len() } else { 0 };
        if avail.len() < need + need2 + 5 * (nrand + nrange) {
            return Err(Error::Deck);
        }
        let st = Setup {
            np,
            nboard: board.len(),
            bkey,
            db,
            nboard2: if db { board2.len() } else { 0 },
            bkey2,
            ppairs,
            ptype,
            rlo,
            rhi,
            rtab,
            nrand,
            nrange,
            avail,
            base_used: used,
        };
        let cancel = &*self.cancel;

        let combos = choose64(st.avail.len() as i64, need as i64) * choose64((st.avail.len() - need) as i64, need2 as i64);
        if nrand == 0 && nrange == 0 && combos > 0 && combos <= max_enum {
            let mut w = Acc::new(0);
            if db { st.run_exact_db(&mut w, cancel) } else { st.run_exact(&mut w, cancel) }
            if w.cancelled {
                return Err(Error::Cancelled);
            }
            let n = w.trials as f64;
            return Ok(EquityResult {
                equity: (0..np).map(|p| w.sum[p] / n).collect(),
                win: (0..np).map(|p| w.nwin[p] as f64 / n).collect(),
                tie: (0..np).map(|p| w.ntie[p] as f64 / n).collect(),
                ci95: vec![0.0; np],
                samples: w.trials,
                exact: true,
                hand_type: (0..np).map(|p| w.ht[p].map(|c| c as f64 / n)).collect(),
                hand_type_b: (0..np).map(|p| if db { w.ht_b[p].map(|c| c as f64 / n) } else { [0.0; NCAT] }).collect(),
            });
        }

        if trials == 0 {
            return Err(Error::Trials);
        }
        let nt = (self.threads.clamp(1, 64) as u64).min(trials) as usize;
        let per = trials / nt as u64;
        let rem = trials % nt as u64;
        let mut ws: Vec<(Acc, u64)> = (0..nt)
            .map(|i| {
                let mut s = seed.wrapping_add(i as u64);
                (Acc::new(per + if i == 0 { rem } else { 0 }), splitmix64(&mut s))
            })
            .collect();
        let st_ref = &st;
        if nt == 1 {
            let (w, s) = &mut ws[0];
            st.run_mc(w, *s, cancel);
        } else {
            std::thread::scope(|sc| {
                for (w, s) in ws.iter_mut() {
                    sc.spawn(move || st_ref.run_mc(w, *s, cancel));
                }
            });
        }
        if ws.iter().any(|(w, _)| w.cancelled) {
            return Err(Error::Cancelled);
        }
        if ws.iter().any(|(w, _)| w.failed) {
            return Err(Error::Range);
        }

        let mut tot = Acc::new(0);
        for (w, _) in &ws {
            tot.trials += w.trials;
            for p in 0..np {
                tot.sum[p] += w.sum[p];
                tot.sum2[p] += w.sum2[p];
                tot.nwin[p] += w.nwin[p];
                tot.ntie[p] += w.ntie[p];
                for c in 0..NCAT {
                    tot.ht[p][c] += w.ht[p][c];
                    tot.ht_b[p][c] += w.ht_b[p][c];
                }
            }
        }
        let n = tot.trials as f64;
        let mut out = EquityResult { samples: tot.trials, exact: false, ..Default::default() };
        for p in 0..np {
            let eq = tot.sum[p] / n;
            let var = (tot.sum2[p] / n - eq * eq).max(0.0);
            out.equity.push(eq);
            out.win.push(tot.nwin[p] as f64 / n);
            out.tie.push(tot.ntie[p] as f64 / n);
            out.ci95.push(1.96 * (var / n).sqrt());
            out.hand_type.push(tot.ht[p].map(|c| c as f64 / n));
            out.hand_type_b.push(if db { tot.ht_b[p].map(|c| c as f64 / n) } else { [0.0; NCAT] });
        }
        Ok(out)
    }
}
