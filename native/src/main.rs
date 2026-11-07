// PLO5 Equity Lab — native desktop app: the Rust PLO5 engine with a Slint
// interface. No browser engine.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod anim;
mod calc;
mod engine_host;
mod persist;

use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::time::{Duration, Instant};

use plo5::api::Presets;
use serde_json::Value;
use slint::{Color, ComponentHandle, Model, ModelRc, SharedString, Timer, TimerMode, VecModel};

use anim::{Anim, Rect, SlotKey};
use calc::{Calc, Mode, Pick, Slot, Zone};
use engine_host::{EngineHost, Reply};
use persist::{Paths, State};

slint::include_modules!();

const CAT_COLORS: [u32; 9] = [0x64748b, 0x60a5fa, 0x22d3ee, 0x34d399, 0xa3e635, 0xfacc15, 0xfb923c, 0xf472b6, 0xc084fc];
const PLAYER_COLORS: [u32; 6] = [0x5cb2ff, 0xff7a93, 0xffc65c, 0xb38cff, 0x3fe0c5, 0xa6e35e];
const STREETS: [&str; 5] = ["flop", "flop", "flop", "turn", "river"];

fn rgb(c: u32) -> Color {
    Color::from_rgb_u8((c >> 16) as u8, (c >> 8) as u8, c as u8)
}

fn fmt_int(n: u64) -> String {
    let s = n.to_string();
    let mut out = String::new();
    for (i, ch) in s.chars().enumerate() {
        if i > 0 && (s.len() - i).is_multiple_of(3) {
            out.push(',');
        }
        out.push(ch);
    }
    out
}

fn fmt_trials(n: u64) -> String {
    if n >= 1_000_000 && n.is_multiple_of(1_000_000) {
        format!("{}M trials", n / 1_000_000)
    } else if n >= 1_000_000 {
        format!("{:.1}M trials", n as f64 / 1e6)
    } else {
        format!("{}k trials", n / 1000)
    }
}

fn cards_of(v: &Value) -> Vec<u8> {
    v.as_str()
        .and_then(|s| plo5::cards::parse_cards(s, 52))
        .map(|c| c.into_iter().map(|x| x as u8).collect())
        .unwrap_or_default()
}

fn f64s(v: &Value, k: &str) -> Vec<f64> {
    v.get(k).and_then(Value::as_array).map(|a| a.iter().map(|x| x.as_f64().unwrap_or(f64::NAN)).collect()).unwrap_or_default()
}

fn ordinal(n: i64) -> String {
    let suf = match (n % 100, n % 10) {
        (11..=13, _) => "th",
        (_, 1) => "st",
        (_, 2) => "nd",
        (_, 3) => "rd",
        _ => "th",
    };
    format!("{n}{suf}")
}

/// a random seed for the trainers' deals
fn seed() -> u64 {
    plo5::api::fresh_seed()
}

#[derive(Default)]
struct Run {
    result: Option<Value>,
    key: Option<String>,
    snapshot: Vec<(u32, Mode)>,
    calculating: bool,
    req: u64,
    error: Option<String>,
    progress: Option<(String, u32, u32)>,
}

#[derive(Default)]
struct EqQuiz {
    token: u64,
    hero: Vec<u8>,
    flop: Vec<u8>,
    flop2: Vec<u8>,
    nopp: usize,
    answered: Option<f64>,
    result: Option<Value>,
    scored: bool,
    started: bool,
}

#[derive(Default)]
struct SprQuiz {
    token: u64,
    q: Option<Value>,
    choice: Option<bool>,
    started: bool,
}

struct App {
    ui: slint::Weak<AppWindow>,
    calc: Calc,
    host: EngineHost,
    paths: Paths,
    state: State,
    presets: Presets,
    ranks_loaded: bool,
    run: Run,
    auto_timer: Timer,
    save_timer: Timer,
    lookup_timer: Timer,
    pending_rank: Option<usize>,
    busy_board: bool,
    pct_hand: Option<(Vec<u8>, String)>,
    pct_err: String,
    pct_token: u64,
    eq_hand: Option<(Vec<u8>, f64)>,
    eq_err: String,
    eq_busy: bool,
    rank_job: Option<(Instant, u32, u32)>,
    toasts: Vec<(i32, String, i32)>,
    toast_id: i32,
    eq: EqQuiz,
    spr: SprQuiz,
    // models kept alive so rows update in place (animations keep running)
    m_players: Rc<VecModel<PlayerData>>,
    rows: Vec<u32>,
    leaving: Vec<u32>,
    m_cards: HashMap<u32, Rc<VecModel<i32>>>,
    m_ht: HashMap<u32, Rc<VecModel<HtSeg>>>,
    m_board: Rc<VecModel<i32>>,
    m_board2: Rc<VecModel<i32>>,
    m_dead: Rc<VecModel<i32>>,
    m_used: Rc<VecModel<bool>>,
    m_hidden: Rc<VecModel<bool>>,
    m_donut: Rc<VecModel<DonutSeg>>,
    last_active: Option<Slot>,
}

thread_local! {
    static APP: RefCell<Option<App>> = const { RefCell::new(None) };
    static ANIM: RefCell<Anim> = RefCell::new(Anim::default());
}

/// run `f` on the app; if it is busy (re-entrant call), retry next tick
fn with_app(f: impl FnOnce(&mut App) + 'static) {
    let pending = APP.with(|cell| match cell.try_borrow_mut() {
        Ok(mut g) => {
            if let Some(app) = g.as_mut() {
                f(app);
            }
            None
        }
        Err(_) => Some(f),
    });
    if let Some(f) = pending {
        Timer::single_shot(Duration::ZERO, move || with_app(f));
    }
}

/// hop from the engine thread back to the UI thread
fn on_ui(f: impl FnOnce(&mut App) + Send + 'static) {
    let _ = slint::invoke_from_event_loop(move || with_app(f));
}

impl App {
    fn ui(&self) -> AppWindow {
        self.ui.upgrade().expect("window")
    }

    fn preset(&self) -> (u64, u64) {
        let t = self.presets.0[self.state.settings.precision.min(2)];
        (t, t / 5)
    }

    /// the request the current setup would send (None = not calculable)
    fn run_key(&self) -> Option<String> {
        if self.calc.validate(self.ranks_loaded).is_some() {
            return None;
        }
        let (t, m) = self.preset();
        Some(self.calc.equity_query(t, m, self.state.settings.build_ranks))
    }

    fn toast(&mut self, text: impl Into<String>, kind: i32) {
        self.toast_id += 1;
        let id = self.toast_id;
        self.toasts.push((id, text.into(), kind));
        if self.toasts.len() > 3 {
            self.toasts.remove(0);
        }
        self.sync_toasts();
        Timer::single_shot(Duration::from_millis(2600), move || {
            with_app(move |app| {
                app.toasts.retain(|t| t.0 != id);
                app.sync_toasts();
            })
        });
    }

    fn sync_toasts(&self) {
        let ui = self.ui();
        let v: Vec<Toast> = self.toasts.iter().map(|(id, t, k)| Toast { id: *id, text: t.into(), kind: *k }).collect();
        ui.global::<Data>().set_toasts(ModelRc::new(VecModel::from(v)));
    }

    /* ---------------- calculator state -> UI ---------------- */

    /// called after every calculator change
    fn changed(&mut self) {
        self.sync();
        self.schedule_auto();
        self.schedule_lookup();
        self.save_timer.start(TimerMode::SingleShot, Duration::from_millis(500), || with_app(|app| app.save()));
    }

    fn save(&mut self) {
        self.persist();
    }

    /// write settings, stats and the current calculator setup together
    fn persist(&mut self) {
        self.state.calc = Some(self.calc.saved());
        persist::save_state(&self.paths.state, &self.state);
    }

    fn result_for(&self, id: u32, mode: Mode) -> Option<(usize, &Value)> {
        let r = self.run.result.as_ref()?;
        let k = self.run.snapshot.iter().position(|&(i, m)| i == id && m == mode)?;
        Some((k, r))
    }

