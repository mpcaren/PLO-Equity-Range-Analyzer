//! PLO5 engine: 5-card Pot Limit Omaha equity, percentile rankings and
//! stack-off analysis. A Rust port of the C engine in `src/` — same
//! algorithms, same random streams and the same rank-table file, so
//! seeded results match the C tools exactly.
//!
//! Card id = rank*4 + suit (rank 0='2'..12='A'; suit 0=c 1=d 2=h 3=s).
//! Omaha rules: exactly 2 hole cards + exactly 3 board cards, best of the
//! C(5,2) x C(5,3) = 100 combinations.
//!
//! [`Engine`] owns the caches the C library kept in globals (preflop rank
//! table, board-conditional rankings, per-player chain rankings). Methods
//! that can run long accept a progress callback and stop early with
//! [`Error::Cancelled`] once [`Engine::cancel_flag`] is set.

// index loops intentionally mirror the C engine line for line
#![allow(clippy::needless_range_loop)]

pub mod api;
pub mod cards;
pub mod eval;
mod equity;
pub mod rng;
pub mod spr;

use std::fs;
use std::io::Write;
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

pub use equity::{EquityResult, Player};
use eval::{make_trips, sort_small, tables, Combo};
use rng::{splitmix64, Rng};

pub const MAX_PLAYERS: usize = 6;
pub const CHAIN_MAX: usize = 2;
/// number of 5-card holdings, C(52,5)
pub const NCOMBO: usize = 2_598_960;
const RANK_MAGIC: &[u8; 8] = b"PLO5RNK1";
const INVALID: u32 = u32::MAX;

/// Engine errors; the numeric codes match the C engine's PLO5_ERR_*.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Players,
    Board,
    Card,
    Dup,
    Deck,
    Trials,
    Arg,
    Ranks,
    Io,
    Range,
    Cancelled,
}

impl Error {
    pub fn code(self) -> i32 {
        match self {
            Error::Players => -1,
            Error::Board => -2,
            Error::Card => -3,
            Error::Dup => -4,
            Error::Deck => -5,
            Error::Trials => -6,
            Error::Arg => -7,
            Error::Ranks => -8,
            Error::Io => -9,
            Error::Range => -10,
            Error::Cancelled => -11,
        }
    }

    pub fn message(self) -> &'static str {
        match self {
            Error::Players => "need 2..6 players",
            Error::Board => "board must have 0, 3, 4 or 5 cards",
            Error::Card => "invalid card",
            Error::Dup => "duplicate card",
            Error::Deck => "not enough cards left in deck",
            Error::Trials => "no trials and enumeration not possible",
            Error::Ranks => "preflop rank table missing - build it once in Settings (or run plo5setup)",
            Error::Io => "cannot read/write rank file",
            Error::Range => "range cannot be dealt with these dead cards",
            Error::Cancelled => "cancelled",
            Error::Arg => "invalid arguments",
        }
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.message())
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// progress callback: (done, total)
pub type Progress<'a> = &'a mut dyn FnMut(u32, u32);

/// A strength ranking: holdings weakest first, plus a colex-index lookup
/// (u32::MAX for holdings not in this ranking).
pub struct RankList {
    pub cards: Vec<[u8; 5]>,
    pub of_colex: Vec<u32>,
}

impl RankList {
    pub fn len(&self) -> u32 {
        self.cards.len() as u32
    }

    pub fn is_empty(&self) -> bool {
        self.cards.is_empty()
    }

    /// percentile of a hand (0..100, 100 strongest), or None if absent
    pub fn percentile(&self, cards: &[usize; 5]) -> Option<f64> {
        if cards.iter().any(|&c| c > 51) {
            return None;
        }
        let mut c = *cards;
        sort_small(&mut c);
        let r = self.of_colex[tables().colex5(c) as usize];
        if r == INVALID {
            return None;
        }
        Some((r as f64 + 0.5) * 100.0 / self.len() as f64)
    }

    /// the hand at a percentile (clamped to 0..100)
    pub fn hand_at(&self, pct: f64) -> [usize; 5] {
        let pct = pct.clamp(0.0, 100.0);
        let n = self.len();
        let mut i = (pct / 100.0 * n as f64) as u32;
        if i >= n {
            i = n - 1;
        }
        self.cards[i as usize].map(|c| c as usize)
    }
}

