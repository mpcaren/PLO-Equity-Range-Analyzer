/* Trainers: equity estimation and flop stack-off decisions. */

import { useCallback, useEffect, useRef, useState } from "react";
import { AnimatePresence, motion } from "motion/react";
import { call } from "../lib/engine.js";
import { cardsToStr, strToIds } from "../lib/cards.js";
import { DealtCard } from "../components/Cards.jsx";
import { AnimatedNumber, Button, Kbd, Segmented, softSpring, Spinner, spring } from "../components/ui.jsx";
import { IconArrowRight, IconRefresh } from "../components/Icons.jsx";
import { load, save } from "../lib/storage.js";

function shuffleDeal(n) {
  const deck = [...Array(52).keys()];
  for (let i = 0; i < n; i++) {
    const j = i + Math.floor(Math.random() * (52 - i));
    [deck[i], deck[j]] = [deck[j], deck[i]];
  }
  return deck.slice(0, n);
}

function CardLine({ ids, label, size = "lg", stagger = 0.05, base = 0 }) {
  return (
    <div className="t-line">
      <div className="t-lbl">{label}</div>
      <div className="t-cards">
        {ids.map((id, i) => <DealtCard key={id} id={id} size={size} delay={base + i * stagger} />)}
      </div>
    </div>
  );
}

function Stat({ label, value }) {
  return (
    <div className="stat">
      <span>{label}</span>
      <AnimatePresence mode="popLayout" initial={false}>
        <motion.b key={String(value)} initial={{ y: -10, opacity: 0 }} animate={{ y: 0, opacity: 1 }}
          exit={{ y: 10, opacity: 0 }} transition={spring}>{value}</motion.b>
      </AnimatePresence>
    </div>
  );
}

/* ------------------------------------------------------------------ */

function gradeOf(err) {
  if (err <= 1.5) return ["Perfect", "good"];
  if (err <= 3) return ["Excellent", "good"];
  if (err <= 6) return ["Good", "mid"];
  if (err <= 10) return ["Rough", "mid"];
  return ["Way off", "bad"];
}

function dealEquityQ(noppSel, dbl) {
  const d = shuffleDeal(11);
  return {
    hero: d.slice(0, 5).sort((a, b) => b - a),
    flop: d.slice(5, 8),
    flop2: d.slice(8, 11),
    nopp: noppSel || 1 + Math.floor(Math.random() * 5),
    dbl,
  };
}

