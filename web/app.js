/* PLO5 Equity Lab — React UI (no build step: React UMD + htm) */
/* global React, ReactDOM, htm */
"use strict";

const html = htm.bind(React.createElement);
const { useState, useEffect, useRef, useMemo, useCallback } = React;

/* ------------------------------------------------------------------ */
/* Card helpers (same encoding as the engine: id = rank*4 + suit)      */
/* ------------------------------------------------------------------ */

const RANKS = "23456789TJQKA";
const SUITS = "cdhs";
const SUITSYM = { c: "♣", d: "♦", h: "♥", s: "♠" };
const PCOLORS = ["#60a5fa", "#f87171", "#4ade80", "#fbbf24", "#c084fc", "#2dd4bf"];

const idRank = (id) => RANKS[id >> 2];
const idSuit = (id) => SUITS[id & 3];
const idToStr = (id) => idRank(id) + idSuit(id);
const cardsToStr = (arr) => arr.filter((c) => c != null).map(idToStr).join("");

function strToIds(s) {
  const out = [];
  const t = s.replace(/[\s,]/g, "");
  for (let i = 0; i + 1 < t.length; i += 2) {
    const r = RANKS.indexOf(t[i].toUpperCase());
    const u = SUITS.indexOf(t[i + 1].toLowerCase());
    if (r < 0 || u < 0) return null;
    out.push(r * 4 + u);
  }
  return out;
}

/* fallback presets; /api/status serves per-machine calibrated ones
 * (written by plo5setup into plo5config.ini) */
const PRECISIONS = [
  { name: "Fast", trials: 200000, maxenum: 20000 },
  { name: "Balanced", trials: 1000000, maxenum: 200000 },
  { name: "Precise", trials: 10000000, maxenum: 2000000 },
];

const newPlayer = () => ({
  mode: "hand",
  cards: [null, null, null, null, null],
  lo: 0, hi: 100,
  chain: [{ lo: 0, hi: 100 }, { lo: 0, hi: 100 }],
});

const fmtInt = (n) => Number(n).toLocaleString("en-US");

/* ------------------------------------------------------------------ */
/* Small components                                                    */
/* ------------------------------------------------------------------ */

function CardFace({ id, used, onClick, small }) {
  return html`<div
    className=${"card s-" + idSuit(id) + (used ? " used" : "")}
    onClick=${onClick}
    title=${idToStr(id)}
  >${idRank(id)}${SUITSYM[idSuit(id)]}</div>`;
}

function Slot({ card, active, onClick, ph }) {
  if (card != null)
    return html`<div className=${"slot filled card s-" + idSuit(card) + (active ? " active" : "")}
      onClick=${onClick}>${idRank(card)}${SUITSYM[idSuit(card)]}</div>`;
  return html`<div className=${"slot" + (active ? " active" : "")} onClick=${onClick}>${ph || ""}</div>`;
}

function HandText({ str }) {
  const ids = strToIds(str) || [];
  return html`<span className="handtext">
    ${ids.map((id, i) => html`<span key=${i} className=${"s-" + idSuit(id)}>${idRank(id)}${SUITSYM[idSuit(id)]}</span>`)}
  </span>`;
}

function Deck({ usedSet, onPick }) {
  const rows = [3, 2, 1, 0]; /* spades, hearts, diamonds, clubs */
  const cells = [];
  for (const s of rows)
    for (let col = 0; col < 13; col++) {
      const id = (12 - col) * 4 + s;
      cells.push(html`<${CardFace} key=${id} id=${id} used=${usedSet.has(id)}
        onClick=${() => onPick(id)} />`);
    }
  return html`<div className="deck">${cells}</div>`;
}

function RangeSlider({ lo, hi, color, onChange }) {
  const ref = useRef(null);
  const drag = useRef(null);
  const pctOf = (ev) => {
    const r = ref.current.getBoundingClientRect();
    return Math.max(0, Math.min(100, Math.round(((ev.clientX - r.left) / r.width) * 100)));
  };
  const onDown = (ev) => {
    const p = pctOf(ev);
    drag.current = Math.abs(p - lo) <= Math.abs(p - hi) ? "lo" : "hi";
    ev.currentTarget.setPointerCapture(ev.pointerId);
    apply(p);
  };
  const apply = (p) => {
    if (drag.current === "lo") onChange(Math.min(p, hi), hi);
    else onChange(lo, Math.max(p, lo));
  };
  const onMove = (ev) => { if (drag.current) apply(pctOf(ev)); };
  const onUp = () => { drag.current = null; };
  return html`<div className="rslider" ref=${ref} style=${{ color }}
    onPointerDown=${onDown} onPointerMove=${onMove} onPointerUp=${onUp}>
    <div className="track" />
    <div className="band" style=${{ left: lo + "%", width: hi - lo + "%", background: color }} />
    <div className="thumb" style=${{ left: lo + "%" }} />
    <div className="thumb" style=${{ left: hi + "%" }} />
  </div>`;
}

function HTMini({ dist, cats, onClick }) {
  if (!dist) return null;
  return html`<div className="htbar" title="Hand type distribution — click for details" onClick=${onClick}>
    ${dist.map((v, i) =>
      v >= 0.25
        ? html`<div key=${i} className=${"cat" + i} style=${{ width: v + "%" }}
            title=${cats[i] + " " + v.toFixed(1) + "%"} />`
        : null)}
  </div>`;
}

function HTBig({ dist, cats }) {
  return html`<div className="htbig">
    ${dist.map((v, i) =>
      v >= 0.15
        ? html`<div key=${i} className=${"cat" + i} style=${{ width: v + "%" }}
            title=${cats[i] + " " + v.toFixed(1) + "%"} />`
        : null)}
  </div>`;
}