    fn sync(&mut self) {
        let ui = self.ui();
        let d = ui.global::<Data>();
        let calc = &self.calc;
        let key = self.run_key();
        let stale = self.run.result.is_some() && key != self.run.key;

        // ---- players (rows update in place; removed rows fade out first)
        let mut datas: Vec<PlayerData> = Vec::new();
        for (pi, p) in calc.players.iter().enumerate() {
            let cards = self.m_cards.entry(p.id).or_insert_with(|| Rc::new(VecModel::from(vec![-1; 5]))).clone();
            for i in 0..5 {
                let v = p.cards[i].map_or(-1, |c| c as i32);
                if cards.row_data(i) != Some(v) {
                    cards.set_row_data(i, v);
                }
            }
            let ht = self.m_ht.entry(p.id).or_insert_with(|| Rc::new(VecModel::from(vec![HtSeg::default(); 9]))).clone();
            let mut pd = PlayerData {
                id: p.id as i32,
                mode: p.mode as i32,
                cards: ModelRc::from(cards),
                lo: p.lo as i32,
                hi: p.hi as i32,
                flo: p.chain[0].lo as i32,
                fhi: p.chain[0].hi as i32,
                tlo: p.chain[1].lo as i32,
                thi: p.chain[1].hi as i32,
                filters_open: p.filters_open,
                chained: !calc.dbl && (p.chain[0].active() || p.chain[1].active()),
                selected: calc.sel_p == pi,
                has_res: false,
                equity: 0.0,
                win: 0.0,
                tie: 0.0,
                ci: 0.0,
                pct: -1.0,
                ht: ModelRc::from(ht.clone()),
                leaving: false,
            };
            if let Some((k, r)) = self.result_for(p.id, p.mode) {
                let g = |name: &str| f64s(r, name).get(k).copied().unwrap_or(0.0) as f32;
                pd.has_res = true;
                pd.equity = g("equity");
                pd.win = g("win");
                pd.tie = g("tie");
                pd.ci = g("ci95");
                pd.pct = r.get("pct").and_then(|a| a.get(k)).and_then(Value::as_f64).map_or(-1.0, |v| v as f32);
                if let Some(h) = r.get("handType").and_then(|a| a.get(k)).and_then(Value::as_array) {
                    let mut x = 0.0;
                    for (c, v) in h.iter().enumerate().take(9) {
                        let w = v.as_f64().unwrap_or(0.0) as f32;
                        let seg = HtSeg { x, w, color: rgb(CAT_COLORS[c]) };
                        if ht.row_data(c) != Some(seg.clone()) {
                            ht.set_row_data(c, seg);
                        }
                        x += w;
                    }
                }
            }
            datas.push(pd);
        }
        // reconcile rows: leaving rows stay (faded) until removed by a timer
        let ids: Vec<u32> = calc.players.iter().map(|p| p.id).collect();
        for (row, &rid) in self.rows.clone().iter().enumerate() {
            if !ids.contains(&rid) && !self.leaving.contains(&rid) {
                self.leaving.push(rid);
                if let Some(mut old) = self.m_players.row_data(row) {
                    old.leaving = true;
                    self.m_players.set_row_data(row, old);
                }
                Timer::single_shot(Duration::from_millis(300), move || {
                    with_app(move |app| {
                        if let Some(r) = app.rows.iter().position(|&x| x == rid) {
                            app.rows.remove(r);
                            app.m_players.remove(r);
                        }
                        app.leaving.retain(|&x| x != rid);
                        app.m_cards.remove(&rid);
                        app.m_ht.remove(&rid);
                        app.sync();
                    })
                });
            }
        }
        for pd in datas {
            let id = pd.id as u32;
            match self.rows.iter().position(|&r| r == id) {
                Some(row) => {
                    if self.m_players.row_data(row).as_ref() != Some(&pd) {
                        self.m_players.set_row_data(row, pd);
                    }
                }
                None => {
                    self.rows.push(id);
                    self.m_players.push(pd);
                }
            }
        }
        d.set_nplayers(calc.players.len() as i32);

        // ---- boards, dead cards, deck
        let put = |m: &Rc<VecModel<i32>>, cards: &[calc::Card]| {
            for (i, c) in cards.iter().enumerate() {
                let v = c.map_or(-1, |c| c as i32);
                if m.row_data(i) != Some(v) {
                    m.set_row_data(i, v);
                }
            }
        };
        put(&self.m_board, &calc.board);
        put(&self.m_board2, &calc.board2);
        put(&self.m_dead, &calc.dead);
        d.set_dbl(calc.dbl);
        let used = calc.used();
        for (i, &u) in used.iter().enumerate() {
            if self.m_used.row_data(i) != Some(u) {
                self.m_used.set_row_data(i, u);
            }
        }

        // ---- active slot + ring
        let (z, p, i) = match calc.active {
            Some(Slot { zone: Zone::Player(p), i }) => (0, self.rows.iter().position(|&r| r == calc.players[p].id).unwrap_or(p) as i32, i as i32),
            Some(Slot { zone: Zone::Board, i }) => (1, 0, i as i32),
            Some(Slot { zone: Zone::Board2, i }) => (2, 0, i as i32),
            Some(Slot { zone: Zone::Dead, i }) => (3, 0, i as i32),
            None => (-1, 0, 0),
        };
        d.set_active_zone(z);
        d.set_active_p(p);
        d.set_active_i(i);
        d.set_sel_p(calc.sel_p as i32);
        d.set_deal_hint(
            match calc.active {
                Some(Slot { zone: Zone::Player(p), i }) => format!("Dealing to Player {} · card {}", p + 1, i + 1),
                Some(Slot { zone: Zone::Board, i }) => format!("Dealing to Board · {}", STREETS[i]),
                Some(Slot { zone: Zone::Board2, i }) => format!("Dealing to Board B · {}", STREETS[i]),
                Some(Slot { zone: Zone::Dead, i }) => format!("Dealing to dead card {}", i + 1),
                None => "Select a slot to deal into".into(),
            }
            .into(),
        );
        let moved = self.last_active != calc.active;
        self.last_active = calc.active;
        self.update_ring(moved);
        d.set_pending_rank(self.pending_rank.map_or(SharedString::new(), |r| (if r == 8 { "10" } else { &"23456789TJQKA"[r..r + 1] }).into()));
        d.set_busy_board(self.busy_board);

        // ---- run panel + notices
        let hint = calc.validate(self.ranks_loaded);
        d.set_can_calc(hint.is_none());
        d.set_calculating(self.run.calculating);
        d.set_stale(stale);
        let (notice, kind) = if let Some(e) = &self.run.error {
            (e.clone(), 2)
        } else if let Some(h) = hint {
            (h, 1)
        } else if stale && !self.state.settings.auto {
            ("Results are out of date — press Enter or Calculate".into(), 3)
        } else {
            (String::new(), 0)
        };
        d.set_notice(notice.into());
        d.set_notice_kind(kind);
        let (bold, meta) = if self.run.calculating {
            match &self.run.progress {
                Some((w, a, b)) => {
                    let mut w = w.clone();
                    if let Some(f) = w.get_mut(0..1) {
                        f.make_ascii_uppercase();
                    }
                    (String::new(), format!("{w} {}%", (100 * a / (*b).max(1))))
                }
                None => (String::new(), "Running simulation…".into()),
            }
        } else if let Some(r) = &self.run.result {
            let exact = r.get("exact").and_then(Value::as_bool).unwrap_or(false);
            let n = r.get("samples").and_then(Value::as_u64).unwrap_or(0);
            let ms = r.get("ms").and_then(Value::as_f64).unwrap_or(0.0);
            let t = if ms < 1.0 { "<1 ms".into() } else if ms < 1000.0 { format!("{ms:.0} ms") } else { format!("{:.1} s", ms / 1000.0) };
            (
                (if exact { "Exact" } else { "Monte Carlo" }).into(),
                format!("· {} {} · {t}", fmt_int(n), if exact { "runouts" } else { "trials" }),
            )
        } else {
            (String::new(), format!("{} engine threads ready", self.host.threads))
        };
        d.set_run_meta_bold(bold.into());
        d.set_run_meta(meta.into());
        d.set_progress(match (&self.run.progress, self.run.calculating) {
            (Some((_, a, b)), true) => *a as f32 / (*b).max(1) as f32,
            _ => -1.0,
        });

        // ---- pot share + hand types (only when the result matches the table)
        let matches = self.run.result.is_some()
            && self.run.snapshot.len() == calc.players.len()
            && self.run.snapshot.iter().zip(&calc.players).all(|(s, p)| s.0 == p.id);
        d.set_has_result(matches);
        if let (true, Some(r)) = (matches, &self.run.result) {
            let eq = f64s(r, "equity");
            let mut start = 0.0f32;
            let segs: Vec<DonutSeg> = eq
                .iter()
                .enumerate()
                .map(|(i, &e)| {
                    let len = (e / 100.0).max(0.0) as f32;
                    let s = DonutSeg { start, len, color: rgb(PLAYER_COLORS[i]) };
                    start += len;
                    s
                })
                .collect();
            if self.m_donut.row_count() != segs.len() {
                self.m_donut.set_vec(segs);
            } else {
                for (i, s) in segs.into_iter().enumerate() {
                    self.m_donut.set_row_data(i, s);
                }
            }
            let top = eq.iter().enumerate().fold(0, |b, (i, &e)| if e > eq[b] { i } else { b });
            d.set_donut_label(format!("P{}", top + 1).into());
            d.set_donut_value(eq[top] as f32);
            d.set_donut_color(rgb(PLAYER_COLORS[top]));
            let legend: Vec<LegendRow> = calc
                .players
                .iter()
                .enumerate()
                .map(|(i, p)| LegendRow {
                    who: format!("P{}", i + 1).into(),
                    what: calc::player_label(p, calc.dbl).into(),
                    val: format!("{:.1}%", eq.get(i).copied().unwrap_or(0.0)).into(),
                    color: rgb(PLAYER_COLORS[i]),
                })
                .collect();
            d.set_legend(ModelRc::new(VecModel::from(legend)));
            let rows = |key: &str| -> Vec<HtRow> {
                r.get(key)
                    .and_then(Value::as_array)
                    .map(|a| {
                        a.iter()
                            .enumerate()
                            .map(|(p, h)| {
                                let vals: Vec<f64> = h.as_array().map(|v| v.iter().map(|x| x.as_f64().unwrap_or(0.0)).collect()).unwrap_or_default();
                                let mut x = 0.0f32;
                                let segs: Vec<HtSeg> = vals
                                    .iter()
                                    .enumerate()
                                    .map(|(c, &v)| {
                                        let s = HtSeg { x, w: v as f32, color: rgb(CAT_COLORS[c]) };
                                        x += v as f32;
                                        s
                                    })
                                    .collect();
                                HtRow {
                                    who: format!("P{}", p + 1).into(),
                                    color: rgb(PLAYER_COLORS[p % 6]),
                                    segs: ModelRc::new(VecModel::from(segs)),
                                    vals: ModelRc::new(VecModel::from(
                                        vals.iter().map(|&v| SharedString::from(if v < 0.005 { "—".to_string() } else { format!("{v:.2}") })).collect::<Vec<_>>(),
                                    )),
                                }
                            })
                            .collect()
                    })
                    .unwrap_or_default()
            };
            d.set_ht_rows(ModelRc::new(VecModel::from(rows("handType"))));
            let dbl = r.get("double").and_then(Value::as_bool).unwrap_or(false);
            d.set_ht_double(dbl);
            if dbl {
                d.set_ht_rows_b(ModelRc::new(VecModel::from(rows("handTypeB"))));
            }
        }

        // ---- tools
        let (nb, nb2) = (calc.board.iter().flatten().count(), if calc.dbl { calc.board2.iter().flatten().count() } else { 0 });
        let board_ok = nb != 1 && nb != 2 && nb2 != 1 && nb2 != 2;
        d.set_board_ok(board_ok);
        d.set_pct_street(
            (if nb >= 3 {
                if calc.dbl && nb2 >= 3 { "both boards" } else { STREETS[nb.min(5) - 1] }
            } else {
                "preflop"
            })
            .into(),
        );
        let ints = |v: &[u8]| ModelRc::new(VecModel::from(v.iter().map(|&c| c as i32).collect::<Vec<_>>()));
        d.set_pct_hand(self.pct_hand.as_ref().map_or_else(|| ints(&[]), |(h, _)| ints(h)));
        d.set_pct_err(self.pct_err.clone().into());
        d.set_eq_hand(self.eq_hand.as_ref().map_or_else(|| ints(&[]), |(h, _)| ints(h)));
        d.set_eq_val(self.eq_hand.as_ref().map_or(String::new(), |(_, e)| format!("{e:.1}%")).into());
        d.set_eq_err(self.eq_err.clone().into());
        d.set_eq_busy(self.eq_busy);

        // ---- engine + rank table
        d.set_ranks_loaded(self.ranks_loaded);
        d.set_rank_busy(self.rank_job.is_some());
        if let Some((t0, done, total)) = self.rank_job {
            let frac = done as f32 / total.max(1) as f32;
            d.set_rank_frac(frac);
            let eta = if done > 0 {
                let el = t0.elapsed().as_secs_f64();
                let left = el / done as f64 * (total - done.min(total)) as f64;
                if left < 60.0 { format!("About {} s left.", left.max(1.0).round()) } else { format!("About {} min left.", (left / 60.0).round()) }
            } else {
                "Estimating time…".into()
            };
            d.set_rank_eta(eta.into());
        }
    }