export function EquityTrainer() {
  const [dbl, setDbl] = useState(false);
  const [noppSel, setNoppSel] = useState(0);
  const [q, setQ] = useState(() => dealEquityQ(0, false));
  const [qn, setQn] = useState(1);
  const [guess, setGuess] = useState("");
  const [answered, setAnswered] = useState(null); // guess value
  const [res, setRes] = useState(null);
  const [err, setErr] = useState(null);
  const [warn, setWarn] = useState(null);
  const [stats, setStats] = useState(() => load("quiz-eq", { n: 0, sum: 0, best: null }));
  const gen = useRef(0);
  const scored = useRef(false);
  const input = useRef(null);

  const deal = useCallback((sel, d) => {
    setQ(dealEquityQ(sel, d));
    setQn((n) => n + 1);
    setGuess("");
    setAnswered(null);
    setWarn(null);
    scored.current = false;
    setTimeout(() => input.current && input.current.focus(), 30);
  }, []);

  // the truth is computed in the background while you think
  useEffect(() => {
    const my = ++gen.current;
    setRes(null);
    setErr(null);
    call("/api/equity", {
      players: [cardsToStr(q.hero), ...Array(q.nopp).fill("random")].join(","),
      board: cardsToStr(q.flop),
      board2: q.dbl ? cardsToStr(q.flop2) : "",
      double: q.dbl,
      trials: 500000,
      maxenum: 0,
      buildranks: true,
    }).then((j) => { if (my === gen.current) setRes(j); },
      (e) => { if (my === gen.current && !e.cancelled) setErr(e.message); });
  }, [q]);

  // score once both the guess and the answer are in
  useEffect(() => {
    if (answered == null || !res || scored.current) return;
    scored.current = true;
    const e = Math.abs(answered - res.equity[0]);
    setStats((s) => {
      const n = { n: s.n + 1, sum: s.sum + e, best: s.best == null ? e : Math.min(s.best, e) };
      save("quiz-eq", n);
      return n;
    });
  }, [answered, res]);

  const submit = () => {
    const v = parseFloat(guess);
    if (guess.trim() === "" || Number.isNaN(v) || v < 0 || v > 100) {
      setWarn("Enter your estimate between 0 and 100");
      return;
    }
    setWarn(null);
    setAnswered(v);
  };
  const onEnter = () => (answered == null ? submit() : deal(noppSel, dbl));
  const enterRef = useRef(onEnter);
  enterRef.current = onEnter;
  useEffect(() => {
    const k = (e) => { if (e.key === "Enter") { e.preventDefault(); enterRef.current(); } };
    window.addEventListener("keydown", k);
    input.current && input.current.focus();
    return () => window.removeEventListener("keydown", k);
  }, []);

  const eq = res ? res.equity[0] : null;
  const e = res && answered != null ? Math.abs(answered - eq) : null;
  const [gtxt, gcls] = e != null ? gradeOf(e) : ["", ""];

  return (
    <div className="trainer">
      <div className="t-bar">
        <Segmented size="sm" value={dbl} layoutKey="eq-dbl" onChange={(v) => { setDbl(v); deal(noppSel, v); }}
          options={[{ value: false, label: "Single board" }, { value: true, label: "Double board" }]} />
        <span className="dim small">Opponents</span>
        <Segmented size="sm" value={noppSel} layoutKey="eq-opp" onChange={(v) => { setNoppSel(v); deal(v, dbl); }}
          options={[0, 1, 2, 3, 4, 5].map((i) => ({ value: i, label: i === 0 ? "Mixed" : String(i) }))} />
        <div className="t-stats">
          <Stat label="Answered" value={stats.n} />
          <Stat label="Avg error" value={stats.n ? `${(stats.sum / stats.n).toFixed(1)}pp` : "—"} />
          <Stat label="Best" value={stats.best != null ? `${stats.best.toFixed(1)}pp` : "—"} />
          <button type="button" className="icon-btn" title="Reset stats"
            onClick={() => { const z = { n: 0, sum: 0, best: null }; setStats(z); save("quiz-eq", z); }}>
            <IconRefresh size={15} />
          </button>
        </div>
      </div>

      <AnimatePresence mode="wait">
        <motion.div key={qn} className="t-stage"
          initial={{ opacity: 0, x: 40 }} animate={{ opacity: 1, x: 0 }} exit={{ opacity: 0, x: -40 }}
          transition={softSpring}>
          <div className="t-title">
            <span className="t-qn">Question {qn}</span>
            <h2>What's your equity?</h2>
            <p>
              Your hand vs <b>{q.nopp} random opponent{q.nopp === 1 ? "" : "s"}</b> on this flop
              {q.dbl ? " — two boards, each pays half the pot" : ""}.
            </p>
          </div>
          <div className="t-felt">
            <CardLine ids={q.flop} label={q.dbl ? "Flop A" : "Flop"} base={0.05} />
            {q.dbl && <CardLine ids={q.flop2} label="Flop B" base={0.2} />}
            <CardLine ids={q.hero} label="Your hand" base={q.dbl ? 0.35 : 0.22} />
          </div>

          <div className="t-answer">
            <div className="guess">
              <input ref={input} type="number" min="0" max="100" step="0.1" inputMode="decimal"
                placeholder="—" value={guess} disabled={answered != null}
                onChange={(ev) => setGuess(ev.target.value)} aria-label="Your equity estimate in percent" />
              <span>%</span>
            </div>
            <Button variant={answered == null ? "primary" : "soft"} onClick={onEnter} icon={answered == null ? undefined : IconArrowRight}>
              {answered == null ? "Lock in" : "Next hand"} <small><Kbd>Enter</Kbd></small>
            </Button>
          </div>
          <AnimatePresence>{warn && <motion.p className="t-warn" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>{warn}</motion.p>}</AnimatePresence>

          <AnimatePresence>
            {answered != null && (
              <motion.div className="reveal" initial={{ opacity: 0, y: 16 }} animate={{ opacity: 1, y: 0 }}
                exit={{ opacity: 0 }} transition={softSpring}>
                {!res ? (
                  <div className="reveal-wait">{err ? err : <><Spinner /> working out the answer…</>}</div>
                ) : (
                  <>
                    <div className="reveal-bar">
                      <motion.div className="reveal-fill" initial={{ width: 0 }} animate={{ width: `${eq}%` }}
                        transition={{ ...softSpring, delay: 0.05 }} />
                      <motion.div className={`reveal-mark ${gcls}`} initial={{ left: "0%", opacity: 0 }}
                        animate={{ left: `${Math.min(100, answered)}%`, opacity: 1 }} transition={{ ...spring, delay: 0.25 }}>
                        <span>you · {answered.toFixed(1)}</span>
                      </motion.div>
                    </div>
                    <div className="reveal-main">
                      <div className="reveal-eq"><span><AnimatedNumber value={eq} decimals={1} />%</span><small>true equity</small></div>
                      <motion.div className={`grade grade-${gcls}`} initial={{ scale: 0.5, opacity: 0 }}
                        animate={{ scale: 1, opacity: 1 }} transition={{ ...spring, delay: 0.35 }}>
                        {gtxt}<small>{e.toFixed(1)}pp off</small>
                      </motion.div>
                    </div>
                    <p className="reveal-sub">
                      {q.dbl ? "scoop" : "win"} {res.win[0].toFixed(1)}% · tie {res.tie[0].toFixed(1)}%
                      {res.pct && res.pct[0] != null && ` · your hand is the ${ordinal(Math.round(res.pct[0]))} percentile ${q.dbl ? "across both flops" : "on this flop"}`}
                    </p>
                  </>
                )}
              </motion.div>
            )}
          </AnimatePresence>
        </motion.div>
      </AnimatePresence>
    </div>
  );
}

