/* Equity calculator view. */

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { AnimatePresence, LayoutGroup, motion } from "motion/react";
import { useApp } from "../state.jsx";
import { call } from "../lib/engine.js";
import { load, save } from "../lib/storage.js";
import { MAX_PLAYERS, playerLabel, playerSpec } from "../lib/calcState.js";
import {
  cardsToStr, fmtInt, idToStr, parseLooseCards, PLAYER_COLORS, RANKS, strToIds, SUIT_SYM, SUITS,
} from "../lib/cards.js";
import { CardRow, Deck, HandChips } from "../components/Cards.jsx";
import { PlayerCard } from "../components/PlayerCard.jsx";
import { EquityDonut, HandTypesModal } from "../components/Charts.jsx";
import {
  Button, IconButton, Kbd, Panel, PctInput, ProgressRing, Segmented, softSpring, Spinner, spring, Switch,
} from "../components/ui.jsx";
import {
  IconAlert, IconBars, IconBolt, IconChevron, IconCopy, IconDice, IconMinus, IconPlus, IconStop, IconTrash, IconX,
} from "../components/Icons.jsx";

export default function Calculator({ onOpenSettings }) {
  const app = useApp();
  const { calc, dispatch, used, run, stale, settings, categories } = app;
  const [htOpen, setHtOpen] = useState(false);
  const [pendRank, setPendRank] = useState(null);
  const [deckOpen, setDeckOpen] = useState(() => load("deck-open", true));
  useEffect(() => save("deck-open", deckOpen), [deckOpen]);

  /* ---- keyboard: rank then suit deals a card; Enter calculates ---- */
  const kb = useRef(null);
  kb.current = (e) => {
    if (htOpen) return;
    const t = e.target;
    if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.isContentEditable)) return;
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key === "Enter") { e.preventDefault(); if (!run.calculating) app.calculate(); return; }
    if (e.key === "Escape") { setPendRank(null); if (run.calculating) app.cancel(); return; }
    if (e.key === "Backspace" || e.key === "Delete") { e.preventDefault(); dispatch({ type: "clearSlot" }); return; }
    const ch = e.key.length === 1 ? e.key : "";
    const si = SUITS.indexOf(ch.toLowerCase());
    if (pendRank != null && si >= 0) {
      e.preventDefault();
      const id = pendRank * 4 + si;
      setPendRank(null);
      if (!used.has(id)) dispatch({ type: "pick", card: id });
      else app.toast(`${idToStr(id)} is already in use`);
      return;
    }
    const ri = RANKS.indexOf(ch.toUpperCase() === "1" ? "T" : ch.toUpperCase());
    if (ri >= 0) { e.preventDefault(); setPendRank(ri); }
  };
  useEffect(() => {
    const onKey = (e) => kb.current(e);
    const onPaste = (e) => {
      const t = e.target;
      if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA")) return;
      const ids = parseLooseCards(e.clipboardData.getData("text"));
      if (ids.length) { e.preventDefault(); dispatch({ type: "paste", cards: ids }); }
    };
    window.addEventListener("keydown", onKey);
    window.addEventListener("paste", onPaste);
    return () => { window.removeEventListener("keydown", onKey); window.removeEventListener("paste", onPaste); };
  }, [dispatch]);

  /* ---- keep the slot being dealt into visible above the docked deck ---- */
  useEffect(() => {
    const raf = requestAnimationFrame(() => {
      const el = document.querySelector(".slot.active");
      const dock = document.querySelector(".deck-dock");
      const scroller = el && el.closest(".content");
      if (!el || !dock || !scroller) return;
      const r = el.getBoundingClientRect();
      const top = scroller.getBoundingClientRect().top;
      const limit = dock.getBoundingClientRect().top - 16;
      if (r.bottom > limit) scroller.scrollBy({ top: r.bottom - limit, behavior: "smooth" });
      else if (r.top < top + 8) scroller.scrollBy({ top: r.top - top - 16, behavior: "smooth" });
    });
    return () => cancelAnimationFrame(raf);
  }, [calc.active, deckOpen]);

  /* ---- results mapped onto the current players (by identity) ---- */
  const resFor = useMemo(() => {
    const r = run.result;
    if (!r || !run.players) return () => null;
    return (p) => {
      const k = run.players.findIndex((x) => x.id === p.id && x.mode === p.mode);
      if (k < 0 || k >= r.equity.length) return null;
      return {
        equity: r.equity[k], win: r.win[k], tie: r.tie[k], ci95: r.ci95[k],
        pct: r.pct ? r.pct[k] : null, handType: r.handType[k],
      };
    };
  }, [run.result, run.players]);

  const labels = calc.players.map((_, i) => `P${i + 1}`);
  const showResults = run.result && run.players && run.players.length === calc.players.length &&
    run.players.every((x, i) => x.id === calc.players[i].id);

  return (
    <div className="calc">
      <div className="calc-main">
        <TablePanel />
        <LayoutGroup>
          <Panel
            className="players-panel"
            title="Players"
            actions={
              <>
                <div className="stepper" aria-label="Number of players">
                  <IconButton icon={IconMinus} label="Remove last player" disabled={calc.players.length <= 2}
                    onClick={() => dispatch({ type: "removePlayer", p: calc.players.length - 1 })} />
                  <AnimatePresence mode="popLayout" initial={false}>
                    <motion.span key={calc.players.length} className="stepper-val"
                      initial={{ y: -10, opacity: 0 }} animate={{ y: 0, opacity: 1 }} exit={{ y: 10, opacity: 0 }}
                      transition={spring}>
                      {calc.players.length}
                    </motion.span>
                  </AnimatePresence>
                  <IconButton icon={IconPlus} label="Add player" disabled={calc.players.length >= MAX_PLAYERS}
                    onClick={() => dispatch({ type: "addPlayer" })} />
                </div>
                <span className="sep" />
                <Button icon={IconBars} onClick={() => setHtOpen(true)} disabled={!run.result}>Hand types</Button>
                <Button icon={IconCopy} onClick={() => copyResults(app)} disabled={!run.result}>Copy</Button>
                <Button icon={IconTrash} onClick={() => dispatch({ type: "clearAll" })}>Clear all</Button>
              </>
            }
          >
            <div className="player-list">
              <AnimatePresence initial={false} mode="popLayout">
                {calc.players.map((p, pi) => (
                  <PlayerCard
                    key={p.id}
                    p={p}
                    pi={pi}
                    color={PLAYER_COLORS[pi]}
                    active={calc.active}
                    selected={calc.selP === pi}
                    dispatch={dispatch}
                    dbl={calc.dbl}
                    res={resFor(p)}
                    stale={stale}
                    calculating={run.calculating}
                    canRemove={calc.players.length > 2}
                    categories={categories}
                    onHandTypes={() => setHtOpen(true)}
                  />
                ))}
              </AnimatePresence>
              {calc.players.length < MAX_PLAYERS && (
                <motion.button layout type="button" className="add-player" onClick={() => dispatch({ type: "addPlayer" })}
                  whileTap={{ scale: 0.98 }} transition={softSpring}>
                  <IconPlus size={16} /> Add player
                </motion.button>
              )}
            </div>
            <Notices />
          </Panel>
        </LayoutGroup>

        <section className={`deck-dock ${deckOpen ? "" : "collapsed"}`} aria-label="Card picker">
          <div className="deck-head">
            <button type="button" className="deck-toggle" onClick={() => setDeckOpen((v) => !v)}
              aria-expanded={deckOpen} title={deckOpen ? "Hide the deck (typing still deals cards)" : "Show the deck"}>
              <h2>Deck</h2>
              <motion.span className="chev" animate={{ rotate: deckOpen ? 0 : 180 }} transition={spring}>
                <IconChevron size={14} />
              </motion.span>
            </button>
            <AnimatePresence>
              {pendRank != null && (
                <motion.span className="pending-rank" initial={{ opacity: 0, scale: 0.8 }} animate={{ opacity: 1, scale: 1 }}
                  exit={{ opacity: 0, scale: 0.8 }} transition={spring}>
                  {RANKS[pendRank] === "T" ? "10" : RANKS[pendRank]}
                  <span className="blink">_</span> now type a suit: <Kbd>c</Kbd><Kbd>d</Kbd><Kbd>h</Kbd><Kbd>s</Kbd>
                </motion.span>
              )}
            </AnimatePresence>
            <span className="deck-hint">
              {calc.active ? <>Dealing to <b>{slotName(calc.active)}</b></> : "Select a slot to deal into"}
            </span>
          </div>
          <AnimatePresence initial={false}>
            {deckOpen && (
              <motion.div key="deck" className="deck-body" initial={{ height: 0, opacity: 0 }}
                animate={{ height: "auto", opacity: 1 }} exit={{ height: 0, opacity: 0 }} transition={softSpring}>
                <Deck used={used} onPick={(id) => dispatch({ type: "pick", card: id })} />
              </motion.div>
            )}
          </AnimatePresence>
        </section>
      </div>

      <aside className="calc-side">
        <RunPanel onOpenSettings={onOpenSettings} />
        <Panel title="Pot share" className="summary-panel">
          <AnimatePresence mode="wait" initial={false}>
            {showResults ? (
              <motion.div key="donut" className="summary" initial={{ opacity: 0, scale: 0.94 }}
                animate={{ opacity: 1, scale: 1 }} exit={{ opacity: 0, scale: 0.94 }} transition={softSpring}>
                <EquityDonut shares={run.result.equity} colors={PLAYER_COLORS} labels={labels} stale={stale} />
                <ul className="summary-legend">
                  {calc.players.map((p, i) => (
                    <li key={p.id}>
                      <i style={{ background: PLAYER_COLORS[i] }} />
                      <span className="who">P{i + 1}</span>
                      <span className="what">{playerLabel(p, calc.dbl)}</span>
                      <b>{run.result.equity[i].toFixed(1)}%</b>
                    </li>
                  ))}
                </ul>
              </motion.div>
            ) : (
              <motion.div key="empty" className="summary-empty" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
                <div className="donut-ghost" />
                <span>Run a calculation to see each player's share of the pot.</span>
              </motion.div>
            )}
          </AnimatePresence>
        </Panel>
        <ToolsPanel />
        <ShortcutsPanel />
      </aside>

      <HandTypesModal open={htOpen} onClose={() => setHtOpen(false)} result={run.result}
        labels={labels} colors={PLAYER_COLORS} categories={categories} />
    </div>
  );
}