function HandTypesModal({ result, playerLabels, cats, onClose }) {
  const boards = result.double ? [["Board A", result.handType], ["Board B", result.handTypeB]]
                               : [[null, result.handType]];
  return html`<div className="overlay" onClick=${(e) => { if (e.target === e.currentTarget) onClose(); }}>
    <div className="modal">
      <button className="btn small ghost close" onClick=${onClose}>Close</button>
      <h3>Hand Type Distribution</h3>
      <div className="legend">
        ${cats.map((c, i) => html`<span key=${i}><i className=${"cat" + i} /> ${c}</span>`)}
      </div>
      ${boards.map(([bname, ht], bi) => html`<div key=${bi}>
        ${bname && html`<h2 style=${{ marginTop: 14 }}>${bname}</h2>`}
        ${ht.map((dist, p) => html`<div key=${p}>
          <div style=${{ fontWeight: 650, fontSize: 13, marginTop: 10 }} className=${"p" + p}>
            Player ${p + 1} ${playerLabels[p] ? " — " + playerLabels[p] : ""}
          </div>
          <${HTBig} dist=${dist} cats=${cats} />
        </div>`)}
        <table className="httable">
          <thead><tr><th>Player</th>${cats.map((c, i) => html`<th key=${i}>${c}</th>`)}</tr></thead>
          <tbody>
            ${ht.map((dist, p) => html`<tr key=${p}>
              <td className=${"p" + p}>P${p + 1}</td>
              ${dist.map((v, i) => html`<td key=${i}>${v < 0.005 ? "—" : v.toFixed(2) + "%"}</td>`)}
            </tr>`)}
          </tbody>
        </table>
      </div>`)}
    </div>
  </div>`;
}

/* ------------------------------------------------------------------ */
/* Equity quiz (port of the desktop plo5quiz trainer)                  */
/* ------------------------------------------------------------------ */

function dealQuiz(noppSel, dbl) {
  const deck = [...Array(52).keys()];
  for (let i = 0; i < 11; i++) {
    const j = i + Math.floor(Math.random() * (52 - i));
    [deck[i], deck[j]] = [deck[j], deck[i]];
  }
  return {
    hero: deck.slice(0, 5).sort((a, b) => b - a),
    flop: deck.slice(5, 8),
    flop2: deck.slice(8, 11),
    nopp: noppSel || 1 + Math.floor(Math.random() * 5),
    dbl,
  };
}

function gradeOf(err) {
  if (err <= 1.5) return ["perfect", "qgood"];
  if (err <= 3) return ["excellent", "qgood"];
  if (err <= 6) return ["good", "qmid"];
  if (err <= 10) return ["rough", "qmid"];
  return ["way off", "qbad"];
}

function QCards({ ids }) {
  return html`<div className="qcards">
    ${ids.map((id, i) => html`<div key=${i} className=${"card static s-" + idSuit(id)}>
      ${idRank(id)}${SUITSYM[idSuit(id)]}</div>`)}
  </div>`;
}

function EquityQuiz() {
  const [dbl, setDbl] = useState(false);
  const [noppSel, setNoppSel] = useState(0); /* 0 = random, 1..5 fixed */
  const [q, setQ] = useState(() => dealQuiz(0, false));
  const [qnum, setQnum] = useState(1);
  const [phase, setPhase] = useState(0); /* 0 = thinking, 1 = reveal */
  const [guess, setGuess] = useState("");
  const [gval, setGval] = useState(null);
  const [res, setRes] = useState(null);
  const [hint, setHint] = useState(null);
  const [stats, setStats] = useState({ n: 0, sum: 0, best: Infinity, worst: 0 });
  const gen = useRef(0);
  const scored = useRef(false);
  const inputRef = useRef(null);

  const deal = (sel, d) => {
    setQ(dealQuiz(sel, d));
    setQnum((n) => n + 1);
    setPhase(0);
    setGuess("");
    setGval(null);
    setHint(null);
    scored.current = false;
    setTimeout(() => inputRef.current && inputRef.current.focus(), 0);
  };

  /* compute truth in the background while the user thinks */
  useEffect(() => {
    const my = ++gen.current;
    setRes(null);
    const players = [cardsToStr(q.hero), ...Array(q.nopp).fill("random")].join(",");
    const params = new URLSearchParams({
      players,
      board: cardsToStr(q.flop),
      board2: q.dbl ? cardsToStr(q.flop2) : "",
      double: q.dbl ? 1 : 0,
      trials: 500000,
      maxenum: 0,
      buildranks: 1,
    });
    fetch("/api/equity?" + params)
      .then((r) => r.json())
      .then((j) => { if (my === gen.current && !j.error) setRes(j); })
      .catch(() => {});
  }, [q]);

  const applyScore = (g, eq) => {
    const err = Math.abs(g - eq);
    setStats((s) => ({
      n: s.n + 1, sum: s.sum + err,
      best: Math.min(s.best, err), worst: Math.max(s.worst, err),
    }));
    scored.current = true;
  };

  const submit = () => {
    const v = parseFloat(guess);
    if (guess.trim() === "" || isNaN(v) || v < 0 || v > 100) {
      setHint("Enter your equity estimate as 0–100");
      return;
    }
    setHint(null);
    setGval(v);
    setPhase(1);
    if (res) applyScore(v, res.equity[0]);
  };

  /* guess was submitted before the result arrived */
  useEffect(() => {
    if (res && phase === 1 && gval != null && !scored.current)
      applyScore(gval, res.equity[0]);
    // eslint-disable-next-line
  }, [res]);

  const doEnter = () => { if (phase === 0) submit(); else deal(noppSel, dbl); };
  const enterRef = useRef(doEnter);
  enterRef.current = doEnter;

  useEffect(() => {
    const onKey = (ev) => {
      if (ev.key === "Enter") { ev.stopPropagation(); enterRef.current(); }
    };
    window.addEventListener("keydown", onKey, true);
    return () => window.removeEventListener("keydown", onKey, true);
  }, []);

  useEffect(() => { inputRef.current && inputRef.current.focus(); }, []);

  const eq = res ? res.equity[0] : null;
  const err = res && gval != null ? Math.abs(gval - eq) : null;
  const [gtxt, gcls] = err != null ? gradeOf(err) : ["", ""];
  const winLbl = q.dbl ? "scoop" : "win";

  return html`<div>
      <h3>Equity estimate — Question ${qnum}</h3>
      <div className="qstats">
        ${stats.n > 0
          ? `answered ${stats.n} · avg error ${(stats.sum / stats.n).toFixed(1)}pp · best ${stats.best.toFixed(1)} · worst ${stats.worst.toFixed(1)}`
          : "estimate your equity, press Enter"}
      </div>

      <div className="qsettings">
        <div className="seg mini">
          <button className=${!dbl ? "on" : ""} onClick=${() => { setDbl(false); deal(noppSel, false); }}>Single board</button>
          <button className=${dbl ? "on" : ""} onClick=${() => { setDbl(true); deal(noppSel, true); }}>Double board · split</button>
        </div>
        <span className="qopplbl">Opponents</span>
        <div className="seg mini">
          ${[0, 1, 2, 3, 4, 5].map((i) => html`<button key=${i}
            className=${noppSel === i ? "on" : ""}
            onClick=${() => { setNoppSel(i); deal(i, dbl); }}>${i === 0 ? "Rnd" : i}</button>`)}
        </div>
      </div>

      <div className="qlbl">Your hand</div>
      <${QCards} ids=${q.hero} />

      <div className="qlbl">
        ${q.dbl
          ? `Flop A — vs ${q.nopp} random opponent${q.nopp === 1 ? "" : "s"} · each board pays half`
          : `Flop — vs ${q.nopp} random opponent${q.nopp === 1 ? "" : "s"} (full range)`}
      </div>
      <${QCards} ids=${q.flop} />
      ${q.dbl && html`
        <div className="qlbl">Flop B</div>
        <${QCards} ids=${q.flop2} />`}

      <div className="qinput">
        <span>Your equity estimate (%):</span>
        <input ref=${inputRef} type="number" min="0" max="100" step="0.1"
          inputMode="decimal" enterKeyHint="go"
          value=${guess} disabled=${phase === 1}
          onChange=${(e) => setGuess(e.target.value)} />
        <button className=${"btn" + (phase === 0 ? " primary" : "")} onClick=${doEnter}>
          ${phase === 0 ? "Submit" : "Next hand"}
        </button>
      </div>
      ${hint && html`<div className="qhint">${hint}</div>`}

      ${phase === 1 && html`<div className="qreveal">
        ${!res && html`<div className="qcomputing"><span className="spin" />computing…</div>`}
        ${res && html`
          <div className="qbar">
            <div className="qfill" style=${{ width: Math.max(0, Math.min(100, eq)) + "%" }} />
            <div className=${"qmark " + gcls} style=${{ left: Math.max(0, Math.min(100, gval)) + "%" }} />
          </div>
          <div className="qbig">Equity ${eq.toFixed(1)}%</div>
          <div className=${"qgrade " + gcls}>you said ${gval.toFixed(1)} — error ${err.toFixed(1)}pp (${gtxt})</div>
          <div className="qsub">
            ${winLbl} ${res.win[0].toFixed(1)}% · tie ${res.tie[0].toFixed(1)}%${res.pct && res.pct[0] != null
              ? ` · your hand is the ${res.pct[0].toFixed(0)}th percentile ${q.dbl ? "across both flops" : "on this flop"}`
              : ""}
          </div>`}
      </div>`}

      <div className="qfoot"><span className="kbd">Enter</span> = submit / next question · <span className="kbd">Esc</span> closes</div>
  </div>`;
}

