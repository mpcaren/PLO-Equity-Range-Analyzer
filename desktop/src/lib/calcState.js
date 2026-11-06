/* Calculator state: players, boards, dead cards and the "active slot"
 * the next picked card goes into. Pure reducer — no engine calls. */

import { cardsToStr, SUIT_SYM } from "./cards.js";

export const DEAD_SLOTS = 6;
export const MAX_PLAYERS = 6;

let uid = 1;
const emptyHand = () => [null, null, null, null, null];

export const newPlayer = (mode = "hand") => ({
  id: uid++,
  mode,
  cards: emptyHand(),
  lo: 0,
  hi: 100,
  chain: [{ lo: 0, hi: 100 }, { lo: 0, hi: 100 }],
  filtersOpen: false,
});

export const initialCalc = () => ({
  players: [newPlayer(), newPlayer()],
  board: emptyHand(),
  board2: emptyHand(),
  dead: Array(DEAD_SLOTS).fill(null),
  dbl: false,
  active: { zone: "p", p: 0, i: 0 },
  selP: 0,
});

/* restore a saved setup defensively (shape may come from an older build) */
export function reviveCalc(saved) {
  const base = initialCalc();
  if (!saved || typeof saved !== "object") return base;
  const okCard = (c) => (Number.isInteger(c) && c >= 0 && c < 52 ? c : null);
  const arr = (a, n) => Array.from({ length: n }, (_, i) => okCard(a && a[i]));
  const band = (b, d) => ({
    lo: Math.max(0, Math.min(100, Number(b && b.lo) || 0)),
    hi: Math.max(0, Math.min(100, b && b.hi != null ? Number(b.hi) : d)),
  });
  const players = Array.isArray(saved.players)
    ? saved.players.slice(0, MAX_PLAYERS).map((p) => {
        const np = newPlayer(["hand", "range", "random"].includes(p.mode) ? p.mode : "hand");
        np.cards = arr(p.cards, 5);
        const b = band(p, 100);
        np.lo = b.lo; np.hi = b.hi;
        np.chain = [band(p.chain && p.chain[0], 100), band(p.chain && p.chain[1], 100)];
        return np;
      })
    : base.players;
  while (players.length < 2) players.push(newPlayer());
  const s = {
    ...base,
    players,
    board: arr(saved.board, 5),
    board2: arr(saved.board2, 5),
    dead: arr(saved.dead, DEAD_SLOTS),
    dbl: !!saved.dbl,
  };
  return dedupe(s);
}

/* drop any card that appears twice (keeps the first occurrence) */
function dedupe(s) {
  const seen = new Set();
  const keep = (c) => {
    if (c == null || seen.has(c)) return null;
    seen.add(c);
    return c;
  };
  return {
    ...s,
    players: s.players.map((p) => ({ ...p, cards: p.mode === "hand" ? p.cards.map(keep) : emptyHand() })),
    board: s.board.map(keep),
    board2: s.dbl ? s.board2.map(keep) : emptyHand(),
    dead: s.dead.map(keep),
  };
}

export function usedCards(s) {
  const u = new Set();
  for (const p of s.players) if (p.mode === "hand") for (const c of p.cards) if (c != null) u.add(c);
  for (const c of s.board) if (c != null) u.add(c);
  if (s.dbl) for (const c of s.board2) if (c != null) u.add(c);
  for (const c of s.dead) if (c != null) u.add(c);
  return u;
}

/* deal order: hand players' slots, then board A, then board B */
function slotOrder(s) {
  const order = [];
  s.players.forEach((p, pi) => {
    if (p.mode === "hand")
      for (let i = 0; i < 5; i++) order.push({ zone: "p", p: pi, i, filled: p.cards[i] != null });
  });
  for (let i = 0; i < 5; i++) order.push({ zone: "b", i, filled: s.board[i] != null });
  if (s.dbl) for (let i = 0; i < 5; i++) order.push({ zone: "b2", i, filled: s.board2[i] != null });
  return order;
}

const sameSlot = (a, b) =>
  a && b && a.zone === b.zone && a.i === b.i && (a.zone !== "p" || a.p === b.p);

export function nextActive(s, from) {
  if (from && from.zone === "d") {
    for (let i = from.i + 1; i < DEAD_SLOTS; i++) if (s.dead[i] == null) return { zone: "d", i };
    return null;
  }
  const order = slotOrder(s);
  const idx = from ? order.findIndex((o) => sameSlot(o, from)) : -1;
  for (let k = idx + 1; k < order.length; k++)
    if (!order[k].filled) return { zone: order[k].zone, p: order[k].p, i: order[k].i };
  // wrap around to the first empty slot, so typing keeps working
  for (let k = 0; k <= idx && k < order.length; k++)
    if (!order[k].filled) return { zone: order[k].zone, p: order[k].p, i: order[k].i };
  return null;
}

