//! Card flights between the deck and the card slots.
//!
//! Slots and the deck report their window positions (see cards.slint); a
//! flight interpolates from where the card is to where it is going, every
//! frame, along a shallow arc with a little overshoot. The flying copy is
//! drawn on an overlay layer while the real card is hidden (Data.hidden),
//! and lands exactly on the slot, so the hand-off is invisible.

use std::collections::HashMap;
use std::rc::Rc;
use std::time::{Duration, Instant};

use slint::{Model, Timer, TimerMode, VecModel};

use crate::{Flight, ANIM};

#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

/// a card slot, with players keyed by id so rows can shift underneath
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SlotKey {
    Player(u32, usize),
    Board(usize),
    Board2(usize),
    Dead(usize),
}

#[derive(Clone, Copy, Debug)]
pub enum End {
    Deck,
    Slot(SlotKey),
}

// must match Deck in cards.slint
const CW: f32 = 36.0;
const CH: f32 = 48.0;
const GAP: f32 = 5.0;
const DURATION: Duration = Duration::from_millis(430);

struct Fl {
    card: u8,
    from: Rect,
    to: End,
    start: Instant,
}

#[derive(Default)]
pub struct Anim {
    pub slots: HashMap<SlotKey, Rect>,
    pub deck: Option<(f32, f32)>,
    pub model: Rc<VecModel<Flight>>,
    flights: Vec<Fl>,
    hidden: Option<Rc<VecModel<bool>>>,
    timer: Option<Timer>,
}

impl Anim {
    fn deck_cell(&self, card: u8) -> Option<Rect> {
        let (x, y) = self.deck?;
        let col = 12 - (card / 4) as usize;
        let row = 3 - (card % 4) as usize;
        Some(Rect { x: x + col as f32 * (CW + GAP), y: y + row as f32 * (CH + GAP), w: CW, h: CH })
    }

    fn rect(&self, card: u8, end: End) -> Option<Rect> {
        match end {
            End::Deck => self.deck_cell(card),
            End::Slot(k) => self.slots.get(&k).copied(),
        }
    }

    /// start a flight now (or after `delay_ms`), from wherever `from` is now
    pub fn fly(&mut self, card: u8, from: End, to: End, delay_ms: u64) {
        // a card already in the air starts from where it currently is
        let from_rect = match self.flights.iter().position(|f| f.card == card) {
            Some(i) => {
                let f = self.flights.remove(i);
                self.current(&f, Instant::now()).unwrap_or(f.from)
            }
            None => match self.rect(card, from) {
                Some(r) => r,
                None => return, // nothing to fly from: just appear
            },
        };
        self.flights.push(Fl { card, from: from_rect, to, start: Instant::now() + Duration::from_millis(delay_ms) });
        self.set_hidden(card, true);
    }

    fn set_hidden(&self, card: u8, v: bool) {
        if let Some(h) = &self.hidden {
            if h.row_data(card as usize) != Some(v) {
                h.set_row_data(card as usize, v);
            }
        }
    }

    fn current(&self, f: &Fl, now: Instant) -> Option<Rect> {
        let to = self.rect(f.card, f.to)?;
        let t = if now < f.start { 0.0 } else { ((now - f.start).as_secs_f32() / DURATION.as_secs_f32()).min(1.0) };
        // position: ease-out with a slight overshoot; size: smooth
        let c1 = 1.15f32;
        let c3 = c1 + 1.0;
        let u = t - 1.0;
        let pe = 1.0 + c3 * u * u * u + c1 * u * u;
        let se = 1.0 - (1.0 - t).powi(3);
        let lerp = |a: f32, b: f32, e: f32| a + (b - a) * e;
        let dist = ((to.x - f.from.x).powi(2) + (to.y - f.from.y).powi(2)).sqrt();
        let lift = (std::f32::consts::PI * t).sin() * (dist * 0.08).min(36.0);
        Some(Rect {
            x: lerp(f.from.x, to.x, pe),
            y: lerp(f.from.y, to.y, pe) - lift,
            w: lerp(f.from.w, to.w, se),
            h: lerp(f.from.h, to.h, se),
        })
    }

    /// advance every flight one frame; false when none are left
    fn tick(&mut self) -> bool {
        let now = Instant::now();
        let mut rows: Vec<Flight> = Vec::new();
        let mut done: Vec<u8> = Vec::new();
        let mut keep: Vec<Fl> = Vec::new();
        for f in std::mem::take(&mut self.flights) {
            match self.current(&f, now) {
                Some(r) => {
                    let finished = now >= f.start + DURATION;
                    if finished {
                        done.push(f.card);
                    } else {
                        rows.push(Flight { card: f.card as i32, x: r.x, y: r.y, w: r.w, h: r.h });
                        keep.push(f);
                    }
                }
                // destination not laid out yet: wait briefly, then give up
                None if now < f.start + Duration::from_millis(200) => {
                    rows.push(Flight { card: f.card as i32, x: f.from.x, y: f.from.y, w: f.from.w, h: f.from.h });
                    keep.push(f);
                }
                None => done.push(f.card),
            }
        }
        self.flights = keep;
        for c in done {
            self.set_hidden(c, false);
        }
        if self.model.row_count() == rows.len() {
            for (i, r) in rows.into_iter().enumerate() {
                self.model.set_row_data(i, r);
            }
        } else {
            self.model.set_vec(rows);
        }
        !self.flights.is_empty()
    }
}

/// make sure the frame timer runs while flights are in the air
pub fn start(hidden: Rc<VecModel<bool>>) {
    ANIM.with(|a| {
        let mut a = a.borrow_mut();
        a.hidden = Some(hidden);
        if a.timer.as_ref().is_some_and(|t| t.running()) {
            return;
        }
        let t = Timer::default();
        t.start(TimerMode::Repeated, Duration::from_millis(16), || {
            let more = ANIM.with(|a| a.try_borrow_mut().map(|mut a| a.tick()).unwrap_or(true));
            if !more {
                ANIM.with(|a| {
                    if let Ok(a) = a.try_borrow() {
                        if let Some(t) = &a.timer {
                            t.stop();
                        }
                    }
                });
            }
        });
        a.timer = Some(t);
        a.tick();
    });
}