/* ------------------------------------------------------------------ */
/* Stack-off quiz: shove or fold at the dealt SPR? Hands are dealt a   */
/* chosen distance (in equity pp) from the stack-off threshold.        */
/* ------------------------------------------------------------------ */

function SprQuiz() {
  const [sprSel, setSprSel] = useState(0);   /* 0 = random 3-10 */
  const [noppSel, setNoppSel] = useState(0); /* 0 = random 1-5 */
  const [margin, setMargin] = useState(5);   /* pp from the stack-off point */
  const [qnum, setQnum] = useState(0);
  const [q, setQ] = useState(null);
  const [phase, setPhase] = useState(0);     /* 0 dealing, 1 answer, 2 reveal */
  const [choice, setChoice] = useState(null);
  const [stats, setStats] = useState({ n: 0, right: 0, streak: 0, best: 0 });
  const [err, setErr] = useState(null);
  const gen = useRef(0);
  const marginRef = useRef(margin);
  marginRef.current = margin;

  const deal = useCallback(async (sSel, oSel) => {
    const my = ++gen.current;
    setPhase(0); setChoice(null); setErr(null); setQ(null);
    setQnum((n) => n + 1);
    const spr = sSel || Math.round((3 + Math.random() * 7) * 2) / 2;
    const nopp = oSel || 1 + Math.floor(Math.random() * 5);
    const off = (Math.random() < 0.5 ? 1 : -1) * marginRef.current;
    try {
      const rf = await (await fetch("/api/random?street=3&count=1")).json();
      if (rf.error) { if (my === gen.current) setErr(rf.error); return; }
      const params = new URLSearchParams({ board: rf.board, spr, opp: nopp,
        offset: off, pot: 10 });
      const j = await (await fetch("/api/sprquiz?" + params)).json();
      if (my !== gen.current) return;
      if (j.error) { setErr(j.error); return; }
      setQ(j);
      setPhase(1);
    } catch {
      if (my === gen.current) setErr("server unreachable — is plo5web.exe running?");
    }
  }, []);

  useEffect(() => { deal(sprSel, noppSel); /* eslint-disable-line */ }, []);

  const answer = (shove) => {
    if (phase !== 1 || !q) return;
    const good = q.equity >= q.eqNeeded;
    setChoice(shove);
    setStats((s) => {
      const ok = shove === good;
      const streak = ok ? s.streak + 1 : 0;
      return { n: s.n + 1, right: s.right + (ok ? 1 : 0),
               streak, best: Math.max(s.best, streak) };
    });
    setPhase(2);
  };

  const kb = useRef(null);
  kb.current = (ev) => {
    if (ev.target.tagName === "INPUT") return;
    const k = ev.key.toLowerCase();
    if (phase === 1 && k === "s") answer(true);
    else if (phase === 1 && k === "f") answer(false);
    else if (phase === 2 && (ev.key === "Enter" || k === " ")) deal(sprSel, noppSel);
  };
  useEffect(() => {
    const onKey = (ev) => kb.current(ev);
    window.addEventListener("keydown", onKey, true);
    return () => window.removeEventListener("keydown", onKey, true);
  }, []);

  const heroIds = q ? strToIds(q.hand) : null;
  const flopIds = q ? strToIds(q.board) : null;
  const good = q ? q.equity >= q.eqNeeded : false;
  const correct = choice != null && choice === good;

  return html`<div>
    <h3>Stack-off quiz — Question ${qnum}</h3>
    <div className="qstats">
      ${stats.n > 0
        ? `answered ${stats.n} · correct ${stats.right} (${(100 * stats.right / stats.n).toFixed(0)}%) · streak ${stats.streak} · best ${stats.best}`
        : "flop stack-off: shove or fold? press S / F"}
    </div>

    <div className="qsettings">
      <span className="qseglbl">SPR</span>
      <div className="seg mini">
        ${[0, 3, 5, 7, 10].map((v) => html`<button key=${v}
          className=${sprSel === v ? "on" : ""}
          onClick=${() => { setSprSel(v); deal(v, noppSel); }}>${v === 0 ? "Rnd" : v}</button>`)}
      </div>
      <span className="qseglbl">Opponents</span>
      <div className="seg mini">
        ${[0, 1, 2, 3, 4, 5].map((i) => html`<button key=${i}
          className=${noppSel === i ? "on" : ""}
          onClick=${() => { setNoppSel(i); deal(sprSel, i); }}>${i === 0 ? "Rnd" : i}</button>`)}
      </div>
      <label className="qmargin">
        hands dealt
        <input type="number" min="1" max="20" step="0.5" value=${margin}
          inputMode="decimal"
          onChange=${(e) => setMargin(Math.max(1, Math.min(20, +e.target.value || 1)))} />
        pp from the stack-off point
        <span className="qdim">(smaller = harder · applies from the next hand)</span>
      </label>
    </div>

    ${err && html`<div className="errbox" style=${{ textAlign: "left" }}>${err}</div>`}
    ${phase === 0 && !err && html`<div className="qcomputing" style=${{ margin: "44px 0" }}>
      <span className="spin" />dealing a hand near the stack-off point…</div>`}

    ${q && html`
      <div className="qlbl">Flop · SPR ${q.spr} · pot ${q.pot.toFixed(0)} · stacks ${q.stack.toFixed(0)}</div>
      <${QCards} ids=${flopIds} />
      <div className="qlbl">
        Your hand — ${q.opp} villain${q.opp === 1 ? "" : "s"} stack${q.opp === 1 ? "s" : ""} off
        the top ${q.mdf.toFixed(0)}% of hands${q.bandWidened ? "*" : ""}
      </div>
      <${QCards} ids=${heroIds} />

      ${phase === 1 && html`<div className="qanswers">
        <button className="btn primary" onClick=${() => answer(true)}>Stack off · S</button>
        <button className="btn" onClick=${() => answer(false)}>Fold · F</button>
      </div>`}

      ${phase === 2 && html`<div className="qreveal">
        <div className=${"qbig " + (correct ? "qgood" : "qbad")}>
          ${correct ? "Correct" : "Wrong"} — ${good ? "stack off" : "fold"}
        </div>
        <div className="qbar">
          <div className="qfill" style=${{ width: Math.min(100, q.equity) + "%" }} />
          <div className=${"qmark " + (good ? "qgood" : "qbad")}
            style=${{ left: Math.min(100, q.eqNeeded) + "%" }} />
        </div>
        <div className="qgrade">
          your equity ${q.equity.toFixed(1)}% vs ${q.eqNeeded.toFixed(1)}% needed —
          ${good ? "+" : "−"}${Math.abs(q.equity - q.eqNeeded).toFixed(1)}pp
          ${good ? "above" : "below"} the stack-off point
        </div>
        <div className="qsub">
          villain range: top ${q.mdf.toFixed(1)}% (band ${q.bandLo.toFixed(0)}–${q.bandHi.toFixed(0)}
          ${q.bandWidened ? ", widened to stay dealable" : ""})
          ${!good && q.breakevenFold != null
            ? ` · a shove still breaks even with ${q.breakevenFold.toFixed(0)}% folds`
            : ""}
        </div>
        <button className="btn primary" style=${{ marginTop: 14 }}
          onClick=${() => deal(sprSel, noppSel)}>Next hand · Enter</button>
      </div>`}
    `}
  </div>`;
}