struct ChainSlot {
    board: Vec<usize>, // sorted
    lo: Vec<f64>,
    hi: Vec<f64>,
    list: RankList,
}

/// The engine and its ranking caches.
pub struct Engine {
    pub threads: usize,
    preflop: Option<RankList>,
    bt: Option<(Vec<usize>, RankList)>,
    bt2: Option<(Vec<usize>, Vec<usize>, RankList)>,
    chain: [Option<ChainSlot>; MAX_PLAYERS],
    cancel: Arc<AtomicBool>,
}

pub fn available_threads() -> usize {
    std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1)
}

fn sorted(b: &[usize]) -> Vec<usize> {
    let mut v = b.to_vec();
    sort_small(&mut v);
    v
}

pub(crate) fn take_card(c: usize, used: &mut u64) -> Result<()> {
    if c > 51 {
        return Err(Error::Card);
    }
    if *used >> c & 1 != 0 {
        return Err(Error::Dup);
    }
    *used |= 1u64 << c;
    Ok(())
}

fn check_board(board: &[usize], used: &mut u64) -> Result<()> {
    if !matches!(board.len(), 3..=5) {
        return Err(Error::Board);
    }
    for &c in board {
        take_card(c, used)?;
    }
    Ok(())
}

/// run `f(index, &mut item)` over `out`, split across `nt` threads
fn par_each<T: Send>(out: &mut [T], nt: usize, f: &(impl Fn(usize, &mut T) + Sync)) {
    let n = out.len();
    let nt = nt.clamp(1, 64).min(n.max(1));
    if nt == 1 {
        for (i, x) in out.iter_mut().enumerate() {
            f(i, x);
        }
        return;
    }
    let per = n.div_ceil(nt);
    std::thread::scope(|s| {
        for (k, chunk) in out.chunks_mut(per).enumerate() {
            s.spawn(move || {
                let base = k * per;
                for (i, x) in chunk.iter_mut().enumerate() {
                    f(base + i, x);
                }
            });
        }
    });
}

/// run `f(start_index, chunk)` over `out` split into `nt` contiguous chunks
fn par_chunks<T: Send>(out: &mut [T], nt: usize, f: &(impl Fn(usize, &mut [T]) + Sync)) {
    let n = out.len();
    let nt = nt.clamp(1, 64).min(n.max(1));
    if nt == 1 {
        f(0, out);
        return;
    }
    let per = n.div_ceil(nt);
    std::thread::scope(|s| {
        for (k, chunk) in out.chunks_mut(per).enumerate() {
            s.spawn(move || f(k * per, chunk));
        }
    });
}

/// ancestor ranking used to restrict a later street's ranking to the
/// ancestor's survivors (chained ranges)
struct AncFilter<'a> {
    list: &'a RankList,
    lo: f64,
    hi: f64,
}

impl Default for Engine {
    fn default() -> Self {
        Engine::new(0)
    }
}

impl Engine {
    /// `threads` = 0 uses every available core
    pub fn new(threads: usize) -> Engine {
        let _ = tables();
        Engine {
            threads: if threads == 0 { available_threads() } else { threads },
            preflop: None,
            bt: None,
            bt2: None,
            chain: Default::default(),
            cancel: Arc::new(AtomicBool::new(false)),
        }
    }

    /// set to true to make the running (and any later) call return
    /// `Error::Cancelled`; reset it to false before the next job
    pub fn cancel_flag(&self) -> Arc<AtomicBool> {
        self.cancel.clone()
    }