    fn update_ring(&self, moved: bool) {
        let ui = self.ui();
        let d = ui.global::<Data>();
        let key = self.calc.active.map(|s| self.slot_key(s));
        let rect = key.and_then(|k| ANIM.with(|a| a.borrow().slots.get(&k).copied()));
        match rect {
            Some(r) => {
                d.set_ring_anim(moved);
                d.set_ring_x(r.x);
                d.set_ring_y(r.y);
                d.set_ring_w(r.w);
                d.set_ring_h(r.h);
                d.set_ring_on(true);
            }
            None => d.set_ring_on(false),
        }
    }

    /// geometry key of a slot: players by id, so it survives row shifts
    fn slot_key(&self, s: Slot) -> SlotKey {
        match s.zone {
            Zone::Player(p) => SlotKey::Player(self.calc.players.get(p).map_or(0, |x| x.id), s.i),
            Zone::Board => SlotKey::Board(s.i),
            Zone::Board2 => SlotKey::Board2(s.i),
            Zone::Dead => SlotKey::Dead(s.i),
        }
    }

    /// UI (zone, row, i) -> calculator slot
    fn slot_of(&self, zone: i32, row: i32, i: i32) -> Option<Slot> {
        let i = i as usize;
        let zone = match zone {
            0 => {
                let id = *self.rows.get(row as usize)?;
                Zone::Player(self.calc.players.iter().position(|p| p.id == id)?)
            }
            1 => Zone::Board,
            2 => Zone::Board2,
            3 => Zone::Dead,
            _ => return None,
        };
        Some(Slot { zone, i })
    }

    fn player_of_row(&self, row: i32) -> Option<usize> {
        let id = *self.rows.get(row as usize)?;
        self.calc.players.iter().position(|p| p.id == id)
    }

    /* ---------------- flights ---------------- */

    fn fly_in(&self, card: u8, to: Slot, delay_ms: u64) {
        if !self.state.settings.deck_open || self.state.settings.view != 0 || self.state.settings.reduced_motion {
            return;
        }
        let key = self.slot_key(to);
        ANIM.with(|a| a.borrow_mut().fly(card, anim::End::Deck, anim::End::Slot(key), delay_ms));
        self.kick_anim();
    }

