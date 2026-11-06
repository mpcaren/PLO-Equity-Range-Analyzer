/* Card helpers — same encoding as the engine: id = rank*4 + suit,
 * rank 0 = '2' .. 12 = 'A'; suit 0 = c, 1 = d, 2 = h, 3 = s. */

export const RANKS = "23456789TJQKA";
export const SUITS = "cdhs";
export const SUIT_SYM = { c: "♣", d: "♦", h: "♥", s: "♠" };
export const SUIT_NAME = { c: "clubs", d: "diamonds", h: "hearts", s: "spades" };

export const idRank = (id) => RANKS[id >> 2];
export const idSuit = (id) => SUITS[id & 3];
export const idToStr = (id) => idRank(id) + idSuit(id);
export const cardsToStr = (arr) => arr.filter((c) => c != null).map(idToStr).join("");
export const cardLabel = (id) => `${idRank(id) === "T" ? "10" : idRank(id)} of ${SUIT_NAME[idSuit(id)]}`;

/** "AhKd.." -> ids, or null if anything is malformed */
export function strToIds(s) {
  const t = String(s || "").replace(/[\s,]/g, "");
  if (t.length % 2) return null;
  const out = [];
  for (let i = 0; i < t.length; i += 2) {
    const r = RANKS.indexOf(t[i].toUpperCase());
    const u = SUITS.indexOf(t[i + 1].toLowerCase());
    if (r < 0 || u < 0) return null;
    out.push(r * 4 + u);
  }
  return out;
}

const UNI_SUIT = { "♣": "c", "♧": "c", "♦": "d", "♢": "d", "♥": "h", "♡": "h", "♠": "s", "♤": "s" };

/** forgiving parser for pasted text: "A♥ K♠ 10d qc" -> ids (invalid bits skipped) */
export function parseLooseCards(text) {
  const s = String(text || "")
    .replace(/10/g, "T")
    .replace(/[♣♧♦♢♥♡♠♤]/g, (m) => UNI_SUIT[m]);
  const out = [];
  const re = /([2-9TJQKA])\s*([cdhs])/gi;
  let m;
  while ((m = re.exec(s))) {
    const id = RANKS.indexOf(m[1].toUpperCase()) * 4 + SUITS.indexOf(m[2].toLowerCase());
    if (!out.includes(id)) out.push(id);
  }
  return out;
}

export const sortDesc = (ids) => [...ids].sort((a, b) => b - a);

export const fmtInt = (n) => Number(n).toLocaleString("en-US");

export const PLAYER_COLORS = ["#5cb2ff", "#ff7a93", "#ffc65c", "#b38cff", "#3fe0c5", "#a6e35e"];

export const CATEGORY_COLORS = [
  "#64748b", "#60a5fa", "#22d3ee", "#34d399", "#a3e635",
  "#facc15", "#fb923c", "#f472b6", "#c084fc",
];

export const DEFAULT_CATEGORIES = [
  "High Card", "Pair", "Two Pair", "Trips", "Straight",
  "Flush", "Full House", "Quads", "Straight Flush",
];
