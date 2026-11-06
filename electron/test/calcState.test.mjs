// Calculator reducer: dealing order, card ownership, validation, request specs.
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  calcReducer, initialCalc, playerSpec, reviveCalc, usedCards, validate, equityParams,
} from "../src/lib/calcState.js";
import { parseLooseCards, strToIds } from "../src/lib/cards.js";

const deal = (s, str) => strToIds(str).reduce((st, card) => calcReducer(st, { type: "pick", card }), s);

test("picking cards fills player 1, then player 2, then the board", () => {
  let s = deal(initialCalc(), "AhAdKcKd7c9s8s7d6h5cQhJc2s");
  assert.deepEqual(s.players[0].cards, strToIds("AhAdKcKd7c"));
  assert.deepEqual(s.players[1].cards, strToIds("9s8s7d6h5c"));
  assert.deepEqual(s.board.slice(0, 3), strToIds("QhJc2s"));
  assert.deepEqual(s.active, { zone: "b", p: undefined, i: 3 });
  assert.equal(validate(s, false), null);
});

test("picking a card that is in use returns it to the deck", () => {
  let s = deal(initialCalc(), "AhKh");
  const kh = strToIds("Kh")[0];
  assert.ok(usedCards(s).has(kh));
  s = calcReducer(s, { type: "pick", card: kh });
  assert.ok(!usedCards(s).has(kh));
  assert.equal(s.players[0].cards[1], null);
});

test("paste fills consecutive slots and skips used cards", () => {
  let s = deal(initialCalc(), "Ah");
  s = calcReducer(s, { type: "paste", cards: parseLooseCards("A♥ K♠ 10d qc 2c 3c") });
  assert.deepEqual(s.players[0].cards, strToIds("AhKsTdQc2c"));
  assert.equal(s.players[1].cards[0], strToIds("3c")[0]);
});

test("removing a player shifts the active slot and selection", () => {
  let s = calcReducer(initialCalc(), { type: "addPlayer" });
  s = calcReducer(s, { type: "focus", slot: { zone: "p", p: 2, i: 1 } });
  s = calcReducer(s, { type: "removePlayer", p: 0 });
  assert.equal(s.players.length, 2);
  assert.deepEqual(s.active, { zone: "p", p: 1, i: 1 });
  assert.equal(calcReducer(s, { type: "removePlayer", p: 0 }).players.length, 2, "never below two players");
});

test("dealing a hand takes its cards from wherever they were", () => {
  let s = deal(initialCalc(), "AhAdKcKd7c9s8s7d6h5cQhJc2s");
  s = calcReducer(s, { type: "dealHand", p: 1, cards: strToIds("QhJc2sTsTd") });
  assert.deepEqual(s.players[1].cards, strToIds("QhJc2sTsTd"));
  assert.deepEqual(s.board.slice(0, 3), [null, null, null]);
});

test("validation messages", () => {
  let s = deal(initialCalc(), "AhAdKcKd7c");
  assert.match(validate(s, true), /Player 2 needs 5 more cards/);
  s = calcReducer(s, { type: "setMode", p: 1, mode: "range" });
  assert.match(validate(s, false), /rank table/);
  assert.equal(validate(s, true), null);
  s = deal(s, "Qh");
  assert.match(validate(s, true), /Board needs 3/);
});

test("range specs include street filters only on a single board", () => {
  const p = { mode: "range", lo: 40, hi: 100, chain: [{ lo: 0, hi: 50 }, { lo: 0, hi: 100 }], cards: [] };
  assert.equal(playerSpec(p, false), "0-50>40-100");
  assert.equal(playerSpec(p, true), "40-100");
  const both = { ...p, chain: [{ lo: 0, hi: 50 }, { lo: 20, hi: 100 }] };
  assert.equal(playerSpec(both, false), "0-50>20-100>40-100");
  assert.equal(playerSpec({ mode: "random" }, false), "random");
});

test("equity params", () => {
  let s = deal(initialCalc(), "AhAdKcKd7c");
  s = calcReducer(s, { type: "setMode", p: 1, mode: "random" });
  const q = equityParams(s, { trials: 1000, maxenum: 10 }, true);
  assert.equal(q.players, "AhAdKcKd7c,random");
  assert.equal(q.board, "");
  assert.equal(q.double, false);
});

test("revive drops duplicates and malformed data", () => {
  const s = reviveCalc({
    players: [{ mode: "hand", cards: [1, 1, 2, 99, "x"] }, { mode: "bogus" }],
    board: [2, 3, 4], dbl: false,
  });
  assert.deepEqual(s.players[0].cards, [1, null, 2, null, null]);
  assert.equal(s.players[1].mode, "hand");
  assert.deepEqual(s.board, [null, 3, 4, null, null]);
  assert.equal(reviveCalc(null).players.length, 2);
});