    fn fly_out(&self, card: u8, from: Slot) {
        if !self.state.settings.deck_open || self.state.settings.view != 0 || self.state.settings.reduced_motion {
            return;
        }
        let key = self.slot_key(from);
        ANIM.with(|a| a.borrow_mut().fly(card, anim::End::Slot(key), anim::End::Deck, 0));
        self.kick_anim();
    }

    fn kick_anim(&self) {
        anim::start(self.m_hidden.clone());
    }

    /* ---------------- engine calls ---------------- */

    fn schedule_auto(&mut self) {
        let key = self.run_key();
        // only while the calculator is on screen: never hog the engine
        // behind a trainer (catch up happens on returning to the view)
        if !self.state.settings.auto || self.state.settings.view != 0 || key.is_none() || key == self.run.key {
            self.auto_timer.stop();
            return;
        }
        self.auto_timer.start(TimerMode::SingleShot, Duration::from_millis(380), || with_app(|app| app.calculate()));
    }

    fn calculate(&mut self) {
        let Some(query) = self.run_key() else { return };
        self.run.req += 1;
        let req = self.run.req;
        self.run.calculating = true;
        self.run.error = None;
        self.run.progress = None;
        let snapshot: Vec<(u32, Mode)> = self.calc.players.iter().map(|p| (p.id, p.mode)).collect();
        let key = query.clone();
        self.host.call(
            "/api/equity",
            query,
            Some(Box::new(move |what, a, b| {
                let what = what.to_string();
                on_ui(move |app| {
                    if app.run.req == req && app.run.calculating {
                        app.run.progress = Some((what, a, b));
                        app.sync();
                    }
                })
            })),
            Box::new(move |reply| {
                on_ui(move |app| {
                    if app.run.req != req {
                        return;
                    }
                    app.run.calculating = false;
                    app.run.progress = None;
                    match reply {
                        Reply::Ok(v) => {
                            app.run.result = Some(v);
                            app.run.key = Some(key);
                            app.run.snapshot = snapshot;
                        }
                        Reply::Failed(e) => app.run.error = Some(e),
                        Reply::Cancelled => {}
                    }
                    app.sync();
                })
            }),
        );
        self.sync();
    }

    fn cancel(&mut self) {
        self.run.req += 1;
        self.run.calculating = false;
        self.run.progress = None;
        self.host.cancel_all();
        self.sync();
    }

    fn schedule_lookup(&mut self) {
        if self.state.settings.view != 0 {
            return;
        }
        // after the auto-calc, so the equity run builds the board ranking
        // (with progress) rather than this lookup
        self.lookup_timer.start(TimerMode::SingleShot, Duration::from_millis(700), || with_app(|app| app.lookup()));
    }

    fn lookup(&mut self) {
        let (b, b2) = (self.calc.board_str(), self.calc.board2_str());
        let (nb, nb2) = (b.len() / 2, b2.len() / 2);
        if nb == 1 || nb == 2 || nb2 == 1 || nb2 == 2 {
            self.pct_hand = None;
            self.pct_err.clear();
            self.sync();
            return;
        }
        self.pct_token += 1;
        let tok = self.pct_token;
        let q = format!(
            "pct={}&board={}&board2={}",
            self.state_pct(),
            if nb >= 3 { b } else { String::new() },
            if nb >= 3 && nb2 >= 3 { b2 } else { String::new() }
        );
        self.host.call(
            "/api/lookup",
            q,
            None,
            Box::new(move |reply| {
                on_ui(move |app| {
                    if app.pct_token != tok {
                        return;
                    }
                    match reply {
                        Reply::Ok(v) => {
                            app.pct_hand = Some((cards_of(&v["hand"]), v["street"].as_str().unwrap_or("").into()));
                            app.pct_err.clear();
                        }
                        Reply::Failed(e) => {
                            app.pct_hand = None;
                            app.pct_err = e;
                        }
                        Reply::Cancelled => {}
                    }
                    app.sync();
                })
            }),
        );
    }

    fn state_pct(&self) -> i32 {
        self.ui().global::<Data>().get_pct_q()
    }

    fn find_eq(&mut self) {
        let target = self.ui().global::<Data>().get_eq_q();
        self.eq_busy = true;
        self.eq_hand = None;
        self.eq_err.clear();
        let q = format!(
            "target={target}&board={}&board2={}&double={}",
            self.calc.board_str(),
            self.calc.board2_str(),
            u8::from(self.calc.dbl)
        );
        self.host.call(
            "/api/findeq",
            q,
            None,
            Box::new(move |reply| {
                on_ui(move |app| {
                    app.eq_busy = false;
                    match reply {
                        Reply::Ok(v) => app.eq_hand = Some((cards_of(&v["hand"]), v["equity"].as_f64().unwrap_or(0.0))),
                        Reply::Failed(e) => app.eq_err = e,
                        Reply::Cancelled => {}
                    }
                    app.sync();
                })
            }),
        );
        self.sync();
    }

    fn deal_board(&mut self, street: usize) {
        let mut keep = String::new();
        for p in &self.calc.players {
            if p.mode == Mode::Hand {
                keep += &calc::cards_str(&p.cards);
            }
        }
        keep += &calc::cards_str(&self.calc.dead);
        self.busy_board = true;
        let q = format!("street={street}&count={}&used={keep}", if self.calc.dbl { 2 } else { 1 });
        self.host.call(
            "/api/random",
            q,
            None,
            Box::new(move |reply| {
                on_ui(move |app| {
                    app.busy_board = false;
                    match reply {
                        Reply::Ok(v) => {
                            let (a, b) = (cards_of(&v["board"]), cards_of(&v["board2"]));
                            // previous board cards fly home, new ones fly in
                            let old: Vec<(u8, Slot)> = app
                                .calc
                                .board
                                .iter()
                                .enumerate()
                                .filter_map(|(i, c)| c.map(|c| (c, Slot { zone: Zone::Board, i })))
                                .chain(app.calc.board2.iter().enumerate().filter_map(|(i, c)| c.map(|c| (c, Slot { zone: Zone::Board2, i }))))
                                .collect();
                            for (c, s) in &old {
                                if !a.contains(c) && !b.contains(c) {
                                    app.fly_out(*c, *s);
                                }
                            }
                            app.calc.set_boards(&a, &b);
                            for (i, &c) in a.iter().enumerate() {
                                app.fly_in(c, Slot { zone: Zone::Board, i }, i as u64 * 45);
                            }
                            for (i, &c) in b.iter().enumerate() {
                                app.fly_in(c, Slot { zone: Zone::Board2, i }, 120 + i as u64 * 45);
                            }
                            app.changed();
                        }
                        Reply::Failed(e) => app.toast(e, 2),
                        Reply::Cancelled => {}
                    }
                    app.sync();
                })
            }),
        );
        self.sync();
    }

    fn copy_results(&mut self) {
        let Some(r) = &self.run.result else { return };
        let eq = f64s(r, "equity");
        let (win, tie) = (f64s(r, "win"), f64s(r, "tie"));
        let dbl = r.get("double").and_then(Value::as_bool).unwrap_or(false);
        let mut lines: Vec<String> = self
            .calc
            .players
            .iter()
            .enumerate()
            .map(|(i, p)| {
                format!(
                    "P{} {:<18} equity {:.2}%  {} {:.2}%  tie {:.2}%",
                    i + 1,
                    calc::player_spec(p, self.calc.dbl),
                    eq.get(i).copied().unwrap_or(0.0),
                    if dbl { "scoop" } else { "win" },
                    win.get(i).copied().unwrap_or(0.0),
                    tie.get(i).copied().unwrap_or(0.0)
                )
            })
            .collect();
        let b = self.calc.board_str();
        if !b.is_empty() {
            lines.push(format!("board {b}"));
        }
        let b2 = self.calc.board2_str();
        if !b2.is_empty() {
            lines.push(format!("board B {b2}"));
        }
        let n = r.get("samples").and_then(Value::as_u64).unwrap_or(0);
        lines.push(if r.get("exact").and_then(Value::as_bool).unwrap_or(false) {
            format!("(exact, {} runouts)", fmt_int(n))
        } else {
            format!("(Monte Carlo, {} trials)", fmt_int(n))
        });
        match arboard::Clipboard::new().and_then(|mut c| c.set_text(lines.join("\n"))) {
            Ok(()) => self.toast("Results copied to clipboard", 1),
            Err(_) => self.toast("Could not access the clipboard", 2),
        }
    }

