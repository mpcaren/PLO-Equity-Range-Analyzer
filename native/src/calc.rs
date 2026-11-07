//! Calculator state: players, boards, dead cards and the "active slot" the
//! next picked card goes into. A port of desktop/src/lib/calcState.js —
//! same dealing order, ownership rules, validation and request specs.

use serde::{Deserialize, Serialize};

pub const DEAD_SLOTS: usize = 6;
pub const MAX_PLAYERS: usize = 6;

pub type Card = Option<u8>;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, Default)]
pub enum Mode {
    #[default]
    Hand,
    Range,
    Random,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct Band {
    pub lo: f64,
    pub hi: f64,
}

impl Band {
    pub const FULL: Band = Band { lo: 0.0, hi: 100.0 };
    pub fn active(&self) -> bool {
        self.lo > 0.0 || self.hi < 100.0
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Player {
    #[serde(skip)]
    pub id: u32,
    pub mode: Mode,
    pub cards: [Card; 5],
    pub lo: f64,
    pub hi: f64,
    pub chain: [Band; 2],
    #[serde(skip)]
    pub filters_open: bool,
}

/// zone of a card slot
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Zone {
    Player(usize),
    Board,
    Board2,
    Dead,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Slot {
    pub zone: Zone,
    pub i: usize,
}

#[derive(Clone, Debug)]
pub struct Calc {
    pub players: Vec<Player>,
    pub board: [Card; 5],
    pub board2: [Card; 5],
    pub dead: [Card; DEAD_SLOTS],
    pub dbl: bool,
    pub active: Option<Slot>,
    pub sel_p: usize,
    next_id: u32,
}

/// the persisted part
#[derive(Serialize, Deserialize, Default)]
pub struct Saved {
    pub players: Vec<Player>,
    pub board: Vec<Card>,
    pub board2: Vec<Card>,
    pub dead: Vec<Card>,
    pub dbl: bool,
}

impl Default for Calc {
    fn default() -> Self {
        let mut c = Calc {
            players: vec![],
            board: [None; 5],
            board2: [None; 5],
            dead: [None; DEAD_SLOTS],
            dbl: false,
            active: Some(Slot { zone: Zone::Player(0), i: 0 }),
            sel_p: 0,
            next_id: 1,
        };
        c.players = vec![c.new_player(Mode::Hand), c.new_player(Mode::Hand)];
        c
    }
}

impl Calc {
    fn new_player(&mut self, mode: Mode) -> Player {
        let id = self.next_id;
        self.next_id += 1;
        Player { id, mode, cards: [None; 5], lo: 0.0, hi: 100.0, chain: [Band::FULL; 2], filters_open: false }
    }

    /* ---------------- persistence ---------------- */

    pub fn saved(&self) -> Saved {
        Saved {
            players: self.players.clone(),
            board: self.board.to_vec(),
            board2: self.board2.to_vec(),
            dead: self.dead.to_vec(),
            dbl: self.dbl,
        }
    }

    /// restore defensively (the file may come from an older build)
    pub fn revive(saved: Saved) -> Calc {
        let mut c = Calc::default();
        let ok = |x: &Card| x.filter(|&v| v < 52);
        let band = |b: Band| Band { lo: b.lo.clamp(0.0, 100.0), hi: b.hi.clamp(0.0, 100.0) };
        let mut players: Vec<Player> = Vec::new();
        for p in saved.players.into_iter().take(MAX_PLAYERS) {
            let mut np = c.new_player(p.mode);
            for i in 0..5 {
                np.cards[i] = ok(&p.cards[i]);
            }
            np.lo = p.lo.clamp(0.0, 100.0);
            np.hi = p.hi.clamp(0.0, 100.0);
            np.chain = [band(p.chain[0]), band(p.chain[1])];
            players.push(np);
        }
        while players.len() < 2 {
            let np = c.new_player(Mode::Hand);
            players.push(np);
        }
        c.players = players;
        let fill = |v: &[Card], out: &mut [Card]| {
            for (i, o) in out.iter_mut().enumerate() {
                *o = v.get(i).and_then(ok);
            }
        };
        fill(&saved.board, &mut c.board);
        fill(&saved.board2, &mut c.board2);
        fill(&saved.dead, &mut c.dead);
        c.dbl = saved.dbl;
        c.dedupe();
        c.active = c.next_active(None);
        c
    }

    fn dedupe(&mut self) {
        let mut seen = [false; 52];
        let mut keep = |c: &mut Card| {
            if let Some(v) = *c {
                if seen[v as usize] {
                    *c = None;
                } else {
                    seen[v as usize] = true;
                }
            }
        };
        for p in &mut self.players {
            if p.mode == Mode::Hand {
                p.cards.iter_mut().for_each(&mut keep);
            } else {
                p.cards = [None; 5];
            }
        }
        self.board.iter_mut().for_each(&mut keep);
        if self.dbl {
            self.board2.iter_mut().for_each(&mut keep);
        } else {
            self.board2 = [None; 5];
        }
        self.dead.iter_mut().for_each(&mut keep);
    }

    /* ---------------- queries ---------------- */

    pub fn used(&self) -> [bool; 52] {
        let mut u = [false; 52];
        let mut mark = |c: &Card| {
            if let Some(v) = c {
                u[*v as usize] = true;
            }
        };
        for p in &self.players {
            if p.mode == Mode::Hand {
                p.cards.iter().for_each(&mut mark);
            }
        }
        self.board.iter().for_each(&mut mark);
        if self.dbl {
            self.board2.iter().for_each(&mut mark);
        }
        self.dead.iter().for_each(&mut mark);
        u
    }

    /// where a card currently sits
    pub fn find(&self, card: u8) -> Option<Slot> {
        for (p, pl) in self.players.iter().enumerate() {
            if pl.mode == Mode::Hand {
                if let Some(i) = pl.cards.iter().position(|&c| c == Some(card)) {
                    return Some(Slot { zone: Zone::Player(p), i });
                }
            }
        }
        if let Some(i) = self.board.iter().position(|&c| c == Some(card)) {
            return Some(Slot { zone: Zone::Board, i });
        }
        if self.dbl {
            if let Some(i) = self.board2.iter().position(|&c| c == Some(card)) {
                return Some(Slot { zone: Zone::Board2, i });
            }
        }
        self.dead.iter().position(|&c| c == Some(card)).map(|i| Slot { zone: Zone::Dead, i })
    }

    /// the card in a slot: Some(None) = empty slot, None = no such slot
    pub fn get(&self, s: Slot) -> Option<Card> {
        match s.zone {
            Zone::Player(p) => self.players.get(p).filter(|pl| pl.mode == Mode::Hand).map(|pl| pl.cards[s.i]),
            Zone::Board => Some(self.board[s.i]),
            Zone::Board2 => self.dbl.then(|| self.board2[s.i]),
            Zone::Dead => self.dead.get(s.i).copied(),
        }
    }

    fn set(&mut self, s: Slot, c: Card) {
        match s.zone {
            Zone::Player(p) => self.players[p].cards[s.i] = c,
            Zone::Board => self.board[s.i] = c,
            Zone::Board2 => self.board2[s.i] = c,
            Zone::Dead => self.dead[s.i] = c,
        }
    }

    /// deal order: hand players' slots, board A, board B
    fn order(&self) -> Vec<(Slot, bool)> {
        let mut o = Vec::new();
        for (p, pl) in self.players.iter().enumerate() {
            if pl.mode == Mode::Hand {
                for i in 0..5 {
                    o.push((Slot { zone: Zone::Player(p), i }, pl.cards[i].is_some()));
                }
            }
        }
        for i in 0..5 {
            o.push((Slot { zone: Zone::Board, i }, self.board[i].is_some()));
        }
        if self.dbl {
            for i in 0..5 {
                o.push((Slot { zone: Zone::Board2, i }, self.board2[i].is_some()));
            }
        }
        o
    }

    pub fn next_active(&self, from: Option<Slot>) -> Option<Slot> {
        if let Some(Slot { zone: Zone::Dead, i }) = from {
            return (i + 1..DEAD_SLOTS).find(|&k| self.dead[k].is_none()).map(|k| Slot { zone: Zone::Dead, i: k });
        }
        let order = self.order();
        let idx = from.and_then(|f| order.iter().position(|(s, _)| *s == f));
        let start = idx.map_or(0, |i| i + 1);
        order[start..]
            .iter()
            .chain(order[..idx.map_or(0, |i| i + 1)].iter()) // wrap around
            .find(|(_, filled)| !filled)
            .map(|(s, _)| *s)
    }

    /* ---------------- actions ---------------- */

    /// deck click / typed card: a used card is removed, else dealt to the
    /// active slot. Returns what happened, for the flight animation.
    pub fn pick(&mut self, card: u8) -> Pick {
        if let Some(from) = self.find(card) {
            self.set(from, None);
            return Pick::Removed(from);
        }
        let Some(target) = self.active else { return Pick::Nothing };
        if self.get(target).is_none() {
            return Pick::Nothing;
        }
        let displaced = self.get(target).flatten();
        self.set(target, Some(card));
        if let Zone::Player(p) = target.zone {
            self.sel_p = p;
        }
        self.active = self.next_active(Some(target));
        Pick::Dealt { to: target, displaced }
    }

    pub fn focus(&mut self, s: Slot) {
        self.active = Some(s);
        if let Zone::Player(p) = s.zone {
            self.sel_p = p;
        }
    }

    pub fn clear_slot(&mut self, s: Option<Slot>) -> Option<(u8, Slot)> {
        let s = s.or(self.active)?;
        let c = self.get(s).flatten()?;
        self.set(s, None);
        self.active = Some(s);
        Some((c, s))
    }

    /// fill consecutive slots from the active one; returns (card, slot)s
    pub fn paste(&mut self, cards: &[u8]) -> Vec<(u8, Slot)> {
        let mut at = self.active;
        let mut used = self.used();
        let mut dealt = Vec::new();
        for &c in cards {
            let Some(s) = at else { break };
            if used[c as usize] {
                continue;
            }
            self.set(s, Some(c));
            used[c as usize] = true;
            dealt.push((c, s));
            at = self.next_active(Some(s));
        }
        self.active = at;
        dealt
    }

    pub fn add_player(&mut self) -> bool {
        if self.players.len() >= MAX_PLAYERS {
            return false;
        }
        let p = self.new_player(Mode::Hand);
        self.players.push(p);
        self.active = Some(Slot { zone: Zone::Player(self.players.len() - 1), i: 0 });
        true
    }

    pub fn remove_player(&mut self, p: usize) -> bool {
        if self.players.len() <= 2 || p >= self.players.len() {
            return false;
        }
        self.players.remove(p);
        self.sel_p = self.sel_p.min(self.players.len() - 1);
        self.active = match self.active {
            Some(Slot { zone: Zone::Player(q), .. }) if q == p => None,
            Some(Slot { zone: Zone::Player(q), i }) if q > p => Some(Slot { zone: Zone::Player(q - 1), i }),
            a => a,
        };
        if self.active.is_none() {
            self.active = self.next_active(None);
        }
        true
    }

    pub fn set_mode(&mut self, p: usize, mode: Mode) {
        let pl = &mut self.players[p];
        pl.mode = mode;
        if mode != Mode::Hand {
            pl.cards = [None; 5];
        }
        self.sel_p = p;
        if mode == Mode::Hand {
            if let Some(i) = self.players[p].cards.iter().position(|c| c.is_none()) {
                self.active = Some(Slot { zone: Zone::Player(p), i });
            }
        } else if matches!(self.active, Some(Slot { zone: Zone::Player(q), .. }) if q == p) {
            self.active = self.next_active(None);
        }
    }

    pub fn clear_player(&mut self, p: usize) {
        self.players[p].cards = [None; 5];
        if self.players[p].mode == Mode::Hand {
            self.active = Some(Slot { zone: Zone::Player(p), i: 0 });
        }
    }

    pub fn clear_boards(&mut self) {
        self.board = [None; 5];
        self.board2 = [None; 5];
        self.active = Some(Slot { zone: Zone::Board, i: 0 });
    }

    pub fn clear_all(&mut self) {
        for p in &mut self.players {
            p.cards = [None; 5];
        }
        self.board = [None; 5];
        self.board2 = [None; 5];
        self.dead = [None; DEAD_SLOTS];
        self.active = self.next_active(None);
    }

    pub fn set_double(&mut self, v: bool) {
        self.dbl = v;
        if !v {
            self.board2 = [None; 5];
            if matches!(self.active, Some(Slot { zone: Zone::Board2, .. })) {
                self.active = Some(Slot { zone: Zone::Board, i: 0 });
            }
        }
    }

    fn remove_card(&mut self, c: u8) {
        if let Some(s) = self.find(c) {
            self.set(s, None);
        }
    }

    pub fn set_boards(&mut self, a: &[u8], b: &[u8]) {
        for &c in a.iter().chain(b) {
            self.remove_card(c);
        }
        self.board = [None; 5];
        for (i, &c) in a.iter().take(5).enumerate() {
            self.board[i] = Some(c);
        }
        if self.dbl {
            self.board2 = [None; 5];
            for (i, &c) in b.iter().take(5).enumerate() {
                self.board2[i] = Some(c);
            }
        }
        self.active = self.next_active(None);
    }

    pub fn deal_hand(&mut self, p: usize, cards: &[u8]) {
        for &c in cards {
            self.remove_card(c);
        }
        let pl = &mut self.players[p];
        pl.mode = Mode::Hand;
        pl.cards = [None; 5];
        for (i, &c) in cards.iter().take(5).enumerate() {
            pl.cards[i] = Some(c);
        }
        self.sel_p = p;
        self.active = self.next_active(None);
    }

    /* ---------------- engine request ---------------- */

    fn counts(&self) -> (usize, usize) {
        let n = |b: &[Card]| b.iter().filter(|c| c.is_some()).count();
        (n(&self.board), if self.dbl { n(&self.board2) } else { 0 })
    }

    /// why the setup can't be calculated yet, or None
    pub fn validate(&self, ranks_loaded: bool) -> Option<String> {
        let (nb, nb2) = self.counts();
        if nb == 1 || nb == 2 {
            return Some(format!("Board{} needs 3, 4 or 5 cards (or none)", if self.dbl { " A" } else { "" }));
        }
        if self.dbl && (nb2 == 1 || nb2 == 2) {
            return Some("Board B needs 3, 4 or 5 cards (or none)".into());
        }
        for (i, p) in self.players.iter().enumerate() {
            match p.mode {
                Mode::Hand => {
                    let n = p.cards.iter().filter(|c| c.is_some()).count();
                    if n < 5 {
                        let k = 5 - n;
                        return Some(format!(
                            "Player {} needs {k} more card{} — pick from the deck or type e.g. A then H for A♥",
                            i + 1,
                            if k == 1 { "" } else { "s" }
                        ));
                    }
                }
                Mode::Range => {
                    if p.hi <= p.lo {
                        return Some(format!("Player {}: the range is empty", i + 1));
                    }
                    if !self.dbl && (p.chain[0].hi <= p.chain[0].lo || p.chain[1].hi <= p.chain[1].lo) {
                        return Some(format!("Player {}: a street filter is empty", i + 1));
                    }
                    if nb == 0 && (!self.dbl || nb2 == 0) && !ranks_loaded {
                        return Some("Preflop ranges need the rank table — build it once in Settings".into());
                    }
                    if self.dbl && (nb > 0) != (nb2 > 0) {
                        return Some("Double-board ranges need both boards set (or both empty)".into());
                    }
                }
                Mode::Random => {}
            }
        }
        None
    }

    pub fn equity_query(&self, trials: u64, maxenum: u64, build_ranks: bool) -> String {
        let players: Vec<String> = self.players.iter().map(|p| player_spec(p, self.dbl)).collect();
        format!(
            "players={}&board={}&board2={}&dead={}&double={}&trials={trials}&maxenum={maxenum}&buildranks={}&seed=3141592",
            enc(&players.join(",")),
            cards_str(&self.board),
            if self.dbl { cards_str(&self.board2) } else { String::new() },
            cards_str(&self.dead),
            u8::from(self.dbl),
            u8::from(build_ranks),
        )
    }

    pub fn board_str(&self) -> String {
        cards_str(&self.board)
    }

    pub fn board2_str(&self) -> String {
        if self.dbl { cards_str(&self.board2) } else { String::new() }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Pick {
    Nothing,
    Removed(Slot),
    Dealt { to: Slot, displaced: Option<u8> },
}

fn fmt_num(v: f64) -> String {
    if v.fract() == 0.0 { format!("{}", v as i64) } else { format!("{v}") }
}

pub fn player_spec(p: &Player, dbl: bool) -> String {
    match p.mode {
        Mode::Random => "random".into(),
        Mode::Range => {
            let [f, t] = p.chain;
            let b = |x: Band| format!("{}-{}", fmt_num(x.lo), fmt_num(x.hi));
            let mut s = String::new();
            if !dbl && t.active() {
                s = format!("{}>{}>", b(f), b(t));
            } else if !dbl && f.active() {
                s = format!("{}>", b(f));
            }
            s + &b(Band { lo: p.lo, hi: p.hi })
        }
        Mode::Hand => cards_str(&p.cards),
    }
}

pub fn player_label(p: &Player, dbl: bool) -> String {
    match p.mode {
        Mode::Random => "Random hand".into(),
        Mode::Range => {
            let chained = !dbl && (p.chain[0].active() || p.chain[1].active());
            format!("{}–{} pct{}", fmt_num(p.lo), fmt_num(p.hi), if chained { " (chained)" } else { "" })
        }
        Mode::Hand => cards_str(&p.cards),
    }
}

pub fn cards_str(cards: &[Card]) -> String {
    cards.iter().flatten().map(|&c| plo5::cards::card_str(c as usize)).collect()
}

/// percent-encode a query value
pub fn enc(s: &str) -> String {
    s.bytes()
        .map(|b| match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_' | b'~' => (b as char).to_string(),
            _ => format!("%{b:02X}"),
        })
        .collect()
}

/// forgiving parser for pasted text: "A♥ K♠ 10d qc" -> card ids (no repeats)
pub fn parse_loose(text: &str) -> Vec<u8> {
    let norm: String = text
        .replace("10", "T")
        .chars()
        .map(|c| match c {
            '♣' | '♧' => 'c',
            '♦' | '♢' => 'd',
            '♥' | '♡' => 'h',
            '♠' | '♤' => 's',
            c => c,
        })
        .collect();
    let ch: Vec<char> = norm.chars().collect();
    let mut out: Vec<u8> = Vec::new();
    let mut i = 0;
    while i < ch.len() {
        let r = "23456789TJQKA".find(ch[i].to_ascii_uppercase());
        if let Some(r) = r {
            let mut j = i + 1;
            while j < ch.len() && ch[j].is_whitespace() {
                j += 1;
            }
            if let Some(s) = ch.get(j).and_then(|c| "cdhs".find(c.to_ascii_lowercase())) {
                let id = (r * 4 + s) as u8;
                if !out.contains(&id) {
                    out.push(id);
                }
                i = j + 1;
                continue;
            }
        }
        i += 1;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ids(s: &str) -> Vec<u8> {
        plo5::cards::parse_cards(s, 52).unwrap().into_iter().map(|c| c as u8).collect()
    }
    fn deal(c: &mut Calc, s: &str) {
        for id in ids(s) {
            c.pick(id);
        }
    }

    #[test]
    fn picking_fills_players_then_board() {
        let mut c = Calc::default();
        deal(&mut c, "AhAdKcKd7c9s8s7d6h5cQhJc2s");
        assert_eq!(cards_str(&c.players[0].cards), "AhAdKcKd7c");
        assert_eq!(cards_str(&c.players[1].cards), "9s8s7d6h5c");
        assert_eq!(c.board_str(), "QhJc2s");
        assert_eq!(c.active, Some(Slot { zone: Zone::Board, i: 3 }));
        assert_eq!(c.validate(false), None);
    }

    #[test]
    fn picking_a_used_card_returns_it() {
        let mut c = Calc::default();
        deal(&mut c, "AhKh");
        let kh = ids("Kh")[0];
        assert_eq!(c.pick(kh), Pick::Removed(Slot { zone: Zone::Player(0), i: 1 }));
        assert!(!c.used()[kh as usize]);
    }

    #[test]
    fn paste_skips_used_cards() {
        let mut c = Calc::default();
        deal(&mut c, "Ah");
        c.paste(&parse_loose("A♥ K♠ 10d qc 2c 3c"));
        assert_eq!(cards_str(&c.players[0].cards), "AhKsTdQc2c");
        assert_eq!(c.players[1].cards[0], Some(ids("3c")[0]));
    }

    #[test]
    fn removing_players_shifts_focus() {
        let mut c = Calc::default();
        c.add_player();
        c.focus(Slot { zone: Zone::Player(2), i: 1 });
        c.remove_player(0);
        assert_eq!(c.players.len(), 2);
        assert_eq!(c.active, Some(Slot { zone: Zone::Player(1), i: 1 }));
        assert!(!c.remove_player(0), "never below two players");
    }

    #[test]
    fn specs_and_validation() {
        let mut c = Calc::default();
        deal(&mut c, "AhAdKcKd7c");
        assert!(c.validate(true).unwrap().contains("Player 2 needs 5 more cards"));
        c.set_mode(1, Mode::Range);
        assert!(c.validate(false).unwrap().contains("rank table"));
        assert_eq!(c.validate(true), None);
        let p = &mut c.players[1];
        p.lo = 40.0;
        p.chain = [Band { lo: 0.0, hi: 50.0 }, Band::FULL];
        assert_eq!(player_spec(&c.players[1], false), "0-50>40-100");
        assert_eq!(player_spec(&c.players[1], true), "40-100");
        assert!(c.equity_query(1000, 10, true).starts_with("players=AhAdKcKd7c%2C0-50%3E40-100&board=&"));
    }

    #[test]
    fn revive_drops_duplicates() {
        let saved: Saved = serde_json::from_str(
            r#"{"players":[{"mode":"Hand","cards":[1,1,2,99,null],"lo":0,"hi":100,"chain":[{"lo":0,"hi":100},{"lo":0,"hi":100}]}],"board":[2,3,4],"board2":[],"dead":[],"dbl":false}"#,
        )
        .unwrap();
        let c = Calc::revive(saved);
        assert_eq!(c.players[0].cards, [Some(1), None, Some(2), None, None]);
        assert_eq!(c.players.len(), 2);
        assert_eq!(c.board, [None, Some(3), Some(4), None, None]);
    }
}