    #[inline]
    pub(crate) fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::Relaxed)
    }

    /* ---------------- preflop rank table ---------------- */

    pub fn ranks_loaded(&self) -> bool {
        self.preflop.is_some()
    }

    pub fn ranks(&self) -> Option<&RankList> {
        self.preflop.as_ref()
    }

    /// load a plo5rank.bin written by this engine or by the C tools
    pub fn ranks_load(&mut self, path: &Path) -> Result<()> {
        let data = fs::read(path).map_err(|_| Error::Io)?;
        let want = 16 + NCOMBO * 5 + NCOMBO * 4;
        if data.len() < want || &data[0..8] != RANK_MAGIC {
            return Err(Error::Io);
        }
        let n = u32::from_le_bytes(data[8..12].try_into().unwrap()) as usize;
        if n != NCOMBO {
            return Err(Error::Io);
        }
        let cards: Vec<[u8; 5]> = data[16..16 + NCOMBO * 5]
            .chunks_exact(5)
            .map(|c| [c[0], c[1], c[2], c[3], c[4]])
            .collect();
        if cards.iter().any(|c| c.iter().any(|&x| x > 51)) {
            return Err(Error::Io);
        }
        let off = 16 + NCOMBO * 5;
        let of_colex: Vec<u32> = data[off..off + NCOMBO * 4]
            .chunks_exact(4)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
            .collect();
        if of_colex.iter().any(|&r| r as usize >= NCOMBO) {
            return Err(Error::Io);
        }
        self.preflop = Some(RankList { cards, of_colex });
        Ok(())
    }

    /// write the loaded table in the shared plo5rank.bin format
    pub fn ranks_save(&self, path: &Path, trials_per_class: u32) -> Result<()> {
        let t = self.preflop.as_ref().ok_or(Error::Ranks)?;
        let mut buf = Vec::with_capacity(16 + NCOMBO * 9);
        buf.extend_from_slice(RANK_MAGIC);
        buf.extend_from_slice(&(NCOMBO as u32).to_le_bytes());
        buf.extend_from_slice(&trials_per_class.to_le_bytes());
        for c in &t.cards {
            buf.extend_from_slice(c);
        }
        for r in &t.of_colex {
            buf.extend_from_slice(&r.to_le_bytes());
        }
        let mut f = fs::File::create(path).map_err(|_| Error::Io)?;
        f.write_all(&buf).map_err(|_| Error::Io)?;
        f.sync_all().map_err(|_| Error::Io)
    }

    /// percentile of a hand in the preflop table (None if not loaded)
    pub fn hand_percentile(&self, cards: &[usize; 5]) -> Option<f64> {
        self.preflop.as_ref()?.percentile(cards)
    }

    pub fn percentile_hand(&self, pct: f64) -> Result<[usize; 5]> {
        Ok(self.preflop.as_ref().ok_or(Error::Ranks)?.hand_at(pct))
    }

    /// Build the preflop table: every starting hand ranked by heads-up
    /// equity vs a random hand (suit-isomorphic classes share a rank).
    /// Identical to the C generator for the same seed and thread count.
    /// Leaves the table loaded; save it with [`Engine::ranks_save`].
    pub fn ranks_generate(&mut self, trials_per_class: u32, seed: u64, progress: Progress) -> Result<()> {
        let t = tables();
        let trials = trials_per_class.max(1000);

        // the 24 suit permutations
        let mut perms = Vec::with_capacity(24);
        for a in 0..4usize {
            for b in 0..4 {
                if b == a {
                    continue;
                }
                for c in 0..4 {
                    if c == a || c == b {
                        continue;
                    }
                    perms.push([a, b, c, 6 - a - b - c]);
                }
            }
        }

        // pass 1: canonicalize every combo into suit-isomorphism classes
        const CHASH_BITS: u32 = 19;
        const CHASH_SIZE: usize = 1 << CHASH_BITS;
        let mut cls_of = vec![0u32; NCOMBO];
        let mut chk = vec![0u64; CHASH_SIZE];
        let mut chv = vec![0u32; CHASH_SIZE];
        let mut rep: Vec<[u8; 5]> = Vec::with_capacity(140_000);
        for_each_combo(|c| {
            let mut best = u64::MAX;
            for p in &perms {
                let mut m = c.map(|x| (x & !3) | p[x & 3]);
                sort_small(&mut m);
                let key = m[0] as u64 | (m[1] as u64) << 6 | (m[2] as u64) << 12 | (m[3] as u64) << 18 | (m[4] as u64) << 24;
                best = best.min(key);
            }
            let mut h = (best.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> (64 - CHASH_BITS)) as usize;
            while chk[h] != 0 && chk[h] != best {
                h = (h + 1) & (CHASH_SIZE - 1);
            }
            if chk[h] == 0 {
                chk[h] = best;
                chv[h] = rep.len() as u32;
                rep.push(c.map(|x| x as u8));
            }
            cls_of[t.colex5(c) as usize] = chv[h];
        });
        drop(chk);
        drop(chv);
        let nc = rep.len();

        // pass 2: MC equity vs random per class, threaded exactly like C
        // (same partition and per-thread seeds, so the same numbers)
        let nt = self.threads.clamp(1, 64).min(nc);
        let mut eq = vec![0f64; nc];
        let done = AtomicUsize::new(0);
        let stop = AtomicBool::new(false);
        let cancel = &self.cancel;
        let rep = &rep;
        std::thread::scope(|s| {
            let per = nc / nt;
            let remn = nc % nt;
            let mut rest: &mut [f64] = &mut eq;
            let mut at = 0usize;
            for i in 0..nt {
                let len = per + usize::from(i < remn);
                let (mine, tail) = rest.split_at_mut(len);
                rest = tail;
                let i0 = at;
                at += len;
                let mut sd = seed.wrapping_add(0x1234).wrapping_add(i as u64);
                let wseed = splitmix64(&mut sd);
                let (done, stop) = (&done, &stop);
                s.spawn(move || {
                    let mut rng = Rng::new(wseed);
                    for (k, out) in mine.iter_mut().enumerate() {
                        if stop.load(Ordering::Relaxed) {
                            return;
                        }
                        *out = gen_eq_vs_random(&rep[i0 + k], trials, &mut rng);
                        done.fetch_add(1, Ordering::Relaxed);
                    }
                });
            }
            while done.load(Ordering::Relaxed) < nc && !stop.load(Ordering::Relaxed) {
                std::thread::sleep(Duration::from_millis(200));
                if cancel.load(Ordering::Relaxed) {
                    stop.store(true, Ordering::Relaxed);
                }
                progress(done.load(Ordering::Relaxed) as u32, nc as u32);
            }
        });
        if stop.load(Ordering::Relaxed) {
            return Err(Error::Cancelled);
        }

        // pass 3: order classes by equity, lay out combos weakest first
        let mut ord: Vec<(f64, u32)> = eq.iter().enumerate().map(|(i, &e)| (e, i as u32)).collect();
        ord.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap().then(a.1.cmp(&b.1)));
        let mut csize = vec![0u32; nc];
        for &c in &cls_of {
            csize[c as usize] += 1;
        }
        let mut off = vec![0u32; nc];
        let mut cum = 0u32;
        for &(_, id) in &ord {
            off[id as usize] = cum;
            cum += csize[id as usize];
        }
        let mut cards = vec![[0u8; 5]; NCOMBO];
        let mut of_colex = vec![0u32; NCOMBO];
        for_each_combo(|c| {
            let cx = t.colex5(c) as usize;
            let slot = &mut off[cls_of[cx] as usize];
            let pos = *slot;
            *slot += 1;
            cards[pos as usize] = c.map(|x| x as u8);
            of_colex[cx] = pos;
        });
        self.preflop = Some(RankList { cards, of_colex });
        progress(nc as u32, nc as u32);
        Ok(())
    }

    /* ---------------- board-conditional rankings ---------------- */

    /// Rank every holding on a flop/turn/river (cached for one board):
    /// river by made-hand strength, turn by mean showdown percentile over
    /// all rivers, flop over `flop_runouts` sampled runouts (default 100).
    pub fn board_ranks_build(&mut self, board: &[usize], flop_runouts: usize, progress: Progress) -> Result<()> {
        check_board(board, &mut 0)?;
        let key = sorted(board);
        if matches!(&self.bt, Some((b, _)) if *b == key) {
            return Ok(());
        }
        let list = self.build_core(board, flop_runouts, progress, None)?;
        self.bt = Some((key, list));
        Ok(())
    }

    fn board_table(&self, board: &[usize]) -> Option<&RankList> {
        match &self.bt {
            Some((b, l)) if !board.is_empty() && *b == sorted(board) => Some(l),
            _ => None,
        }
    }

    /// the hand's percentile on `board` (or preflop with an empty board);
    /// None if the matching table is not built or the hand uses a board card
    pub fn hand_percentile_on(&self, cards: &[usize; 5], board: &[usize]) -> Option<f64> {
        if board.is_empty() {
            return self.hand_percentile(cards);
        }
        self.board_table(board)?.percentile(cards)
    }

    pub fn percentile_hand_on(&self, pct: f64, board: &[usize]) -> Result<[usize; 5]> {
        if board.is_empty() {
            return self.percentile_hand(pct);
        }
        Ok(self.board_table(board).ok_or(Error::Ranks)?.hand_at(pct))
    }

    /// Combined two-board distribution: holdings ranked by the sum of
    /// their percentiles on each board (board order does not matter).
    pub fn board_ranks_build2(&mut self, b1: &[usize], b2: &[usize], flop_runouts: usize, progress: Progress) -> Result<()> {
        let mut used = 0u64;
        check_board(b1, &mut used)?;
        check_board(b2, &mut used)?;
        if self.board_table2(b1, b2).is_some() {
            return Ok(());
        }
        let t = tables();
        let la = self.build_core(b1, flop_runouts, progress, None)?;
        let lb = self.build_core(b2, flop_runouts, progress, None)?;
        let (na, nb) = (la.len() as f64, lb.len() as f64);
        let deck: Vec<usize> = (0..52).filter(|&c| used >> c & 1 == 0).collect();
        let mut hold: Vec<[u8; 5]> = Vec::new();
        let mut bs: Vec<(f32, u32)> = Vec::new();
        for_each_combo_of(&deck, |c5| {
            let cx = t.colex5(c5) as usize;
            let pa = (la.of_colex[cx] as f64 + 0.5) / na;
            let pb = (lb.of_colex[cx] as f64 + 0.5) / nb;
            bs.push(((pa + pb) as f32, hold.len() as u32));
            hold.push(c5.map(|x| x as u8));
        });
        drop(la);
        drop(lb);
        let list = finish_ranking(&hold, bs);
        self.bt2 = Some((sorted(b1), sorted(b2), list));
        Ok(())
    }

    fn board_table2(&self, b1: &[usize], b2: &[usize]) -> Option<&RankList> {
        let (s1, s2) = (sorted(b1), sorted(b2));
        match &self.bt2 {
            Some((x1, x2, l)) if (*x1 == s1 && *x2 == s2) || (*x1 == s2 && *x2 == s1) => Some(l),
            _ => None,
        }
    }

    pub fn hand_percentile_2b(&self, cards: &[usize; 5], b1: &[usize], b2: &[usize]) -> Option<f64> {
        self.board_table2(b1, b2)?.percentile(cards)
    }

    pub fn percentile_hand_2b(&self, pct: f64, b1: &[usize], b2: &[usize]) -> Result<[usize; 5]> {
        Ok(self.board_table2(b1, b2).ok_or(Error::Ranks)?.hand_at(pct))
    }

    /// Ranking for a chained (street-narrowing) range in `slot`: survivors
    /// of the flop (and turn) bands, re-ranked on the current board.
    pub fn chain_rank_build(&mut self, slot: usize, board: &[usize], lo: &[f64], hi: &[f64], flop_runouts: usize, progress: Progress) -> Result<()> {
        if slot >= MAX_PLAYERS || lo.len() != hi.len() || lo.len() > CHAIN_MAX {
            return Err(Error::Arg);
        }
        check_board(board, &mut 0)?;
        let key = sorted(board);
        if let Some(s) = &self.chain[slot] {
            let same = |a: &[f64], b: &[f64]| a.len() == b.len() && a.iter().zip(b).all(|(x, y)| (x - y).abs() <= 1e-9);
            if s.board == key && same(&s.lo, lo) && same(&s.hi, hi) {
                return Ok(());
            }
        }
        const ANC_LEN: [usize; CHAIN_MAX] = [3, 4];
        let mut stages: Vec<(usize, f64, f64)> = Vec::new();
        for i in 0..lo.len() {
            if ANC_LEN[i] >= board.len() {
                break; // not an earlier street here
            }
            stages.push((ANC_LEN[i], lo[i], hi[i]));
        }
        let list = self.chain_core(board, board.len(), &stages, flop_runouts, progress)?;
        self.chain[slot] = Some(ChainSlot { board: key, lo: lo.to_vec(), hi: hi.to_vec(), list });
        Ok(())
    }

    pub fn chain_table(&self, slot: usize) -> Option<&RankList> {
        self.chain.get(slot)?.as_ref().map(|s| &s.list)
    }

    fn chain_core(&self, board: &[usize], nboard: usize, stages: &[(usize, f64, f64)], flop_runouts: usize, progress: Progress) -> Result<RankList> {
        let Some((&(anb, alo, ahi), prev)) = stages.split_last() else {
            return self.build_core(&board[..nboard], flop_runouts, progress, None);
        };
        let parent = self.chain_core(board, anb, prev, flop_runouts, progress)?;
        let anc = AncFilter { list: &parent, lo: alo, hi: ahi };
        self.build_core(&board[..nboard], flop_runouts, progress, Some(&anc))
    }

    /// core builder for one board; restricted to `anc`'s survivors if given
    fn build_core(&self, board: &[usize], flop_runouts: usize, progress: Progress, anc: Option<&AncFilter>) -> Result<RankList> {
        let t = tables();
        let mut used = 0u64;
        check_board(board, &mut used)?;
        let flop_runouts = if flop_runouts == 0 { 100 } else { flop_runouts };
        let deck: Vec<usize> = (0..52).filter(|&c| used >> c & 1 == 0).collect();
        let nd = deck.len();

        let mut hold: Vec<[u8; 5]> = Vec::with_capacity(eval::choose64(nd as i64, 5) as usize);
        for_each_combo_of(&deck, |c| hold.push(c.map(|x| x as u8)));
        let nh = hold.len();

        // runouts: pairs of extra cards (flop), single river (turn), none (river)
        let mut runs: Vec<(Option<usize>, Option<usize>)> = Vec::new();
        match board.len() {
            5 => runs.push((None, None)),
            4 => runs.extend(deck.iter().map(|&c| (Some(c), None))),
            _ => {
                let mut pa = Vec::new();
                let mut pb = Vec::new();
                for i in 0..nd {
                    for j in i + 1..nd {
                        pa.push(deck[i]);
                        pb.push(deck[j]);
                    }
                }
                let np2 = pa.len();
                let mut rng = Rng::new(0xB0A2D5EED);
                for i in 0..np2 - 1 {
                    let j = i + rng.below((np2 - i) as u32) as usize;
                    pa.swap(i, j);
                    pb.swap(i, j);
                }
                let nrun = flop_runouts.min(np2);
                runs.extend((0..nrun).map(|i| (Some(pa[i]), Some(pb[i]))));
            }
        }

        let alive: Option<Vec<bool>> = anc.map(|a| {
            let mut alive = vec![false; nh];
            let n = a.list.len() as f64;
            par_each(&mut alive, self.threads, &|h, out: &mut bool| {
                let r = a.list.of_colex[t.colex5(hold[h].map(|x| x as usize)) as usize];
                if r == INVALID {
                    *out = false;
                } else {
                    let pct = (r as f64 + 0.5) * 100.0 / n;
                    *out = pct >= a.lo && pct < a.hi;
                }
            });
            alive
        });

        let nvals = t.vals.len();
        let mut ord = vec![0u16; nh];
        let mut acc = vec![(0f32, 0u16); nh];
        let mut hist = vec![0u32; nvals];
        let mut below = vec![0u32; nvals];
        let nrun = runs.len() as u32;
        for (r, &(ra, rb)) in runs.iter().enumerate() {
            if self.cancelled() {
                return Err(Error::Cancelled);
            }
            let mut bkey = [0u32; 5];
            for (i, &c) in board.iter().enumerate() {
                bkey[i] = t.card_key[c];
            }
            let mut excl = 0u64;
            for (k, c) in (board.len()..).zip([ra, rb].into_iter().flatten()) {
                bkey[k] = t.card_key[c];
                excl |= 1u64 << c;
            }
            let bt10: [Combo; 10] = make_trips(&bkey);

            let alive_ref = alive.as_deref();
            par_chunks(&mut ord, self.threads, &|base, out: &mut [u16]| {
                vals_chunk(&hold[base..base + out.len()], out, alive_ref.map(|a| &a[base..base + out.len()]), excl, &bt10);
            });

            hist.iter_mut().for_each(|x| *x = 0);
            let mut total = 0u64;
            for &o in &ord {
                if o != 0xFFFF {
                    hist[o as usize] += 1;
                    total += 1;
                }
            }
            let mut cum = 0u32;
            for i in 0..nvals {
                below[i] = cum;
                cum += hist[i];
            }
            let scale = if total > 0 { 100.0 / total as f64 } else { 0.0 };
            let (ord_ref, hist_ref, below_ref) = (&ord, &hist, &below);
            par_each(&mut acc, self.threads, &|h, a: &mut (f32, u16)| {
                let o = ord_ref[h];
                if o == 0xFFFF {
                    return;
                }
                let pct = (below_ref[o as usize] as f64 + 0.5 * hist_ref[o as usize] as f64) * scale;
                a.0 += pct as f32;
                a.1 += 1;
            });
            progress(r as u32 + 1, nrun);
        }

        // holdings that never scored are excluded (did not survive the filter)
        let bs: Vec<(f32, u32)> = acc
            .iter()
            .enumerate()
            .filter(|(_, a)| a.1 > 0)
            .map(|(h, a)| (a.0 / a.1 as f32, h as u32))
            .collect();
        if bs.is_empty() {
            return Err(Error::Range);
        }
        Ok(finish_ranking(&hold, bs))
    }
}