function slotName(a) {
  if (a.zone === "p") return `Player ${a.p + 1} · card ${a.i + 1}`;
  if (a.zone === "b") return `Board · ${["flop", "flop", "flop", "turn", "river"][a.i]}`;
  if (a.zone === "b2") return `Board B · ${["flop", "flop", "flop", "turn", "river"][a.i]}`;
  return `Dead card ${a.i + 1}`;
}

function copyResults({ run, calc, toast }) {
  const r = run.result;
  if (!r) return;
  const lines = calc.players.map((p, i) =>
    `P${i + 1} ${playerSpec(p, calc.dbl).padEnd(18)} equity ${r.equity[i].toFixed(2)}%  ` +
    `${r.double ? "scoop" : "win"} ${r.win[i].toFixed(2)}%  tie ${r.tie[i].toFixed(2)}%`);
  const b = cardsToStr(calc.board), b2 = calc.dbl ? cardsToStr(calc.board2) : "";
  if (b) lines.push(`board ${b}`);
  if (b2) lines.push(`board B ${b2}`);
  lines.push(r.exact ? `(exact, ${fmtInt(r.samples)} runouts)` : `(Monte Carlo, ${fmtInt(r.samples)} trials)`);
  navigator.clipboard.writeText(lines.join("\n")).then(
    () => toast("Results copied to clipboard", "ok"),
    () => toast("Could not access the clipboard", "error"),
  );
}