    /* ---------------- dealing ---------------- */

    fn pick(&mut self, card: u8) {
        match self.calc.pick(card) {
            Pick::Removed(from) => self.fly_out(card, from),
            Pick::Dealt { to, displaced } => {
                if let Some(old) = displaced {
                    self.fly_out(old, to);
                }
                self.fly_in(card, to, 0);
            }
            Pick::Nothing => {}
        }
        self.changed();
    }

    fn key(&mut self, text: &str, ctrl: bool) -> bool {
        match self.state.settings.view {
            0 => self.key_calc(text, ctrl),
            1 => {
                if text == "Enter" && self.eq.answered.is_some() {
                    self.q_deal();
                    return true;
                }
                false
            }
            2 => {
                let t = text.to_ascii_lowercase();
                let has_q = self.spr.q.is_some();
                if has_q && self.spr.choice.is_none() && (t == "s" || t == "f") {
                    self.s_answer(t == "s");
                    return true;
                }
                if self.spr.choice.is_some() && (text == "Enter" || text == " ") {
                    self.s_deal();
                    return true;
                }
                false
            }
            _ => false,
        }
    }

    fn key_calc(&mut self, text: &str, ctrl: bool) -> bool {
        if ctrl {
            if text.eq_ignore_ascii_case("v") {
                let txt = arboard::Clipboard::new().and_then(|mut c| c.get_text()).unwrap_or_default();
                let ids = calc::parse_loose(&txt);
                if !ids.is_empty() {
                    for (k, (c, s)) in self.calc.paste(&ids).into_iter().enumerate() {
                        self.fly_in(c, s, k as u64 * 40);
                    }
                    self.changed();
                }
                return true;
            }
            return false;
        }
        match text {
            "Enter" => {
                if !self.run.calculating {
                    self.calculate();
                }
                return true;
            }
            "Escape" => {
                self.pending_rank = None;
                if self.run.calculating {
                    self.cancel();
                }
                self.sync();
                return true;
            }
            "Backspace" | "Delete" => {
                if let Some((c, s)) = self.calc.clear_slot(None) {
                    self.fly_out(c, s);
                }
                self.changed();
                return true;
            }
            _ => {}
        }
        let mut chars = text.chars();
        let (Some(ch), None) = (chars.next(), chars.next()) else { return false };
        if let (Some(r), Some(s)) = (self.pending_rank, "cdhs".find(ch.to_ascii_lowercase())) {
            self.pending_rank = None;
            let id = (r * 4 + s) as u8;
            if self.calc.used()[id as usize] {
                self.toast(format!("{} is already in use", plo5::cards::card_str(id as usize)), 0);
                self.sync();
            } else {
                self.pick(id);
            }
            return true;
        }
        let up = if ch == '1' { 'T' } else { ch.to_ascii_uppercase() };
        if let Some(r) = "23456789TJQKA".find(up) {
            self.pending_rank = Some(r);
            self.sync();
            return true;
        }
        false
    }

    /* ---------------- rank table ---------------- */

    fn build_ranks(&mut self) {
        self.rank_job = Some((Instant::now(), 0, 1));
        self.host.gen_ranks(
            Box::new(|_, a, b| {
                on_ui(move |app| {
                    if let Some(j) = &mut app.rank_job {
                        j.1 = a;
                        j.2 = b;
                        app.sync();
                    }
                })
            }),
            Box::new(|reply| {
                on_ui(move |app| {
                    app.rank_job = None;
                    match reply {
                        Reply::Ok(v) => {
                            app.ranks_loaded = true;
                            let s = v["seconds"].as_f64().unwrap_or(0.0);
                            if v["saved"].as_bool().unwrap_or(false) {
                                app.toast(format!("Rank table ready ({s:.0} s)"), 1);
                            } else {
                                app.toast("Rank table built, but it could not be saved", 2);
                            }
                        }
                        Reply::Failed(e) => app.toast(e, 2),
                        Reply::Cancelled => {}
                    }
                    app.changed();
                })
            }),
        );
        self.sync();
    }

    fn cancel_ranks(&mut self) {
        self.rank_job = None;
        self.run.calculating = false;
        self.host.cancel_all();
        self.toast("Rank table build cancelled", 0);
        self.sync();
    }

    fn import_ranks(&mut self) {
        let Some(src) = rfd::FileDialog::new().set_title("Locate plo5rank.bin").add_filter("PLO5 rank table", &["bin"]).pick_file() else {
            return;
        };
        let dest = self.paths.data_dir.join("plo5rank.bin");
        if std::fs::canonicalize(&src).ok() != std::fs::canonicalize(&dest).ok() {
            if let Err(e) = std::fs::copy(&src, &dest) {
                self.toast(e.to_string(), 2);
                return;
            }
        }
        self.host.load_ranks(
            dest.clone(),
            Box::new(move |reply| {
                on_ui(move |app| {
                    match reply {
                        Reply::Ok(_) => {
                            app.ranks_loaded = true;
                            app.paths.ranks = dest;
                            app.ui().global::<Data>().set_ranks_path(app.paths.ranks.display().to_string().into());
                            app.toast("Rank table imported", 1);
                        }
                        Reply::Failed(e) => app.toast(e, 2),
                        Reply::Cancelled => {}
                    }
                    app.changed();
                })
            }),
        );
    }

    fn reveal(&self, which: &str) {
        let p = match which {
            "ranks" => self.paths.ranks.clone(),
            "config" => match &self.paths.config {
                Some(c) => c.clone(),
                None => return,
            },
            _ => return,
        };
        let dir = if p.is_file() { p.parent().map(|d| d.to_path_buf()).unwrap_or(p.clone()) } else { p.parent().map(|d| d.to_path_buf()).unwrap_or_default() };
        #[cfg(target_os = "windows")]
        let r = if p.is_file() {
            std::process::Command::new("explorer").arg(format!("/select,{}", p.display())).spawn()
        } else {
            std::process::Command::new("explorer").arg(&dir).spawn()
        };
        #[cfg(target_os = "macos")]
        let r = if p.is_file() { std::process::Command::new("open").arg("-R").arg(&p).spawn() } else { std::process::Command::new("open").arg(&dir).spawn() };
        #[cfg(not(any(target_os = "windows", target_os = "macos")))]
        let r = std::process::Command::new("xdg-open").arg(&dir).spawn();
        let _ = r;
    }

    /* ---------------- settings ---------------- */

    fn settings_changed(&mut self) {
        let ui = self.ui();
        let d = ui.global::<Data>();
        let s = &mut self.state.settings;
        s.precision = d.get_precision().clamp(0, 2) as usize;
        s.auto = d.get_auto_calc();
        s.build_ranks = d.get_build_ranks();
        s.four_color = ui.global::<Theme>().get_four_color();
        s.reduced_motion = ui.global::<Motion>().get_reduced();
        self.persist();
        self.changed();
    }

    /* ---------------- equity trainer ---------------- */