/// one runout's strength ordinal for a run of holdings (0xFFFF = skipped:
/// filtered out by an ancestor, or uses a runout card)
fn vals_chunk(hold: &[[u8; 5]], out: &mut [u16], alive: Option<&[bool]>, excl: u64, bt10: &[Combo; 10]) {
    let t = tables();
    for (i, (cd, o)) in hold.iter().zip(out.iter_mut()).enumerate() {
        if let Some(al) = alive {
            if !al[i] {
                *o = 0xFFFF;
                continue;
            }
        }
        let m = (1u64 << cd[0]) | (1u64 << cd[1]) | (1u64 << cd[2]) | (1u64 << cd[3]) | (1u64 << cd[4]);
        *o = if m & excl != 0 { 0xFFFF } else { t.val_ord(t.best_val(&t.pairs(cd), bt10)) };
    }
}

/// sort (score, holding) weakest first and build the lookup tables
fn finish_ranking(hold: &[[u8; 5]], mut bs: Vec<(f32, u32)>) -> RankList {
    let t = tables();
    bs.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap().then(a.1.cmp(&b.1)));
    let mut of_colex = vec![INVALID; NCOMBO];
    let cards: Vec<[u8; 5]> = bs
        .iter()
        .enumerate()
        .map(|(pos, &(_, h))| {
            let cd = hold[h as usize];
            of_colex[t.colex5(cd.map(|x| x as usize)) as usize] = pos as u32;
            cd
        })
        .collect();
    RankList { cards, of_colex }
}