function QuizModal({ onClose }) {
  const [mode, setMode] = useState("eq");
  useEffect(() => {
    const onKey = (ev) => { if (ev.key === "Escape") onClose(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);
  return html`<div className="overlay" onClick=${(e) => { if (e.target === e.currentTarget) onClose(); }}>
    <div className="modal quiz">
      <button className="btn small ghost close" onClick=${onClose}>Close</button>
      <div className="qmodebar">
        <div className="seg mini">
          <button className=${mode === "eq" ? "on" : ""} onClick=${() => setMode("eq")}>Equity estimate</button>
          <button className=${mode === "spr" ? "on" : ""} onClick=${() => setMode("spr")}>Stack-off</button>
        </div>
      </div>
      ${mode === "eq" ? html`<${EquityQuiz} />` : html`<${SprQuiz} />`}
    </div>
  </div>`;
}

/* ------------------------------------------------------------------ */
/* Main app                                                            */
/* ------------------------------------------------------------------ */

function App() {
  const [players, setPlayers] = useState([newPlayer(), newPlayer()]);
  const [board, setBoard] = useState(Array(5).fill(null));
  const [board2, setBoard2] = useState(Array(5).fill(null));
  const [dead, setDead] = useState(Array(8).fill(null));
  const [dbl, setDbl] = useState(false);
  const [prec, setPrec] = useState(1);
  const [auto, setAuto] = useState(false);
  const [buildRanks, setBuildRanks] = useState(true);
  const [active, setActive] = useState({ zone: "p", p: 0, i: 0 });
  const [selP, setSelP] = useState(0);
  const [result, setResult] = useState(null);
  const [resultQuery, setResultQuery] = useState(null);
  const [calculating, setCalculating] = useState(false);
  const [error, setError] = useState(null);
  const [status, setStatus] = useState({ ranks: false, ncpu: 1, categories: [] });
  const [htModal, setHtModal] = useState(false);
  const [quiz, setQuiz] = useState(false);
  const [toast, setToast] = useState(null);
  const pendRank = useRef(-1);
  const reqId = useRef(0);

  /* tools state */
  const [pctQ, setPctQ] = useState(30);
  const [pctHand, setPctHand] = useState(null);
  const [eqQ, setEqQ] = useState(55);
  const [eqHand, setEqHand] = useState(null);
  const [eqBusy, setEqBusy] = useState(false);

  useEffect(() => {
    fetch("/api/status").then((r) => r.json()).then(setStatus).catch(() => {});
  }, []);

  /* ---- card occupancy ---- */
  const usedSet = useMemo(() => {
    const s = new Set();
    for (const p of players) if (p.mode === "hand") for (const c of p.cards) if (c != null) s.add(c);
    for (const c of board) if (c != null) s.add(c);
    if (dbl) for (const c of board2) if (c != null) s.add(c);
    for (const c of dead) if (c != null) s.add(c);
    return s;
  }, [players, board, board2, dead, dbl]);

  const removeCard = useCallback((id) => {
    setPlayers((ps) => ps.map((p) => p.mode === "hand" && p.cards.includes(id)
      ? { ...p, cards: p.cards.map((c) => (c === id ? null : c)) } : p));
    setBoard((b) => b.map((c) => (c === id ? null : c)));
    setBoard2((b) => b.map((c) => (c === id ? null : c)));
    setDead((b) => b.map((c) => (c === id ? null : c)));
  }, []);

  /* deal order: hand players' slots, board A, board B (double) */
  const nextActive = useCallback((from) => {
    const order = [];
    players.forEach((p, pi) => {
      if (p.mode === "hand")
        for (let i = 0; i < 5; i++) order.push({ zone: "p", p: pi, i, filled: p.cards[i] != null });
    });
    for (let i = 0; i < 5; i++) order.push({ zone: "b", i, filled: board[i] != null });
    if (dbl) for (let i = 0; i < 5; i++) order.push({ zone: "b2", i, filled: board2[i] != null });

    if (from && from.zone === "d") {
      for (let i = from.i + 1; i < 8; i++) if (dead[i] == null) return { zone: "d", i };
      return null;
    }
    let idx = -1;
    if (from)
      idx = order.findIndex((o) => o.zone === from.zone && o.i === from.i &&
        (from.zone !== "p" || o.p === from.p));
    for (let k = idx + 1; k < order.length; k++)
      if (!order[k].filled) return { zone: order[k].zone, p: order[k].p, i: order[k].i };
    return null;
  }, [players, board, board2, dead, dbl]);

  const assignCard = useCallback((id) => {
    if (usedSet.has(id)) { removeCard(id); return; }
    if (!active) return;
    if (active.zone === "p") {
      const p = players[active.p];
      if (!p || p.mode !== "hand") return;
      setPlayers((ps) => ps.map((pp, i) => i === active.p
        ? { ...pp, cards: pp.cards.map((c, j) => (j === active.i ? id : c)) } : pp));
      setSelP(active.p);
    } else if (active.zone === "b") {
      setBoard((b) => b.map((c, j) => (j === active.i ? id : c)));
    } else if (active.zone === "b2") {
      setBoard2((b) => b.map((c, j) => (j === active.i ? id : c)));
    } else if (active.zone === "d") {
      setDead((b) => b.map((c, j) => (j === active.i ? id : c)));
    }
    /* advance is computed on the next render's state via effect below */
    setAdvanceFrom({ ...active });
  }, [usedSet, active, players, removeCard]);

  const [advanceFrom, setAdvanceFrom] = useState(null);
  useEffect(() => {
    if (!advanceFrom) return;
    setActive(nextActive(advanceFrom));
    setAdvanceFrom(null);
  }, [advanceFrom, nextActive]);

  /* ---- keyboard entry ---- */
  useEffect(() => {
    const onKey = (ev) => {
      if (quiz) return;
      if (ev.target.tagName === "INPUT" || ev.target.tagName === "SELECT") return;
      if (ev.key === "Enter") { calcRef.current(); return; }
      if (ev.key === "Delete" || ev.key === "Backspace") {
        if (active) {
          if (active.zone === "p") setPlayers((ps) => ps.map((pp, i) => i === active.p
            ? { ...pp, cards: pp.cards.map((c, j) => (j === active.i ? null : c)) } : pp));
          else if (active.zone === "b") setBoard((b) => b.map((c, j) => (j === active.i ? null : c)));
          else if (active.zone === "b2") setBoard2((b) => b.map((c, j) => (j === active.i ? null : c)));
          else if (active.zone === "d") setDead((b) => b.map((c, j) => (j === active.i ? null : c)));
        }
        return;
      }
      const ch = ev.key.toLowerCase();
      const si = SUITS.indexOf(ch);
      if (pendRank.current >= 0 && si >= 0) {
        const id = pendRank.current * 4 + si;
        pendRank.current = -1;
        if (!usedSet.has(id)) assignCard(id);
        return;
      }
      const ri = RANKS.indexOf(ch.toUpperCase());
      if (ri >= 0) pendRank.current = ri;
      if (ev.key === "Escape") pendRank.current = -1;
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [active, usedSet, assignCard, quiz]);

  /* ---- spec building + validation ---- */
  const boardStr = cardsToStr(board);
  const board2Str = dbl ? cardsToStr(board2) : "";
  const deadStr = cardsToStr(dead);
  const nb = boardStr.length / 2, nb2 = board2Str.length / 2;

  const playerSpec = (p) => {
    if (p.mode === "random") return "random";
    if (p.mode === "range") {
      const f = p.chain[0], t = p.chain[1];
      const fOn = f.lo > 0 || f.hi < 100, tOn = t.lo > 0 || t.hi < 100;
      let s = "";
      if (!dbl && tOn) s = `${f.lo}-${f.hi}>${t.lo}-${t.hi}>`;
      else if (!dbl && fOn) s = `${f.lo}-${f.hi}>`;
      return s + `${p.lo}-${p.hi}`;
    }
    return cardsToStr(p.cards);
  };

  const hint = useMemo(() => {
    if (nb === 1 || nb === 2) return `Board${dbl ? " A" : ""} needs 3, 4 or 5 cards (or none)`;
    if (dbl && (nb2 === 1 || nb2 === 2)) return "Board B needs 3, 4 or 5 cards (or none)";
    for (let i = 0; i < players.length; i++) {
      const p = players[i];
      if (p.mode === "hand") {
        const n = p.cards.filter((c) => c != null).length;
        if (n < 5) return `Player ${i + 1} needs ${5 - n} more card${n === 4 ? "" : "s"} — click the deck or type e.g. A then H for A${SUITSYM.h}`;
      } else if (p.mode === "range") {
        if (p.hi <= p.lo) return `Player ${i + 1}: empty range`;
        if (p.chain[0].hi <= p.chain[0].lo || p.chain[1].hi <= p.chain[1].lo)
          return `Player ${i + 1}: empty street filter`;
        if (nb === 0 && (!dbl || nb2 === 0) && !status.ranks)
          return "Preflop ranges need the rank table (plo5calc --gen-ranks)";
        if (dbl && (nb > 0) !== (nb2 > 0))
          return "Double-board ranges need both boards set (or both empty)";
      }
    }
    return null;
  }, [players, nb, nb2, dbl, status.ranks]);

  const precs = status.presets && status.presets.length === 3
    ? status.presets : PRECISIONS;

  const query = useMemo(() => {
    if (hint) return null;
    const q = new URLSearchParams({
      players: players.map(playerSpec).join(","),
      board: boardStr, board2: board2Str, dead: deadStr,
      double: dbl ? 1 : 0,
      trials: precs[prec].trials,
      maxenum: precs[prec].maxenum,
      buildranks: buildRanks ? 1 : 0,
      seed: 3141592,
    });
    return q.toString();
    // eslint-disable-next-line
  }, [players, boardStr, board2Str, deadStr, dbl, prec, buildRanks, hint, precs]);

  const stale = result != null && query != null && query !== resultQuery;

  /* ---- calculate ---- */
  const calculate = useCallback(async () => {
    if (!query) return;
    const my = ++reqId.current;
    setCalculating(true);
    setError(null);
    try {
      const r = await fetch("/api/equity?" + query);
      const j = await r.json();
      if (my !== reqId.current) return;
      if (j.error) setError(j.error);
      else { setResult(j); setResultQuery(query); }
    } catch (e) {
      if (my === reqId.current) setError("server unreachable — is plo5web.exe still running?");
    } finally {
      if (my === reqId.current) setCalculating(false);
    }
  }, [query]);
  const calcRef = useRef(calculate);
  useEffect(() => { calcRef.current = calculate; }, [calculate]);

  /* auto mode: debounce */
  useEffect(() => {
    if (!auto || !query || query === resultQuery) return;
    const t = setTimeout(() => calcRef.current(), 350);
    return () => clearTimeout(t);
  }, [auto, query, resultQuery]);

  /* ---- tools ---- */
  useEffect(() => {
    const t = setTimeout(async () => {
      try {
        const q = new URLSearchParams({ pct: pctQ, board: nb >= 3 ? boardStr : "",
          board2: dbl && nb2 >= 3 ? board2Str : "" });
        const j = await (await fetch("/api/lookup?" + q)).json();
        setPctHand(j.error ? null : j);
      } catch { setPctHand(null); }
    }, 350);
    return () => clearTimeout(t);
  }, [pctQ, boardStr, board2Str, nb, nb2, dbl]);

  const findEq = async () => {
    setEqBusy(true);
    setEqHand(null);
    try {
      const q = new URLSearchParams({ target: eqQ, board: boardStr,
        board2: dbl ? board2Str : "", double: dbl ? 1 : 0 });
      const j = await (await fetch("/api/findeq?" + q)).json();
      setEqHand(j.error ? { error: j.error } : j);
    } catch { setEqHand({ error: "request failed" }); }
    setEqBusy(false);
  };

  const randomBoard = async (street) => {
    const used = [];
    for (const p of players) if (p.mode === "hand") for (const c of p.cards) if (c != null) used.push(c);
    for (const c of dead) if (c != null) used.push(c);
    const q = new URLSearchParams({ street, count: dbl ? 2 : 1,
      used: used.map(idToStr).join("") });
    try {
      const j = await (await fetch("/api/random?" + q)).json();
      if (j.error) return;
      const b = strToIds(j.board) || [];
      setBoard(Array(5).fill(null).map((_, i) => b[i] ?? null));
      if (dbl) {
        const b2 = strToIds(j.board2) || [];
        setBoard2(Array(5).fill(null).map((_, i) => b2[i] ?? null));
      }
    } catch {}
  };

  const dealTo = (handStr, pi) => {
    const ids = strToIds(handStr);
    if (!ids || ids.length !== 5) return;
    for (const id of ids) removeCard(id);
    setPlayers((ps) => ps.map((p, i) => i === pi
      ? { ...p, mode: "hand", cards: [...ids] } : p));
    setSelP(pi);
  };

  const copyResults = () => {
    if (!result) return;
    const lines = players.map((p, i) =>
      `P${i + 1} ${playerSpec(p).padEnd(16)} equity ${result.equity[i].toFixed(2)}%` +
      `  win ${result.win[i].toFixed(2)}%  tie ${result.tie[i].toFixed(2)}%`);
    if (boardStr) lines.push(`board ${boardStr}`);
    if (dbl && board2Str) lines.push(`board B ${board2Str}`);
    lines.push(result.exact ? `(exact, ${fmtInt(result.samples)} runouts)`
                            : `(Monte Carlo, ${fmtInt(result.samples)} trials)`);
    navigator.clipboard.writeText(lines.join("\n"));
    setToast("Copied to clipboard");
    setTimeout(() => setToast(null), 1800);
  };

  /* ---- mutators ---- */
  const setPlayerCount = (n) => {
    setPlayers((ps) => {
      const out = ps.slice(0, n);
      while (out.length < n) out.push(newPlayer());
      return out;
    });
    if (selP >= n) setSelP(0);
  };

  const setMode = (pi, mode) => {
    setPlayers((ps) => ps.map((p, i) => i === pi
      ? { ...p, mode, cards: mode === "hand" ? p.cards : [null, null, null, null, null] } : p));
    if (mode === "hand") setActive({ zone: "p", p: pi, i: 0 });
    setSelP(pi);
  };

  const patchPlayer = (pi, patch) =>
    setPlayers((ps) => ps.map((p, i) => (i === pi ? { ...p, ...patch } : p)));

  const clearPlayer = (pi) => {
    setPlayers((ps) => ps.map((p, i) => i === pi
      ? { ...p, cards: [null, null, null, null, null] } : p));
    setActive({ zone: "p", p: pi, i: 0 });
  };

  const clearAll = () => {
    setPlayers((ps) => ps.map((p) => ({ ...p, cards: [null, null, null, null, null] })));
    setBoard(Array(5).fill(null));
    setBoard2(Array(5).fill(null));
    setDead(Array(8).fill(null));
    setActive({ zone: "p", p: 0, i: 0 });
  };

  const toggleDbl = (v) => {
    setDbl(v);
    if (!v) setBoard2(Array(5).fill(null));
  };

  const isActive = (zone, i, p) =>
    active && active.zone === zone && active.i === i && (zone !== "p" || active.p === p);

  const cats = status.categories.length ? status.categories :
    ["High Card", "Pair", "Two Pair", "Trips", "Straight", "Flush", "Full House", "Quads", "Straight Flush"];

  /* ------------------------------------------------------------------ */

  return html`<div className="app">
    <div className="topbar">
      <div className="brand">
        <img src="/logo.svg" alt="" />
        <div>
          <h1>PLO5 Equity Lab</h1>
          <span className="sub">5-card Omaha · range analyzer</span>
        </div>
      </div>
      <div className="seg">
        <button className=${!dbl ? "on" : ""} onClick=${() => toggleDbl(false)}>Single board</button>
        <button className=${dbl ? "on" : ""} onClick=${() => toggleDbl(true)}>Double board · split</button>
      </div>
      <select value=${prec} onChange=${(e) => setPrec(+e.target.value)}>
        ${precs.map((p, i) => html`<option key=${i} value=${i}>${p.name} — ${fmtInt(p.trials)} trials</option>`)}
      </select>
      <label className="check">
        <input type="checkbox" checked=${auto} onChange=${(e) => setAuto(e.target.checked)} /> Auto
      </label>
      <button className="btn" onClick=${() => setQuiz(true)}>Quiz</button>
      <button className="btn primary" disabled=${!!hint} onClick=${calculate}>
        ${calculating && html`<span className="spin" />`}Calculate
      </button>
    </div>

    <div className="grid">
      <div>
        <div className="panel">
          <h2>Board
            <span className="spacer" />
            <span style=${{ color: "var(--text-dim)", fontWeight: 500, textTransform: "none", letterSpacing: 0 }}>random:</span>
            <button className="btn small ghost" onClick=${() => randomBoard(3)}>Flop</button>
            <button className="btn small ghost" onClick=${() => randomBoard(4)}>Turn</button>
            <button className="btn small ghost" onClick=${() => randomBoard(5)}>River</button>
          </h2>
          <div className="boardrow">
            <div className="boardgroup">
              <div className="lbl">${dbl ? "Board A" : "Board"}</div>
              <div className="cardrow">
                ${board.map((c, i) => html`<${Slot} key=${i} card=${c}
                  active=${isActive("b", i)} onClick=${() => setActive({ zone: "b", i })} />`)}
              </div>
            </div>
            ${dbl && html`<div className="boardgroup">
              <div className="lbl">Board B</div>
              <div className="cardrow">
                ${board2.map((c, i) => html`<${Slot} key=${i} card=${c}
                  active=${isActive("b2", i)} onClick=${() => setActive({ zone: "b2", i })} />`)}
              </div>
            </div>`}
            <div className="boardgroup">
              <div className="lbl">Dead</div>
              <div className="cardrow">
                ${dead.map((c, i) => html`<${Slot} key=${i} card=${c}
                  active=${isActive("d", i)} onClick=${() => setActive({ zone: "d", i })} />`)}
              </div>
            </div>
          </div>
        </div>

        <div className="panel">
          <h2>Players
            <span className="seg mini" style=${{ marginLeft: 4 }}>
              <button onClick=${() => players.length > 2 && setPlayerCount(players.length - 1)}>−</button>
              <button className="on">${players.length}</button>
              <button onClick=${() => players.length < 6 && setPlayerCount(players.length + 1)}>+</button>
            </span>
            <span className="spacer" />
            ${result && html`<span style=${{ color: "var(--text-dim)", fontWeight: 500, textTransform: "none", letterSpacing: 0 }}>
              ${result.exact ? `Exact — ${fmtInt(result.samples)} runouts` : `Monte Carlo — ${fmtInt(result.samples)} trials`} · ${result.ms.toFixed(0)} ms
            </span>`}
            <button className="btn small ghost" onClick=${copyResults} disabled=${!result}>Copy</button>
            <button className="btn small ghost" onClick=${() => setHtModal(true)} disabled=${!result}>Hand types</button>
            <button className="btn small ghost" onClick=${clearAll}>Clear all</button>
          </h2>

          ${players.map((p, pi) => {
            const color = PCOLORS[pi];
            const res = result && !error && pi < result.equity.length ? result : null;
            return html`<div className="player" key=${pi}>
              <div className=${"pname p" + pi}>P${pi + 1}</div>
              <div className="seg mini" style=${{ alignSelf: "start", marginTop: 14 }}>
                <button className=${p.mode === "hand" ? "on" : ""} onClick=${() => setMode(pi, "hand")}>Hand</button>
                <button className=${p.mode === "range" ? "on" : ""} onClick=${() => setMode(pi, "range")}>Range</button>
                <button className=${p.mode === "random" ? "on" : ""} onClick=${() => setMode(pi, "random")}>Rnd</button>
              </div>

              <div className="pcontent">
                ${p.mode === "hand" && html`<div className="cardrow">
                  ${p.cards.map((c, i) => html`<${Slot} key=${i} card=${c}
                    active=${isActive("p", i, pi)}
                    onClick=${() => { setActive({ zone: "p", p: pi, i }); setSelP(pi); }} />`)}
                  ${res && res.pct && res.pct[pi] != null &&
                    html`<span className="chip">pct ${res.pct[pi].toFixed(1)}</span>`}
                </div>`}
                ${p.mode === "range" && html`<div>
                  <div className="rangebar">
                    <input type="number" min="0" max="100" value=${p.lo}
                      onChange=${(e) => patchPlayer(pi, { lo: Math.max(0, Math.min(100, +e.target.value || 0)) })} />
                    <span style=${{ color: "var(--text-dim)" }}>–</span>
                    <input type="number" min="0" max="100" value=${p.hi}
                      onChange=${(e) => patchPlayer(pi, { hi: Math.max(0, Math.min(100, +e.target.value || 0)) })} />
                    <span style=${{ color: "var(--text-dim)", fontSize: 12 }}>pctile</span>
                  </div>
                  <${RangeSlider} lo=${p.lo} hi=${p.hi} color=${color}
                    onChange=${(lo, hi) => patchPlayer(pi, { lo, hi })} />
                  ${!dbl && html`<div className="chainrow">
                    <span className="tag">${(p.chain[0].lo > 0 || p.chain[0].hi < 100 || p.chain[1].lo > 0 || p.chain[1].hi < 100) ? "chained" : "filters"}</span>
                    flop
                    <input type="number" value=${p.chain[0].lo} onChange=${(e) =>
                      patchPlayer(pi, { chain: [{ ...p.chain[0], lo: +e.target.value || 0 }, p.chain[1]] })} />
                    –
                    <input type="number" value=${p.chain[0].hi} onChange=${(e) =>
                      patchPlayer(pi, { chain: [{ ...p.chain[0], hi: +e.target.value || 0 }, p.chain[1]] })} />
                    turn
                    <input type="number" value=${p.chain[1].lo} onChange=${(e) =>
                      patchPlayer(pi, { chain: [p.chain[0], { ...p.chain[1], lo: +e.target.value || 0 }] })} />
                    –
                    <input type="number" value=${p.chain[1].hi} onChange=${(e) =>
                      patchPlayer(pi, { chain: [p.chain[0], { ...p.chain[1], hi: +e.target.value || 0 }] })} />
                  </div>`}
                </div>`}
                ${p.mode === "random" && html`<div className="random-note">random hand every trial</div>`}
              </div>

              <button className="xbtn" title="Clear player" onClick=${() => clearPlayer(pi)}>×</button>

              <div className=${"result" + (stale ? " stale" : "")}>
                ${res && html`
                  <div className="eqline">
                    <div className="eqbar"><div className="fill" style=${{ width: res.equity[pi] + "%", color }} /></div>
                    <div className="eqpct" style=${{ color }}>${res.equity[pi].toFixed(2)}%</div>
                  </div>
                  <div className="subline">
                    <span>${res.double ? "scoop" : "win"} ${res.win[pi].toFixed(2)}%</span>
                    <span>tie ${res.tie[pi].toFixed(2)}%</span>
                    ${!res.exact && html`<span>± ${res.ci95[pi].toFixed(2)}%</span>`}
                  </div>
                  <${HTMini} dist=${res.handType[pi]} cats=${cats} onClick=${() => setHtModal(true)} />
                `}
              </div>
            </div>`;
          })}

          ${hint && html`<div className="hint">${hint}</div>`}
          ${error && html`<div className="errbox">${error}</div>`}
          ${stale && !hint && !error && !auto &&
            html`<div className="hint">Results out of date — press <span className="kbd">Enter</span> or Calculate</div>`}
        </div>

        <div className="panel">
          <h2>Deck</h2>
          <${Deck} usedSet=${usedSet} onPick=${assignCard} />
        </div>
      </div>

      <div>
        <div className="panel">
          <h2>Tools</h2>
          <div className="toolrow">
            <span className="lbl">Pct → hand</span>
            <input type="number" min="0" max="100" value=${pctQ}
              onChange=${(e) => setPctQ(Math.max(0, Math.min(100, +e.target.value || 0)))} />
            ${pctHand && html`
              <${HandText} str=${pctHand.hand} />
              <span className="chip">${pctHand.street}</span>
              <button className="btn small ghost" onClick=${() => dealTo(pctHand.hand, selP)}>Deal P${selP + 1}</button>`}
          </div>
          <div className="toolrow">
            <span className="lbl">Equity → hand</span>
            <input type="number" min="0" max="100" value=${eqQ}
              onChange=${(e) => setEqQ(Math.max(0, Math.min(100, +e.target.value || 0)))} />
            <button className="btn small" onClick=${findEq} disabled=${eqBusy}>
              ${eqBusy && html`<span className="spin" />`}Find
            </button>
            ${eqHand && !eqHand.error && html`
              <${HandText} str=${eqHand.hand} />
              <span className="chip">= ${eqHand.equity.toFixed(1)}%</span>
              <button className="btn small ghost" onClick=${() => dealTo(eqHand.hand, selP)}>Deal P${selP + 1}</button>`}
            ${eqHand && eqHand.error && html`<span style=${{ color: "var(--danger)", fontSize: 12 }}>${eqHand.error}</span>`}
          </div>
          <div className="toolrow">
            <label className="check">
              <input type="checkbox" checked=${buildRanks} onChange=${(e) => setBuildRanks(e.target.checked)} />
              Board percentiles <span style=${{ color: "var(--text-dim)" }}>(first calc per board is slower)</span>
            </label>
          </div>
          <div className="statusline">
            Preflop rank table: ${status.ranks ? "loaded" : "missing — run plo5calc --gen-ranks"} · ${status.ncpu} threads<br />
            Percentiles: 0 = weakest, 100 = strongest, on the current board.<br />
            Range chains ("flop / turn" filters) keep only that slice of the
            previous street's survivors.<br />
            Type <span className="kbd">A</span><span className="kbd">H</span> for A${SUITSYM.h} ·
            <span className="kbd">Enter</span> calculates · click a dealt card to remove it.
          </div>
        </div>
      </div>
    </div>

    ${htModal && result && html`<${HandTypesModal} result=${result}
      playerLabels=${players.map(playerSpec)} cats=${cats} onClose=${() => setHtModal(false)} />`}
    ${quiz && html`<${QuizModal} onClose=${() => setQuiz(false)} />`}
    ${toast && html`<div className="toast">${toast}</div>`}
  </div>`;
}

ReactDOM.createRoot(document.getElementById("root")).render(html`<${App} />`);