/* ---------------------------------------------------------------- */

const STREETS = ["Flop", "Flop", "Flop", "Turn", "River"];

function TablePanel() {
  const { calc, dispatch, toast } = useApp();
  const [busy, setBusy] = useState(false);

  const randomBoard = async (street) => {
    const keep = [];
    for (const p of calc.players) if (p.mode === "hand") for (const c of p.cards) if (c != null) keep.push(c);
    for (const c of calc.dead) if (c != null) keep.push(c);
    setBusy(true);
    try {
      const j = await call("/api/random", {
        street, count: calc.dbl ? 2 : 1, used: keep.map(idToStr).join(""),
      });
      dispatch({ type: "setBoards", board: strToIds(j.board) || [], board2: strToIds(j.board2) || [] });
    } catch (e) {
      if (!e.cancelled) toast(e.message, "error");
    } finally {
      setBusy(false);
    }
  };

  return (
    <Panel
      className="table-panel"
      title="Table"
      actions={
        <>
          <Segmented
            size="sm"
            value={calc.dbl}
            layoutKey="board-mode"
            onChange={(v) => dispatch({ type: "setDouble", value: v })}
            options={[
              { value: false, label: "Single board" },
              { value: true, label: "Double board", title: "Two boards, each pays half the pot" },
            ]}
          />
          <span className="sep" />
          <span className="dim small">Deal</span>
          {[3, 4, 5].map((n) => (
            <Button key={n} icon={n === 3 ? IconDice : undefined} disabled={busy} onClick={() => randomBoard(n)}>
              {n === 3 ? "Flop" : n === 4 ? "Turn" : "River"}
            </Button>
          ))}
          <IconButton icon={IconX} label="Clear board" onClick={() => dispatch({ type: "clearBoards" })} />
        </>
      }
    >
      <div className="felt">
        <div className="felt-boards">
          <BoardGroup label={calc.dbl ? "Board A" : "Board"} cards={calc.board} zone="b" />
          <AnimatePresence initial={false}>
            {calc.dbl && (
              <motion.div
                className="board-b-wrap"
                initial={{ opacity: 0, width: 0, x: -12 }}
                animate={{ opacity: 1, width: "auto", x: 0 }}
                exit={{ opacity: 0, width: 0, x: -12 }}
                transition={softSpring}
              >
                <BoardGroup label="Board B" cards={calc.board2} zone="b2" />
              </motion.div>
            )}
          </AnimatePresence>
        </div>
        <div className="dead-group">
          <div className="group-lbl">Dead cards</div>
          <CardRow cards={calc.dead} zone="d" active={calc.active} dispatch={dispatch} size="xs" label="Dead" />
        </div>
      </div>
    </Panel>
  );
}

