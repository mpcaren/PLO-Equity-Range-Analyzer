/* App shell: title bar, sidebar navigation, animated view switching. */

import { useEffect, useState } from "react";
import { AnimatePresence, MotionConfig, motion } from "motion/react";
import { useApp } from "./state.jsx";
import { platform } from "./lib/engine.js";
import { load, save } from "./lib/storage.js";
import Calculator from "./views/Calculator.jsx";
import { EquityTrainer, StackOffTrainer } from "./views/Trainer.jsx";
import Settings from "./views/Settings.jsx";
import { Kbd, ProgressRing, softSpring, spring, Toasts } from "./components/ui.jsx";
import { IconEquity, IconSettings, IconStack, IconTarget } from "./components/Icons.jsx";
import logo from "./logo.svg";

const VIEWS = [
  { id: "calc", label: "Equity", icon: IconEquity, title: "Equity calculator", sub: "Hands, ranges and boards — exact or Monte Carlo" },
  { id: "quiz", label: "Trainer", icon: IconTarget, title: "Equity trainer", sub: "Estimate your flop equity against random hands" },
  { id: "spr", label: "Stack-off", icon: IconStack, title: "Stack-off trainer", sub: "Shove or fold at a given SPR against MDF ranges" },
  { id: "settings", label: "Settings", icon: IconSettings, title: "Settings", sub: "Rank table, display and engine" },
];

export default function App() {
  const { settings, toasts, info, status, rankJob, engineDown } = useApp();
  const [view, setView] = useState(() => {
    const v = load("view", "calc");
    return VIEWS.some((x) => x.id === v) ? v : "calc";
  });
  useEffect(() => save("view", view), [view]);

  // Ctrl/Cmd+1..4 switches views
  useEffect(() => {
    const k = (e) => {
      if (!(e.ctrlKey || e.metaKey) || e.altKey) return;
      const n = Number(e.key);
      if (n >= 1 && n <= VIEWS.length) { e.preventDefault(); setView(VIEWS[n - 1].id); }
    };
    window.addEventListener("keydown", k);
    return () => window.removeEventListener("keydown", k);
  }, []);

  useEffect(() => {
    document.documentElement.dataset.deck = settings.fourColor ? "four" : "two";
    document.documentElement.dataset.platform = platform;
  }, [settings.fourColor]);

  const cur = VIEWS.find((v) => v.id === view);
  const missing = info && !info.found;

  return (
    <MotionConfig reducedMotion={settings.motion === "reduced" ? "always" : "user"} transition={softSpring}>
      <div className="shell">
        <header className="titlebar">
          <div className="brand">
            <img src={logo} alt="" />
            <span>PLO5 Equity Lab</span>
          </div>
          <div className="titlebar-fill" />
          <EnginePill status={status} rankJob={rankJob} down={engineDown} missing={missing} onClick={() => setView("settings")} />
        </header>

        <nav className="sidebar" aria-label="Views">
          {VIEWS.map((v, i) => {
            const on = v.id === view;
            const Icon = v.icon;
            return (
              <button key={v.id} type="button" className={`nav-item ${on ? "on" : ""}`} onClick={() => setView(v.id)}
                title={`${v.title} (Ctrl+${i + 1})`} aria-current={on ? "page" : undefined}>
                {on && <motion.span layoutId="nav-pill" className="nav-pill" transition={spring} />}
                <motion.span className="nav-icon" animate={{ scale: on ? 1.06 : 1 }} transition={spring}><Icon size={20} /></motion.span>
                <span className="nav-label">{v.label}</span>
              </button>
            );
          })}
        </nav>

        <motion.main className="content" layoutScroll>
          {missing ? <EngineMissing info={info} /> : (
            <AnimatePresence mode="wait" initial={false}>
              <motion.div
                key={view}
                className="view"
                initial={{ opacity: 0, y: 14 }}
                animate={{ opacity: 1, y: 0 }}
                exit={{ opacity: 0, y: -8, transition: { duration: 0.12 } }}
                transition={softSpring}
              >
                <div className="view-head">
                  <h1>{cur.title}</h1>
                  <p>{cur.sub}</p>
                </div>
                {view === "calc" && <Calculator onOpenSettings={() => setView("settings")} />}
                {view === "quiz" && <EquityTrainer />}
                {view === "spr" && <StackOffTrainer />}
                {view === "settings" && <Settings />}
              </motion.div>
            </AnimatePresence>
          )}
        </motion.main>
        <Toasts items={toasts} />
      </div>
    </MotionConfig>
  );
}

function EnginePill({ status, rankJob, down, missing, onClick }) {
  let state, text;
  if (missing) { state = "bad"; text = "Engine not found"; }
  else if (down) { state = "bad"; text = "Engine restarting…"; }
  else if (rankJob) { state = "busy"; text = `Rank table ${Math.floor((100 * rankJob.done) / Math.max(1, rankJob.total))}%`; }
  else if (!status) { state = "busy"; text = "Starting engine…"; }
  else { state = "ok"; text = `${status.ncpu} threads${status.ranks ? "" : " · no preflop table"}`; }
  return (
    <motion.button type="button" layout className={`engine-pill ${state}`} onClick={onClick} transition={spring}
      title="Engine status — open settings">
      {rankJob ? <ProgressRing value={rankJob.done / Math.max(1, rankJob.total)} size={14} stroke={2.5} /> : <span className="dot" />}
      <AnimatePresence mode="popLayout" initial={false}>
        <motion.span key={text} initial={{ opacity: 0, y: 6 }} animate={{ opacity: 1, y: 0 }} exit={{ opacity: 0, y: -6 }}>
          {text}
        </motion.span>
      </AnimatePresence>
    </motion.button>
  );
}

function EngineMissing({ info }) {
  const win = platform === "win32";
  return (
    <motion.div className="missing" initial={{ opacity: 0, y: 20 }} animate={{ opacity: 1, y: 0 }}>
      <h1>The engine isn't built yet</h1>
      <p>
        PLO5 Equity Lab runs the C engine as <code>{win ? "plo5svc.exe" : "plo5svc"}</code>. Build it from the
        repository root, then restart the app:
      </p>
      <pre>{win ? "build.bat          (MSVC)\nmake plo5svc       (MinGW / gcc)" : "make plo5svc"}</pre>
      <p className="dim">Looked for: <code>{info.exe}</code></p>
    </motion.div>
  );
}