/// every 5-card holding in ascending card order (the C loop order)
fn for_each_combo(mut f: impl FnMut([usize; 5])) {
    for a in 0..48 {
        for b in a + 1..49 {
            for c in b + 1..50 {
                for d in c + 1..51 {
                    for e in d + 1..52 {
                        f([a, b, c, d, e]);
                    }
                }
            }
        }
    }
}

/// every 5-subset of `deck` (ascending deck), in lexicographic order
fn for_each_combo_of(deck: &[usize], mut f: impl FnMut([usize; 5])) {
    let nd = deck.len();
    if nd < 5 {
        return;
    }
    for a in 0..nd - 4 {
        for b in a + 1..nd - 3 {
            for c in b + 1..nd - 2 {
                for d in c + 1..nd - 1 {
                    for e in d + 1..nd {
                        f([deck[a], deck[b], deck[c], deck[d], deck[e]]);
                    }
                }
            }
        }
    }
}

/// heads-up equity of hand vs one random hand, random board
fn gen_eq_vs_random(hand: &[u8; 5], trials: u32, rng: &mut Rng) -> f64 {
    let t = tables();
    let hp = t.pairs(hand);
    let used = hand.iter().fold(0u64, |m, &c| m | 1u64 << c);
    let mut deck = [0usize; 47];
    let mut nd = 0;
    for c in 0..52 {
        if used >> c & 1 == 0 {
            deck[nd] = c;
            nd += 1;
        }
    }
    let mut sum = 0f64;
    for _ in 0..trials {
        for i in 0..10 {
            let j = i + rng.below((47 - i) as u32) as usize;
            deck.swap(i, j);
        }
        let mut ok = [0u32; 5];
        let mut bk = [0u32; 5];
        for i in 0..5 {
            ok[i] = t.card_key[deck[i]];
            bk[i] = t.card_key[deck[5 + i]];
        }
        let op = eval::make_pairs(&ok);
        let bt = make_trips(&bk);
        let (a, b) = (t.best_val(&hp, &bt), t.best_val(&op, &bt));
        sum += if a > b { 1.0 } else if a == b { 0.5 } else { 0.0 };
    }
    sum / trials as f64
}