function BoardGroup({ label, cards, zone }) {
  const { calc, dispatch } = useApp();
  return (
    <div className="board-group">
      <div className="group-lbl">{label}</div>
      <CardRow cards={cards} zone={zone} active={calc.active} dispatch={dispatch} size={calc.dbl ? "sm" : "md"} label={label}
        placeholders={STREETS} />
    </div>
  );
}

/* ---------------------------------------------------------------- */

function Notices() {
  const { hint, run, stale, settings } = useApp();
  let n = null;
  if (run.error) n = { kind: "error", text: run.error };
  else if (hint) n = { kind: "hint", text: hint };
  else if (stale && !settings.auto) n = { kind: "stale", text: <>Results are out of date — press <Kbd>Enter</Kbd> or Calculate</> };
  return (
    <AnimatePresence initial={false} mode="wait">
      {n && (
        <motion.div
          key={n.kind + String(typeof n.text === "string" ? n.text : "")}
          className={`notice notice-${n.kind}`}
          initial={{ opacity: 0, height: 0, marginTop: 0 }}
          animate={{ opacity: 1, height: "auto", marginTop: 12 }}
          exit={{ opacity: 0, height: 0, marginTop: 0 }}
          transition={softSpring}
        >
          <div className="notice-in">
            {n.kind === "error" && <IconAlert size={15} />}
            <span>{n.text}</span>
          </div>
        </motion.div>
      )}
    </AnimatePresence>
  );
}