function getSlot(s, a) {
  if (!a) return undefined;
  if (a.zone === "p") return s.players[a.p] && s.players[a.p].mode === "hand" ? s.players[a.p].cards[a.i] : undefined;
  if (a.zone === "b") return s.board[a.i];
  if (a.zone === "b2") return s.dbl ? s.board2[a.i] : undefined;
  if (a.zone === "d") return s.dead[a.i];
  return undefined;
}

function setSlot(s, a, card) {
  const put = (arr) => arr.map((c, j) => (j === a.i ? card : c));
  if (a.zone === "p")
    return { ...s, players: s.players.map((p, pi) => (pi === a.p ? { ...p, cards: put(p.cards) } : p)) };
  if (a.zone === "b") return { ...s, board: put(s.board) };
  if (a.zone === "b2") return { ...s, board2: put(s.board2) };
  if (a.zone === "d") return { ...s, dead: put(s.dead) };
  return s;
}

function removeCard(s, id) {
  const rm = (arr) => arr.map((c) => (c === id ? null : c));
  return {
    ...s,
    players: s.players.map((p) => (p.cards.includes(id) ? { ...p, cards: rm(p.cards) } : p)),
    board: rm(s.board),
    board2: rm(s.board2),
    dead: rm(s.dead),
  };
}

const fill5 = (ids) => Array.from({ length: 5 }, (_, i) => (ids[i] != null ? ids[i] : null));

export function calcReducer(s, a) {
  switch (a.type) {
    case "pick": {
      // deck click / typed card: a used card is removed, else dealt to the active slot
      if (usedCards(s).has(a.card)) return removeCard(s, a.card);
      if (!a.target && getSlot(s, s.active) === undefined) return s;
      const target = a.target || s.active;
      let n = setSlot(s, target, a.card);
      if (target.zone === "p") n = { ...n, selP: target.p };
      return { ...n, active: nextActive(n, target) };
    }
    case "remove":
      return removeCard(s, a.card);
    case "focus":
      return { ...s, active: a.slot, selP: a.slot && a.slot.zone === "p" ? a.slot.p : s.selP };
    case "clearSlot": {
      const slot = a.slot || s.active;
      if (getSlot(s, slot) == null) return s;
      return { ...setSlot(s, slot, null), active: slot };
    }
    case "paste": {
      // fill consecutive slots from the active one
      let n = s;
      let at = s.active;
      const used = usedCards(s);
      for (const id of a.cards) {
        if (!at) break;
        if (used.has(id)) continue;
        n = setSlot(n, at, id);
        used.add(id);
        at = nextActive(n, at);
      }
      return { ...n, active: at };
    }
    case "addPlayer": {
      if (s.players.length >= MAX_PLAYERS) return s;
      const players = [...s.players, newPlayer(a.mode || "hand")];
      const n = { ...s, players };
      return { ...n, active: a.mode && a.mode !== "hand" ? s.active : { zone: "p", p: players.length - 1, i: 0 } };
    }
    case "removePlayer": {
      if (s.players.length <= 2) return s;
      const players = s.players.filter((_, i) => i !== a.p);
      const n = { ...s, players, selP: Math.min(s.selP, players.length - 1) };
      const act = s.active && s.active.zone === "p"
        ? (s.active.p === a.p ? null : s.active.p > a.p ? { ...s.active, p: s.active.p - 1 } : s.active)
        : s.active;
      return { ...n, active: act || nextActive(n, null) };
    }
    case "setMode": {
      const players = s.players.map((p, i) =>
        i === a.p ? { ...p, mode: a.mode, cards: a.mode === "hand" ? p.cards : emptyHand() } : p);
      const n = { ...s, players, selP: a.p };
      if (a.mode === "hand") {
        const first = players[a.p].cards.findIndex((c) => c == null);
        return { ...n, active: first >= 0 ? { zone: "p", p: a.p, i: first } : n.active };
      }
      const act = s.active && s.active.zone === "p" && s.active.p === a.p ? nextActive(n, null) : s.active;
      return { ...n, active: act };
    }
    case "patchPlayer":
      return { ...s, players: s.players.map((p, i) => (i === a.p ? { ...p, ...a.patch } : p)) };
    case "clearPlayer":
      return {
        ...s,
        players: s.players.map((p, i) => (i === a.p ? { ...p, cards: emptyHand() } : p)),
        active: s.players[a.p].mode === "hand" ? { zone: "p", p: a.p, i: 0 } : s.active,
      };
    case "clearBoards":
      return { ...s, board: emptyHand(), board2: emptyHand(), active: { zone: "b", i: 0 } };
    case "clearAll": {
      const n = {
        ...s,
        players: s.players.map((p) => ({ ...p, cards: emptyHand() })),
        board: emptyHand(),
        board2: emptyHand(),
        dead: Array(DEAD_SLOTS).fill(null),
      };
      return { ...n, active: nextActive(n, null) };
    }
    case "setDouble": {
      const n = { ...s, dbl: a.value, board2: a.value ? s.board2 : emptyHand() };
      const act = !a.value && s.active && s.active.zone === "b2" ? { zone: "b", i: 0 } : s.active;
      return { ...n, active: act };
    }
    case "setBoards": {
      let n = s;
      for (const id of [...(a.board || []), ...(a.board2 || [])]) n = removeCard(n, id);
      n = { ...n, board: fill5(a.board || []), board2: s.dbl ? fill5(a.board2 || []) : n.board2 };
      return { ...n, active: nextActive(n, null) };
    }
    case "dealHand": {
      let n = s;
      for (const id of a.cards) n = removeCard(n, id);
      n = {
        ...n,
        players: n.players.map((p, i) => (i === a.p ? { ...p, mode: "hand", cards: fill5(a.cards) } : p)),
        selP: a.p,
      };
      return { ...n, active: nextActive(n, null) };
    }
    case "selectPlayer":
      return { ...s, selP: a.p };
    case "restore":
      return reviveCalc(a.state);
    default:
      return s;
  }
}