    fn q_deal(&mut self) {
        let ui = self.ui();
        let d = ui.global::<Data>();
        let mut rng = plo5::rng::Rng::new(seed());
        let mut deck: Vec<u8> = (0..52).collect();
        for i in 0..11 {
            let j = i + rng.below((52 - i) as u32) as usize;
            deck.swap(i, j);
        }
        let mut hero = deck[0..5].to_vec();
        hero.sort_unstable_by(|a, b| b.cmp(a));
        let dbl = d.get_q_dbl();
        let sel = d.get_q_opp_sel();
        let nopp = if sel > 0 { sel as usize } else { 1 + rng.below(5) as usize };
        self.eq = EqQuiz {
            token: self.eq.token + 1,
            hero: hero.clone(),
            flop: deck[5..8].to_vec(),
            flop2: deck[8..11].to_vec(),
            nopp,
            answered: None,
            result: None,
            scored: false,
            started: true,
        };
        let tok = self.eq.token;
        let players = std::iter::once(calc::cards_str(&hero.iter().map(|&c| Some(c)).collect::<Vec<_>>()))
            .chain(std::iter::repeat_n("random".to_string(), nopp))
            .collect::<Vec<_>>()
            .join(",");
        let flop = |v: &[u8]| v.iter().map(|&c| plo5::cards::card_str(c as usize)).collect::<String>();
        let q = format!(
            "players={}&board={}&board2={}&double={}&trials=500000&maxenum=0&buildranks=1",
            calc::enc(&players),
            flop(&self.eq.flop),
            if dbl { flop(&self.eq.flop2) } else { String::new() },
            u8::from(dbl)
        );
        d.set_q_num(d.get_q_num() + 1);
        d.set_q_warn(SharedString::new());
        self.host.call(
            "/api/equity",
            q,
            None,
            Box::new(move |reply| {
                on_ui(move |app| {
                    if app.eq.token != tok {
                        return;
                    }
                    match reply {
                        Reply::Ok(v) => app.eq.result = Some(v),
                        Reply::Failed(e) => app.ui().global::<Data>().set_q_err_text(e.into()),
                        Reply::Cancelled => {}
                    }
                    app.q_score();
                    app.sync_quiz();
                })
            }),
        );
        self.sync_quiz();
    }

    fn q_submit(&mut self, text: &str) {
        let ui = self.ui();
        let d = ui.global::<Data>();
        match text.trim().parse::<f64>() {
            Ok(v) if (0.0..=100.0).contains(&v) => {
                d.set_q_warn(SharedString::new());
                self.eq.answered = Some(v);
                self.q_score();
                self.sync_quiz();
                ui.invoke_refocus();
            }
            _ => d.set_q_warn("Enter your estimate between 0 and 100".into()),
        }
    }

    fn q_score(&mut self) {
        let (Some(g), Some(r)) = (self.eq.answered, &self.eq.result) else { return };
        if self.eq.scored {
            return;
        }
        self.eq.scored = true;
        let eq = f64s(r, "equity").first().copied().unwrap_or(0.0);
        let err = (g - eq).abs();
        let st = &mut self.state.stats;
        st.eq_n += 1;
        st.eq_sum += err;
        st.eq_best = Some(st.eq_best.map_or(err, |b| b.min(err)));
        self.persist();
    }

    fn sync_quiz(&self) {
        let ui = self.ui();
        let d = ui.global::<Data>();
        let ints = |v: &[u8]| ModelRc::new(VecModel::from(v.iter().map(|&c| c as i32).collect::<Vec<_>>()));
        let q = &self.eq;
        d.set_q_hero(ints(&q.hero));
        d.set_q_flop(ints(&q.flop));
        d.set_q_flop2(ints(&q.flop2));
        d.set_q_nopp(q.nopp as i32);
        d.set_q_answered(q.answered.is_some());
        d.set_q_guess(q.answered.unwrap_or(0.0) as f32);
        if q.result.is_none() && q.answered.is_none() {
            d.set_q_err_text(SharedString::new());
        }
        d.set_q_ready(q.result.is_some());
        if let Some(r) = &q.result {
            let eq = f64s(r, "equity").first().copied().unwrap_or(0.0);
            d.set_q_eq(eq as f32);
            if let Some(g) = q.answered {
                let e = (g - eq).abs();
                let (txt, kind) = match e {
                    e if e <= 1.5 => ("Perfect", 0),
                    e if e <= 3.0 => ("Excellent", 0),
                    e if e <= 6.0 => ("Good", 1),
                    e if e <= 10.0 => ("Rough", 1),
                    _ => ("Way off", 2),
                };
                d.set_q_grade(txt.into());
                d.set_q_grade_kind(kind);
            }
            let dbl = r.get("double").and_then(Value::as_bool).unwrap_or(false);
            let (w, t) = (f64s(r, "win").first().copied().unwrap_or(0.0), f64s(r, "tie").first().copied().unwrap_or(0.0));
            let pct = r.get("pct").and_then(|a| a.get(0)).and_then(Value::as_f64);
            let mut sub = format!("{} {w:.1}% · tie {t:.1}%", if dbl { "scoop" } else { "win" });
            if let Some(p) = pct {
                sub += &format!(" · your hand is the {} percentile {}", ordinal(p.round() as i64), if dbl { "across both flops" } else { "on this flop" });
            }
            d.set_q_sub(sub.into());
        }
        let st = &self.state.stats;
        d.set_q_stat_n(st.eq_n.to_string().into());
        d.set_q_stat_avg(if st.eq_n > 0 { format!("{:.1}pp", st.eq_sum / st.eq_n as f64) } else { "—".into() }.into());
        d.set_q_stat_best(st.eq_best.map_or("—".into(), |b| format!("{b:.1}pp")).into());
    }

    /* ---------------- stack-off trainer ---------------- */

    fn s_deal(&mut self) {
        let ui = self.ui();
        let d = ui.global::<Data>();
        self.spr = SprQuiz { token: self.spr.token + 1, q: None, choice: None, started: true };
        let tok = self.spr.token;
        let mut rng = plo5::rng::Rng::new(seed());
        let spr_sel = [0.0, 3.0, 5.0, 7.0, 10.0][d.get_s_spr_sel().clamp(0, 4) as usize];
        let spr = if spr_sel > 0.0 { spr_sel } else { ((3.0 + rng.below(1000) as f64 / 1000.0 * 7.0) * 2.0).round() / 2.0 };
        let sel = d.get_s_opp_sel();
        let nopp = if sel > 0 { sel as u32 } else { 1 + rng.below(5) };
        let off = if rng.below(2) == 0 { 1.0 } else { -1.0 } * d.get_s_margin() as f64;
        d.set_s_num(d.get_s_num() + 1);
        d.set_s_err(SharedString::new());
        self.host.call(
            "/api/random",
            "street=3&count=1".into(),
            None,
            Box::new(move |reply| {
                on_ui(move |app| {
                    if app.spr.token != tok {
                        return;
                    }
                    let board = match reply {
                        Reply::Ok(v) => v["board"].as_str().unwrap_or("").to_string(),
                        Reply::Failed(e) => {
                            app.ui().global::<Data>().set_s_err(e.into());
                            return app.sync_spr();
                        }
                        Reply::Cancelled => return,
                    };
                    let q = format!("board={board}&spr={spr}&opp={nopp}&offset={off}&pot=10");
                    app.host.call(
                        "/api/sprquiz",
                        q,
                        None,
                        Box::new(move |reply| {
                            on_ui(move |app| {
                                if app.spr.token != tok {
                                    return;
                                }
                                match reply {
                                    Reply::Ok(v) => app.spr.q = Some(v),
                                    Reply::Failed(e) => app.ui().global::<Data>().set_s_err(e.into()),
                                    Reply::Cancelled => {}
                                }
                                app.sync_spr();
                            })
                        }),
                    );
                })
            }),
        );
        self.sync_spr();
    }

    fn s_answer(&mut self, shove: bool) {
        let Some(q) = &self.spr.q else { return };
        if self.spr.choice.is_some() {
            return;
        }
        let good = q["equity"].as_f64().unwrap_or(0.0) >= q["eqNeeded"].as_f64().unwrap_or(0.0);
        self.spr.choice = Some(shove);
        let st = &mut self.state.stats;
        st.spr_n += 1;
        if shove == good {
            st.spr_right += 1;
            st.spr_streak += 1;
            st.spr_best = st.spr_best.max(st.spr_streak);
        } else {
            st.spr_streak = 0;
        }
        self.persist();
        self.sync_spr();
    }