function RunPanel({ onOpenSettings }) {
  const { settings, setSettings, presets, run, calculate, cancel, hint, ranksLoaded, rankJob, status } = useApp();
  const r = run.result;
  const progress = run.progress;
  return (
    <Panel title="Run" className="run-panel">
      <Segmented
        value={settings.precision}
        layoutKey="precision"
        className="precision"
        onChange={(v) => setSettings({ precision: v })}
        options={presets.map((p, i) => ({
          value: i,
          label: p.name,
          sub: `${p.trials >= 1e6 ? p.trials / 1e6 + "M" : p.trials / 1e3 + "k"} trials`,
        }))}
      />
      <div className="switches">
        <Switch label="Auto-calculate" hint="Recalculate as you edit" checked={settings.auto}
          onChange={(v) => setSettings({ auto: v })} />
        <Switch label="Board percentiles" hint="Rank hands on the board (first run per board is slower)"
          checked={settings.buildRanks} onChange={(v) => setSettings({ buildRanks: v })} />
      </div>

      <motion.button
        type="button"
        layout
        className={`calc-btn ${run.calculating ? "running" : ""}`}
        disabled={!run.calculating && !!hint}
        onClick={run.calculating ? cancel : calculate}
        whileTap={{ scale: 0.97 }}
        transition={spring}
      >
        <AnimatePresence mode="popLayout" initial={false}>
          {run.calculating ? (
            <motion.span key="stop" className="calc-btn-in" initial={{ y: 14, opacity: 0 }} animate={{ y: 0, opacity: 1 }}
              exit={{ y: -14, opacity: 0 }} transition={spring}>
              <IconStop size={16} /> Cancel <small>Esc</small>
            </motion.span>
          ) : (
            <motion.span key="go" className="calc-btn-in" initial={{ y: 14, opacity: 0 }} animate={{ y: 0, opacity: 1 }}
              exit={{ y: -14, opacity: 0 }} transition={spring}>
              <IconBolt size={16} /> Calculate <small>Enter</small>
            </motion.span>
          )}
        </AnimatePresence>
        {run.calculating && (
          <motion.span className="calc-btn-progress"
            initial={{ scaleX: 0 }}
            animate={progress ? { scaleX: progress.done / Math.max(1, progress.total) } : { scaleX: [0.05, 0.9] }}
            transition={progress ? softSpring : { duration: 2.4, ease: "easeOut" }} />
        )}
      </motion.button>

      <div className="run-meta">
        <AnimatePresence mode="wait" initial={false}>
          {run.calculating ? (
            <motion.span key="busy" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
              <Spinner size={12} /> {progress ? `${capital(progress.what)} ${Math.round((100 * progress.done) / Math.max(1, progress.total))}%` : "Running simulation…"}
            </motion.span>
          ) : r ? (
            <motion.span key={run.key} initial={{ opacity: 0, y: 4 }} animate={{ opacity: 1, y: 0 }} exit={{ opacity: 0 }}>
              <b>{r.exact ? "Exact" : "Monte Carlo"}</b> · {fmtInt(r.samples)} {r.exact ? "runouts" : "trials"} · {r.ms < 1 ? "<1 ms" : r.ms < 1000 ? `${r.ms.toFixed(0)} ms` : `${(r.ms / 1000).toFixed(1)} s`}
            </motion.span>
          ) : (
            <motion.span key="none" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
              {status ? `${status.ncpu} engine threads ready` : "Starting engine…"}
            </motion.span>
          )}
        </AnimatePresence>
      </div>

      <AnimatePresence initial={false}>
        {status && !ranksLoaded && (
          <motion.div className="callout" initial={{ opacity: 0, height: 0 }} animate={{ opacity: 1, height: "auto" }}
            exit={{ opacity: 0, height: 0 }} transition={softSpring}>
            <div className="callout-in">
              {rankJob ? (
                <>
                  <ProgressRing value={rankJob.done / Math.max(1, rankJob.total)} size={34} stroke={3.5} />
                  <div>
                    <b>Building the preflop rank table…</b>
                    <span>{Math.floor((100 * rankJob.done) / Math.max(1, rankJob.total))}% — you can keep working meanwhile</span>
                  </div>
                </>
              ) : (
                <>
                  <IconAlert size={16} />
                  <div>
                    <b>Preflop percentiles are off</b>
                    <span>Build the rank table once to use preflop ranges.</span>
                    <button type="button" className="link" onClick={onOpenSettings}>Set it up →</button>
                  </div>
                </>
              )}
            </div>
          </motion.div>
        )}
      </AnimatePresence>
    </Panel>
  );
}

const capital = (s) => (s ? s[0].toUpperCase() + s.slice(1) : s);

/* ---------------------------------------------------------------- */