/* ---- engine request building + validation ---------------------------- */

const bandActive = (b) => b.lo > 0 || b.hi < 100;

export function playerSpec(p, dbl) {
  if (p.mode === "random") return "random";
  if (p.mode === "range") {
    const [f, t] = p.chain;
    let s = "";
    if (!dbl && bandActive(t)) s = `${f.lo}-${f.hi}>${t.lo}-${t.hi}>`;
    else if (!dbl && bandActive(f)) s = `${f.lo}-${f.hi}>`;
    return s + `${p.lo}-${p.hi}`;
  }
  return cardsToStr(p.cards);
}

export function playerLabel(p, dbl) {
  if (p.mode === "random") return "Random hand";
  if (p.mode === "range") {
    const chained = !dbl && (bandActive(p.chain[0]) || bandActive(p.chain[1]));
    return `${p.lo}–${p.hi} pct${chained ? " (chained)" : ""}`;
  }
  return cardsToStr(p.cards);
}

/** why the setup can't be calculated yet, or null */
export function validate(s, ranksLoaded) {
  const nb = s.board.filter((c) => c != null).length;
  const nb2 = s.dbl ? s.board2.filter((c) => c != null).length : 0;
  if (nb === 1 || nb === 2) return `Board${s.dbl ? " A" : ""} needs 3, 4 or 5 cards (or none)`;
  if (s.dbl && (nb2 === 1 || nb2 === 2)) return "Board B needs 3, 4 or 5 cards (or none)";
  for (let i = 0; i < s.players.length; i++) {
    const p = s.players[i];
    if (p.mode === "hand") {
      const n = p.cards.filter((c) => c != null).length;
      if (n < 5)
        return `Player ${i + 1} needs ${5 - n} more card${n === 4 ? "" : "s"} — pick from the deck or type e.g. A then H for A${SUIT_SYM.h}`;
    } else if (p.mode === "range") {
      if (p.hi <= p.lo) return `Player ${i + 1}: the range is empty`;
      if (!s.dbl && (p.chain[0].hi <= p.chain[0].lo || p.chain[1].hi <= p.chain[1].lo))
        return `Player ${i + 1}: a street filter is empty`;
      if (nb === 0 && (!s.dbl || nb2 === 0) && !ranksLoaded)
        return "Preflop ranges need the rank table — build it once in Settings";
      if (s.dbl && (nb > 0) !== (nb2 > 0))
        return "Double-board ranges need both boards set (or both empty)";
    }
  }
  return null;
}

export function equityParams(s, preset, buildRanks) {
  return {
    players: s.players.map((p) => playerSpec(p, s.dbl)).join(","),
    board: cardsToStr(s.board),
    board2: s.dbl ? cardsToStr(s.board2) : "",
    dead: cardsToStr(s.dead),
    double: s.dbl,
    trials: preset.trials,
    maxenum: preset.maxenum,
    buildranks: buildRanks,
    seed: 3141592,
  };
}