    fn sync_spr(&self) {
        let ui = self.ui();
        let d = ui.global::<Data>();
        let ints = |v: &[u8]| ModelRc::new(VecModel::from(v.iter().map(|&c| c as i32).collect::<Vec<_>>()));
        d.set_s_loading(self.spr.q.is_none());
        if let Some(q) = &self.spr.q {
            let f = |k: &str| q[k].as_f64().unwrap_or(0.0);
            d.set_s_hand(ints(&cards_of(&q["hand"])));
            d.set_s_board(ints(&cards_of(&q["board"])));
            let opp = q["opp"].as_i64().unwrap_or(1);
            d.set_s_desc(
                format!(
                    "SPR {} · pot {:.0} · stacks {:.0} — {} it in with the top {:.0}% of hands{}.",
                    f("spr"),
                    f("pot"),
                    f("stack"),
                    if opp == 1 { "1 villain gets".to_string() } else { format!("{opp} villains get") },
                    f("mdf"),
                    if q["bandWidened"].as_bool().unwrap_or(false) { "*" } else { "" }
                )
                .into(),
            );
            let good = f("equity") >= f("eqNeeded");
            d.set_s_good(good);
            d.set_s_equity(f("equity") as f32);
            d.set_s_needed(f("eqNeeded") as f32);
            d.set_s_choice(self.spr.choice.map_or(-1, i32::from));
            d.set_s_correct(self.spr.choice == Some(good));
            let mut sub = format!(
                "{}{:.1}pp {} the stack-off point · villain range {:.0}–{:.0} pct{}",
                if good { "+" } else { "−" },
                (f("equity") - f("eqNeeded")).abs(),
                if good { "above" } else { "below" },
                f("bandLo"),
                f("bandHi"),
                if q["bandWidened"].as_bool().unwrap_or(false) { " (widened to stay dealable)" } else { "" }
            );
            if let (false, Some(bf)) = (good, q["breakevenFold"].as_f64()) {
                sub += &format!(" · a shove still breaks even with {bf:.0}% folds");
            }
            d.set_s_sub(sub.into());
        } else {
            d.set_s_choice(-1);
            d.set_s_desc(" ".into());
        }
        let st = &self.state.stats;
        d.set_s_stat_pct(if st.spr_n > 0 { format!("{}%", (100.0 * st.spr_right as f64 / st.spr_n as f64).round()) } else { "—".into() }.into());
        d.set_s_stat_streak(st.spr_streak.to_string().into());
        d.set_s_stat_best(st.spr_best.to_string().into());
    }
}