const ordinal = (n) => {
  const s = ["th", "st", "nd", "rd"], v = n % 100;
  return n + (s[(v - 20) % 10] || s[v] || s[0]);
};

/* ------------------------------------------------------------------ */

export function StackOffTrainer() {
  const [sprSel, setSprSel] = useState(0);
  const [noppSel, setNoppSel] = useState(0);
  const [margin, setMargin] = useState(5);
  const [qn, setQn] = useState(0);
  const [q, setQ] = useState(null);
  const [choice, setChoice] = useState(null);
  const [err, setErr] = useState(null);
  const [stats, setStats] = useState(() => load("quiz-spr", { n: 0, right: 0, streak: 0, best: 0 }));
  const gen = useRef(0);
  const marginRef = useRef(margin);
  marginRef.current = margin;

  const deal = useCallback(async (sSel, oSel) => {
    const my = ++gen.current;
    setChoice(null); setErr(null); setQ(null);
    setQn((n) => n + 1);
    const spr = sSel || Math.round((3 + Math.random() * 7) * 2) / 2;
    const nopp = oSel || 1 + Math.floor(Math.random() * 5);
    const offset = (Math.random() < 0.5 ? 1 : -1) * marginRef.current;
    try {
      const rf = await call("/api/random", { street: 3, count: 1 });
      const j = await call("/api/sprquiz", { board: rf.board, spr, opp: nopp, offset, pot: 10 });
      if (my === gen.current) setQ(j);
    } catch (e) {
      if (my === gen.current && !e.cancelled) setErr(e.message);
    }
  }, []);

  useEffect(() => { deal(0, 0); }, [deal]);

  const good = q ? q.equity >= q.eqNeeded : false;
  const answer = (shove) => {
    if (!q || choice != null) return;
    setChoice(shove);
    setStats((s) => {
      const ok = shove === good;
      const streak = ok ? s.streak + 1 : 0;
      const n = { n: s.n + 1, right: s.right + (ok ? 1 : 0), streak, best: Math.max(s.best, streak) };
      save("quiz-spr", n);
      return n;
    });
  };

  const kb = useRef(null);
  kb.current = (e) => {
    if (e.target && e.target.tagName === "INPUT") return;
    const k = e.key.toLowerCase();
    if (q && choice == null && k === "s") answer(true);
    else if (q && choice == null && k === "f") answer(false);
    else if (choice != null && (e.key === "Enter" || k === " ")) { e.preventDefault(); deal(sprSel, noppSel); }
  };
  useEffect(() => {
    const h = (e) => kb.current(e);
    window.addEventListener("keydown", h);
    return () => window.removeEventListener("keydown", h);
  }, []);

  const correct = choice != null && choice === good;

  return (
    <div className="trainer">
      <div className="t-bar">
        <span className="dim small">SPR</span>
        <Segmented size="sm" value={sprSel} layoutKey="spr-sel" onChange={(v) => { setSprSel(v); deal(v, noppSel); }}
          options={[0, 3, 5, 7, 10].map((v) => ({ value: v, label: v === 0 ? "Mixed" : String(v) }))} />
        <span className="dim small">Opponents</span>
        <Segmented size="sm" value={noppSel} layoutKey="spr-opp" onChange={(v) => { setNoppSel(v); deal(sprSel, v); }}
          options={[0, 1, 2, 3, 4, 5].map((i) => ({ value: i, label: i === 0 ? "Mixed" : String(i) }))} />
        <label className="margin" title="How far from the stack-off point hands are dealt — smaller is harder (applies from the next hand)">
          Difficulty
          <input type="range" min="1" max="15" step="0.5" value={16 - margin}
            onChange={(e) => setMargin(16 - Number(e.target.value))} />
          <b>±{margin}pp</b>
        </label>
        <div className="t-stats">
          <Stat label="Correct" value={stats.n ? `${Math.round((100 * stats.right) / stats.n)}%` : "—"} />
          <Stat label="Streak" value={stats.streak} />
          <Stat label="Best" value={stats.best} />
          <button type="button" className="icon-btn" title="Reset stats"
            onClick={() => { const z = { n: 0, right: 0, streak: 0, best: 0 }; setStats(z); save("quiz-spr", z); }}>
            <IconRefresh size={15} />
          </button>
        </div>
      </div>

      <AnimatePresence mode="wait">
        <motion.div key={qn} className="t-stage"
          initial={{ opacity: 0, x: 40 }} animate={{ opacity: 1, x: 0 }} exit={{ opacity: 0, x: -40 }}
          transition={softSpring}>
          <div className="t-title">
            <span className="t-qn">Question {qn}</span>
            <h2>Stack off or fold?</h2>
            {q ? (
              <p>
                SPR <b>{q.spr}</b> · pot {q.pot.toFixed(0)} · stacks {q.stack.toFixed(0)} — {q.opp === 1 ? "1 villain gets" : `${q.opp} villains get`} it in
                with the top <b>{q.mdf.toFixed(0)}%</b> of hands{q.bandWidened ? "*" : ""}.
              </p>
            ) : <p>&nbsp;</p>}
          </div>

          {err ? <div className="notice notice-error"><div className="notice-in">{err}</div></div> : !q ? (
            <div className="t-loading"><Spinner size={18} /> Dealing a hand near the stack-off point…</div>
          ) : (
            <>
              <div className="t-felt">
                <CardLine ids={strToIds(q.board)} label="Flop" base={0.05} />
                <CardLine ids={strToIds(q.hand)} label="Your hand" base={0.22} />
              </div>
              <AnimatePresence mode="wait" initial={false}>
                {choice == null ? (
                  <motion.div key="ask" className="t-choices" initial={{ opacity: 0, y: 10 }} animate={{ opacity: 1, y: 0 }}
                    exit={{ opacity: 0, y: -10 }} transition={softSpring}>
                    <Button variant="primary" onClick={() => answer(true)}>Stack off <small><Kbd>S</Kbd></small></Button>
                    <Button variant="soft" onClick={() => answer(false)}>Fold <small><Kbd>F</Kbd></small></Button>
                  </motion.div>
                ) : (
                  <motion.div key="reveal" className="reveal" initial={{ opacity: 0, y: 16 }} animate={{ opacity: 1, y: 0 }}
                    exit={{ opacity: 0 }} transition={softSpring}>
                    <div className="reveal-main">
                      <motion.div className={`grade grade-${correct ? "good" : "bad"}`} initial={{ scale: 0.5, opacity: 0 }}
                        animate={{ scale: 1, opacity: 1 }} transition={spring}>
                        {correct ? "Correct" : "Wrong"}<small>{good ? "stack off" : "fold"} was right</small>
                      </motion.div>
                      <div className="reveal-eq"><span><AnimatedNumber value={q.equity} decimals={1} />%</span><small>your equity</small></div>
                    </div>
                    <div className="reveal-bar">
                      <motion.div className="reveal-fill" initial={{ width: 0 }} animate={{ width: `${Math.min(100, q.equity)}%` }}
                        transition={softSpring} />
                      <motion.div className="reveal-mark need" initial={{ opacity: 0, y: -6 }} animate={{ opacity: 1, y: 0 }}
                        style={{ left: `${Math.min(100, q.eqNeeded)}%` }} transition={{ ...spring, delay: 0.2 }}>
                        <span>need {q.eqNeeded.toFixed(1)}</span>
                      </motion.div>
                    </div>
                    <p className="reveal-sub">
                      {good ? "+" : "−"}{Math.abs(q.equity - q.eqNeeded).toFixed(1)}pp {good ? "above" : "below"} the stack-off point ·
                      villain range {q.bandLo.toFixed(0)}–{q.bandHi.toFixed(0)} pct{q.bandWidened ? " (widened to stay dealable)" : ""}
                      {!good && q.breakevenFold != null && ` · a shove still breaks even with ${q.breakevenFold.toFixed(0)}% folds`}
                    </p>
                    <Button variant="soft" icon={IconArrowRight} onClick={() => deal(sprSel, noppSel)}>
                      Next hand <small><Kbd>Enter</Kbd></small>
                    </Button>
                  </motion.div>
                )}
              </AnimatePresence>
            </>
          )}
        </motion.div>
      </AnimatePresence>
    </div>
  );
}