function ToolsPanel() {
  const { calc, dispatch, toast } = useApp();
  const [pct, setPct] = useState(30);
  const [pctHand, setPctHand] = useState(null);
  const [eq, setEq] = useState(55);
  const [eqHand, setEqHand] = useState(null);
  const [eqBusy, setEqBusy] = useState(false);
  const boardStr = cardsToStr(calc.board);
  const board2Str = calc.dbl ? cardsToStr(calc.board2) : "";
  const nb = boardStr.length / 2, nb2 = board2Str.length / 2;
  const boardOk = nb !== 1 && nb !== 2 && nb2 !== 1 && nb2 !== 2;

  useEffect(() => {
    if (!boardOk) { setPctHand(null); return; }
    let live = true;
    const t = setTimeout(async () => {
      try {
        const j = await call("/api/lookup", { pct, board: nb >= 3 ? boardStr : "", board2: nb2 >= 3 && nb >= 3 ? board2Str : "" });
        if (live) setPctHand(j);
      } catch (e) {
        if (live && !e.cancelled) setPctHand({ error: e.message });
      }
    }, 700);
    return () => { live = false; clearTimeout(t); };
  }, [pct, boardStr, board2Str, nb, nb2, boardOk]);

  const findEq = async () => {
    setEqBusy(true);
    setEqHand(null);
    try {
      setEqHand(await call("/api/findeq", { target: eq, board: boardStr, board2: board2Str, double: calc.dbl }));
    } catch (e) {
      if (!e.cancelled) setEqHand({ error: e.message });
    }
    setEqBusy(false);
  };

  const deal = (hand) => {
    const ids = strToIds(hand);
    if (!ids) return;
    dispatch({ type: "dealHand", p: calc.selP, cards: ids });
    toast(`Dealt to player ${calc.selP + 1}`, "ok");
  };

  const street = nb >= 3 ? (calc.dbl && nb2 >= 3 ? "both boards" : ["", "", "", "flop", "turn", "river"][nb]) : "preflop";

  return (
    <Panel title="Hand finder" className="tools-panel">
      <div className="tool">
        <div className="tool-q">
          <span>Hand at percentile</span>
          <PctInput value={pct} onChange={setPct} aria-label="Percentile" />
          <span className="dim small">{street}</span>
        </div>
        <ToolResult item={pctHand} onDeal={deal} selP={calc.selP} />
      </div>
      <div className="tool">
        <div className="tool-q">
          <span>Hand with equity</span>
          <PctInput value={eq} onChange={setEq} aria-label="Target equity" />
          <span className="dim small">vs random</span>
          <Button variant="soft" onClick={findEq} disabled={eqBusy || !boardOk}>
            {eqBusy ? <Spinner size={12} /> : null}Find
          </Button>
        </div>
        <ToolResult item={eqHand} onDeal={deal} selP={calc.selP} extra={eqHand && eqHand.equity != null ? `${eqHand.equity.toFixed(1)}%` : null} />
      </div>
      <div className="deal-target">
        Deal to
        <Segmented size="xs" value={calc.selP} layoutKey="deal-target"
          onChange={(p) => dispatch({ type: "selectPlayer", p })}
          options={calc.players.map((_, i) => ({ value: i, label: `P${i + 1}` }))} />
      </div>
    </Panel>
  );
}

function ToolResult({ item, onDeal, selP, extra }) {
  return (
    <div className="tool-a">
      <AnimatePresence mode="wait" initial={false}>
        {item && item.hand ? (
          <motion.div key={item.hand} className="tool-hand" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
            <HandChips hand={item.hand} ids={strToIds(item.hand)} />
            {extra && <span className="pill">{extra}</span>}
            <button type="button" className="link" onClick={() => onDeal(item.hand)}>Deal → P{selP + 1}</button>
          </motion.div>
        ) : item && item.error ? (
          <motion.span key="err" className="tool-err" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
            {item.error}
          </motion.span>
        ) : (
          <motion.span key="none" className="dim small" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>—</motion.span>
        )}
      </AnimatePresence>
    </div>
  );
}

function ShortcutsPanel() {
  return (
    <Panel title="Shortcuts" className="shortcuts">
      <ul>
        <li><span><Kbd>A</Kbd><Kbd>h</Kbd></span> deal A{SUIT_SYM.h} to the selected slot</li>
        <li><span><Kbd>Ctrl</Kbd><Kbd>V</Kbd></span> paste a hand, e.g. “AhKhQd Jc Tc”</li>
        <li><span><Kbd>⌫</Kbd></span> clear the selected slot</li>
        <li><span><Kbd>Enter</Kbd></span> calculate · <Kbd>Esc</Kbd> cancel</li>
        <li><span>Right-click</span> a card to remove it</li>
      </ul>
    </Panel>
  );
}
