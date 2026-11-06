/* App-wide state: settings, engine status, the calculator (kept here so it
 * survives switching views), the rank-table build job, and toasts. */

import { createContext, useCallback, useContext, useEffect, useMemo, useReducer, useRef, useState } from "react";
import { call, cancelAll, engineInfo, onEngineDown } from "./lib/engine.js";
import { load, save } from "./lib/storage.js";
import { calcReducer, equityParams, reviveCalc, usedCards, validate } from "./lib/calcState.js";
import { DEFAULT_CATEGORIES } from "./lib/cards.js";

const Ctx = createContext(null);
export const useApp = () => useContext(Ctx);

const FALLBACK_PRESETS = [
  { name: "Fast", trials: 200000, maxenum: 40000 },
  { name: "Balanced", trials: 1000000, maxenum: 200000 },
  { name: "Precise", trials: 10000000, maxenum: 2000000 },
];

const DEFAULT_SETTINGS = {
  precision: 1,
  auto: true,
  buildRanks: true,
  fourColor: true,
  motion: "full", // "full" | "reduced"
};

export function AppProvider({ children }) {
  /* ---- settings ---- */
  const [settings, setSettingsRaw] = useState(() => ({ ...DEFAULT_SETTINGS, ...load("settings", {}) }));
  const setSettings = useCallback((patch) => setSettingsRaw((s) => {
    const n = { ...s, ...patch };
    save("settings", n);
    return n;
  }), []);

  /* ---- toasts ---- */
  const [toasts, setToasts] = useState([]);
  const toast = useCallback((text, kind = "info") => {
    const id = Math.random().toString(36).slice(2);
    setToasts((t) => [...t.slice(-2), { id, text, kind }]);
    setTimeout(() => setToasts((t) => t.filter((x) => x.id !== id)), 2600);
  }, []);

  /* ---- engine status ---- */
  const [info, setInfo] = useState(null);       // from main: paths, found, error
  const [status, setStatus] = useState(null);   // /api/status
  const [engineDown, setEngineDown] = useState(null);

  const refreshStatus = useCallback(async () => {
    try {
      const i = await engineInfo();
      setInfo(i);
      if (i && i.found && !i.error) {
        setStatus(await call("/api/status"));
        setEngineDown(null);
      }
    } catch (e) {
      setEngineDown(String(e.message || e));
    }
  }, []);

  useEffect(() => {
    refreshStatus();
    return onEngineDown((msg) => {
      setEngineDown(msg);
      setTimeout(refreshStatus, 1200);
    });
  }, [refreshStatus]);

  const presets = status && status.presets && status.presets.length === 3 ? status.presets : FALLBACK_PRESETS;
  const categories = status && status.categories && status.categories.length ? status.categories : DEFAULT_CATEGORIES;
  const ranksLoaded = !!(status && status.ranks);

  /* ---- calculator ---- */
  const [calc, dispatch] = useReducer(calcReducer, undefined, () => reviveCalc(load("calc", null)));
  useEffect(() => {
    const t = setTimeout(() => save("calc", {
      players: calc.players.map(({ mode, cards, lo, hi, chain }) => ({ mode, cards, lo, hi, chain })),
      board: calc.board, board2: calc.board2, dead: calc.dead, dbl: calc.dbl,
    }), 300);
    return () => clearTimeout(t);
  }, [calc.players, calc.board, calc.board2, calc.dead, calc.dbl]);

  const used = useMemo(() => usedCards(calc), [calc]);
  const hint = useMemo(() => validate(calc, ranksLoaded), [calc, ranksLoaded]);
  const preset = presets[Math.max(0, Math.min(2, settings.precision))];
  const params = useMemo(
    () => (hint ? null : equityParams(calc, preset, settings.buildRanks)),
    // only the parts of calc that change the request
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [hint, calc.players, calc.board, calc.board2, calc.dead, calc.dbl, preset, settings.buildRanks],
  );
  const key = params ? JSON.stringify(params) : null;

  const [run, setRun] = useState({ result: null, key: null, calculating: false, error: null, progress: null, players: null });
  const reqId = useRef(0);

  const calculate = useCallback(async () => {
    if (!params) return;
    const my = ++reqId.current;
    const snapshot = calc.players.map((p) => ({ id: p.id, mode: p.mode }));
    setRun((r) => ({ ...r, calculating: true, error: null, progress: null }));
    try {
      const body = await call("/api/equity", params, (p) => {
        if (my === reqId.current) setRun((r) => ({ ...r, progress: p }));
      });
      if (my !== reqId.current) return;
      setRun({ result: body, key, calculating: false, error: null, progress: null, players: snapshot });
    } catch (e) {
      if (my !== reqId.current) return;
      setRun((r) => ({ ...r, calculating: false, progress: null, error: e.cancelled ? null : e.message }));
    }
  }, [params, key, calc.players]);

  const cancel = useCallback(async () => {
    reqId.current++;
    setRun((r) => ({ ...r, calculating: false, progress: null }));
    await cancelAll();
  }, []);

  // auto mode: recalculate shortly after the setup settles
  const calcRef = useRef(calculate);
  calcRef.current = calculate;
  useEffect(() => {
    if (!settings.auto || !key || key === run.key) return;
    const t = setTimeout(() => calcRef.current(), 380);
    return () => clearTimeout(t);
  }, [settings.auto, key, run.key]);

  const stale = run.result != null && key !== run.key;

  /* ---- rank table build ---- */
  const [rankJob, setRankJob] = useState(null); // {done,total,startedAt,error}
  const buildRanks = useCallback(async () => {
    const startedAt = performance.now();
    setRankJob({ done: 0, total: 1, startedAt, error: null });
    try {
      const r = await call("/svc/genranks", {}, (p) =>
        setRankJob((j) => (j ? { ...j, done: p.done, total: p.total } : j)));
      setRankJob(null);
      toast(r.ranks ? `Rank table ready (${Math.round(r.seconds)} s)` : "Rank table built but could not be loaded", r.ranks ? "ok" : "error");
      refreshStatus();
    } catch (e) {
      setRankJob(null);
      if (!e.cancelled) toast(e.message, "error");
    }
  }, [toast, refreshStatus]);
  const cancelRanks = useCallback(async () => {
    setRankJob(null);
    await cancelAll();
    toast("Rank table build cancelled");
  }, [toast]);

  const value = {
    settings, setSettings,
    toasts, toast,
    info, status, presets, categories, ranksLoaded, engineDown, refreshStatus,
    calc, dispatch, used, hint, key,
    run, calculate, cancel, stale,
    rankJob, buildRanks, cancelRanks,
  };
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}