fn main() -> Result<(), slint::PlatformError> {
    let paths = persist::resolve();
    let mut state = persist::load_state(&paths.state);
    let (threads, presets) = persist::read_config(paths.config.as_deref());
    let host = EngineHost::start(threads, presets, paths.ranks.clone());

    let ui = AppWindow::new()?;
    let d = ui.global::<Data>();
    let s = state.settings.clone();
    d.set_view(s.view.min(3) as i32);
    d.set_precision(s.precision.min(2) as i32);
    d.set_auto_calc(s.auto);
    d.set_build_ranks(s.build_ranks);
    d.set_deck_open(s.deck_open);
    ui.global::<Theme>().set_four_color(s.four_color);
    ui.global::<Motion>().set_reduced(s.reduced_motion);
    d.set_presets(ModelRc::new(VecModel::from(
        ["Fast", "Balanced", "Precise"]
            .iter()
            .zip(presets.0)
            .map(|(n, t)| Preset { name: (*n).into(), sub: fmt_trials(t).into() })
            .collect::<Vec<_>>(),
    )));
    d.set_engine_ready(true);
    d.set_ncpu(host.threads as i32);
    d.set_engine_name(format!("Rust, built in ({} threads)", host.threads).into());
    d.set_version(env!("CARGO_PKG_VERSION").into());
    d.set_ranks_path(paths.ranks.display().to_string().into());
    d.set_config_path(paths.config.as_ref().map_or(String::new(), |p| p.display().to_string()).into());

    let calc = state.calc.take().map(Calc::revive).unwrap_or_default();
    let m_players = Rc::new(VecModel::<PlayerData>::default());
    let m_board = Rc::new(VecModel::from(vec![-1; 5]));
    let m_board2 = Rc::new(VecModel::from(vec![-1; 5]));
    let m_dead = Rc::new(VecModel::from(vec![-1; calc::DEAD_SLOTS]));
    let m_used = Rc::new(VecModel::from(vec![false; 52]));
    let m_hidden = Rc::new(VecModel::from(vec![false; 52]));
    let m_donut = Rc::new(VecModel::<DonutSeg>::default());
    d.set_players(ModelRc::from(m_players.clone()));
    d.set_board(ModelRc::from(m_board.clone()));
    d.set_board2(ModelRc::from(m_board2.clone()));
    d.set_dead(ModelRc::from(m_dead.clone()));
    d.set_used(ModelRc::from(m_used.clone()));
    d.set_hidden(ModelRc::from(m_hidden.clone()));
    d.set_donut(ModelRc::from(m_donut.clone()));
    d.set_flights(ModelRc::from(ANIM.with(|a| a.borrow().model.clone())));

    let ranks_loaded = host.ranks_loaded_at_start;
    APP.with(|cell| {
        *cell.borrow_mut() = Some(App {
            ui: ui.as_weak(),
            calc,
            host,
            paths,
            state,
            presets,
            ranks_loaded,
            run: Run::default(),
            auto_timer: Timer::default(),
            save_timer: Timer::default(),
            lookup_timer: Timer::default(),
            pending_rank: None,
            busy_board: false,
            pct_hand: None,
            pct_err: String::new(),
            pct_token: 0,
            eq_hand: None,
            eq_err: String::new(),
            eq_busy: false,
            rank_job: None,
            toasts: Vec::new(),
            toast_id: 0,
            eq: EqQuiz::default(),
            spr: SprQuiz::default(),
            m_players,
            rows: Vec::new(),
            leaving: Vec::new(),
            m_cards: HashMap::new(),
            m_ht: HashMap::new(),
            m_board,
            m_board2,
            m_dead,
            m_used,
            m_hidden,
            m_donut,
            last_active: None,
        })
    });

    /* ---- geometry reports (kept apart from App: they arrive mid-layout) */
    d.on_slot_geometry(|zone, row, i, x, y, w, h| {
        let rect = Rect { x, y, w, h };
        // translate the UI row to a player id now, while rows are current
        let key = APP.with(|cell| {
            let g = cell.try_borrow().ok()?;
            let app = g.as_ref()?;
            Some(match zone {
                0 => SlotKey::Player(*app.rows.get(row as usize)?, i as usize),
                1 => SlotKey::Board(i as usize),
                2 => SlotKey::Board2(i as usize),
                _ => SlotKey::Dead(i as usize),
            })
        });
        if let Some(key) = key {
            ANIM.with(|a| a.borrow_mut().slots.insert(key, rect));
            with_app(|app| app.update_ring(false));
        }
    });
    d.on_deck_geometry(|x, y| ANIM.with(|a| a.borrow_mut().deck = Some((x, y))));
    d.on_viewport_geometry({
        let w = ui.as_weak();
        move |x, y, vw, vh| {
            if let Some(ui) = w.upgrade() {
                let d = ui.global::<Data>();
                d.set_clip_x(x);
                d.set_clip_y(y);
                d.set_clip_w(vw);
                d.set_clip_h(vh);
            }
        }
    });

    /* ---- calculator */
    d.on_pick(|c| with_app(move |app| {
        app.pick(c as u8);
        app.ui().invoke_refocus();
    }));
    d.on_focus_slot(|z, row, i| with_app(move |app| {
        if let Some(s) = app.slot_of(z, row, i) {
            app.calc.focus(s);
            app.sync();
        }
        app.ui().invoke_refocus();
    }));
    d.on_clear_slot(|z, row, i| with_app(move |app| {
        let s = app.slot_of(z, row, i);
        if let Some((c, s)) = app.calc.clear_slot(s) {
            app.fly_out(c, s);
        }
        app.changed();
    }));
    d.on_key(|text, ctrl, _shift| {
        let text = text.to_string();
        let mut handled = false;
        APP.with(|cell| {
            if let Ok(mut g) = cell.try_borrow_mut() {
                if let Some(app) = g.as_mut() {
                    handled = app.key(&text, ctrl);
                }
            }
        });
        handled
    });
    d.on_set_mode(|row, mode| with_app(move |app| {
        if let Some(p) = app.player_of_row(row) {
            let old: Vec<(u8, Slot)> = app.calc.players[p].cards.iter().enumerate().filter_map(|(i, c)| c.map(|c| (c, Slot { zone: Zone::Player(p), i }))).collect();
            let m = match mode {
                1 => Mode::Range,
                2 => Mode::Random,
                _ => Mode::Hand,
            };
            if m != Mode::Hand {
                for (c, s) in old {
                    app.fly_out(c, s);
                }
            }
            app.calc.set_mode(p, m);
            app.changed();
        }
    }));
    d.on_set_range(|row, lo, hi| with_app(move |app| {
        if let Some(p) = app.player_of_row(row) {
            let pl = &mut app.calc.players[p];
            pl.lo = lo.clamp(0, 99) as f64;
            pl.hi = hi.clamp(1, 100) as f64;
            app.changed();
        }
    }));
    d.on_set_filter(|row, k, lo, hi| with_app(move |app| {
        if let Some(p) = app.player_of_row(row) {
            app.calc.players[p].chain[k.clamp(0, 1) as usize] = calc::Band { lo: lo.clamp(0, 99) as f64, hi: hi.clamp(1, 100) as f64 };
            app.changed();
        }
    }));
    d.on_toggle_filters(|row| with_app(move |app| {
        if let Some(p) = app.player_of_row(row) {
            let f = &mut app.calc.players[p].filters_open;
            *f = !*f;
            app.sync();
        }
    }));
    d.on_remove_player(|row| with_app(move |app| {
        if let Some(p) = app.player_of_row(row) {
            for (i, c) in app.calc.players[p].cards.iter().enumerate() {
                if let Some(c) = c {
                    app.fly_out(*c, Slot { zone: Zone::Player(p), i });
                }
            }
            app.calc.remove_player(p);
            app.changed();
        }
    }));
    d.on_clear_player(|row| with_app(move |app| {
        if let Some(p) = app.player_of_row(row) {
            for (i, c) in app.calc.players[p].cards.iter().enumerate() {
                if let Some(c) = c {
                    app.fly_out(*c, Slot { zone: Zone::Player(p), i });
                }
            }
            app.calc.clear_player(p);
            app.changed();
        }
    }));
    d.on_add_player(|| with_app(|app| {
        app.calc.add_player();
        app.changed();
    }));
    d.on_select_player(|row| with_app(move |app| {
        // the "deal to" picker passes a player index; rows match it when no row is leaving
        let p = if app.leaving.is_empty() { row as usize } else { app.player_of_row(row).unwrap_or(0) };
        if p < app.calc.players.len() {
            app.calc.sel_p = p;
            app.sync();
        }
    }));
    d.on_set_double(|v| with_app(move |app| {
        if !v {
            let old: Vec<(u8, usize)> = app.calc.board2.iter().enumerate().filter_map(|(i, c)| c.map(|c| (c, i))).collect();
            for (c, i) in old {
                app.fly_out(c, Slot { zone: Zone::Board2, i });
            }
        }
        app.calc.set_double(v);
        app.changed();
    }));
    d.on_deal_board(|n| with_app(move |app| app.deal_board(n as usize)));
    d.on_clear_boards(|| with_app(|app| {
        let old: Vec<(u8, Slot)> = app
            .calc
            .board
            .iter()
            .enumerate()
            .filter_map(|(i, c)| c.map(|c| (c, Slot { zone: Zone::Board, i })))
            .chain(app.calc.board2.iter().enumerate().filter_map(|(i, c)| c.map(|c| (c, Slot { zone: Zone::Board2, i }))))
            .collect();
        for (c, s) in old {
            app.fly_out(c, s);
        }
        app.calc.clear_boards();
        app.changed();
    }));
    d.on_clear_all(|| with_app(|app| {
        let used = app.calc.used();
        for c in 0..52u8 {
            if used[c as usize] {
                if let Some(s) = app.calc.find(c) {
                    app.fly_out(c, s);
                }
            }
        }
        app.calc.clear_all();
        app.changed();
    }));
    d.on_calculate(|| with_app(|app| app.calculate()));
    d.on_cancel(|| with_app(|app| app.cancel()));
    d.on_copy_results(|| with_app(|app| app.copy_results()));
    d.on_settings_changed(|| with_app(|app| app.settings_changed()));
    d.on_pct_changed(|| with_app(|app| app.schedule_lookup()));
    d.on_find_eq(|| with_app(|app| app.find_eq()));
    d.on_deal_tool(|which| with_app(move |app| {
        let hand = if which == 0 { app.pct_hand.as_ref().map(|h| h.0.clone()) } else { app.eq_hand.as_ref().map(|h| h.0.clone()) };
        if let Some(h) = hand {
            let p = app.calc.sel_p;
            for &c in &h {
                if let Some(s) = app.calc.find(c) {
                    app.fly_out(c, s);
                }
            }
            app.calc.deal_hand(p, &h);
            for (i, &c) in h.iter().enumerate() {
                app.fly_in(c, Slot { zone: Zone::Player(p), i }, i as u64 * 40);
            }
            app.toast(format!("Dealt to player {}", p + 1), 1);
            app.changed();
        }
    }));
    d.on_toggle_deck(|| with_app(|app| {
        app.state.settings.deck_open = !app.state.settings.deck_open;
        app.ui().global::<Data>().set_deck_open(app.state.settings.deck_open);
        app.persist();
    }));
    d.on_view_changed(|v| with_app(move |app| {
        app.state.settings.view = v.clamp(0, 3) as usize;
        app.persist();
        if v == 1 && !app.eq.started {
            app.q_deal();
        }
        if v == 2 && !app.spr.started {
            app.s_deal();
        }
        if v == 0 {
            app.schedule_auto();
            app.schedule_lookup();
        }
        if v == 1 {
            let ui = app.ui();
            let d = ui.global::<Data>();
            d.set_focus_guess(d.get_focus_guess() + 1);
        } else {
            app.ui().invoke_refocus();
        }
    }));

    /* ---- rank table + misc */
    d.on_build_ranks_now(|| with_app(|app| app.build_ranks()));
    d.on_cancel_ranks(|| with_app(|app| app.cancel_ranks()));
    d.on_import_ranks(|| with_app(|app| app.import_ranks()));
    d.on_reveal(|w| {
        let w = w.to_string();
        with_app(move |app| app.reveal(&w))
    });

    /* ---- trainers */
    d.on_q_deal(|| with_app(|app| app.q_deal()));
    d.on_q_submit(|t| {
        let t = t.to_string();
        with_app(move |app| app.q_submit(&t))
    });
    d.on_q_reset_stats(|| with_app(|app| {
        let st = &mut app.state.stats;
        st.eq_n = 0;
        st.eq_sum = 0.0;
        st.eq_best = None;
        app.persist();
        app.sync_quiz();
    }));
    d.on_s_deal(|| with_app(|app| app.s_deal()));
    d.on_s_answer(|shove| with_app(move |app| app.s_answer(shove)));
    d.on_s_reset_stats(|| with_app(|app| {
        let st = &mut app.state.stats;
        st.spr_n = 0;
        st.spr_right = 0;
        st.spr_streak = 0;
        st.spr_best = 0;
        app.persist();
        app.sync_spr();
    }));

    // first paint
    with_app(|app| {
        app.sync();
        app.sync_quiz();
        app.sync_spr();
        app.schedule_auto();
        app.schedule_lookup();
        let v = app.state.settings.view;
        if v == 1 {
            app.q_deal();
        } else if v == 2 {
            app.s_deal();
        }
    });

    ui.run()
}
