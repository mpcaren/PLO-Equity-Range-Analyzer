// Engine bridge vs the real plo5svc binary (build it first: `make plo5svc`).
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync } from "node:fs";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";

const require = createRequire(import.meta.url);
const { Engine } = require("../main/engine.cjs");

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const exe = path.join(root, process.platform === "win32" ? "plo5svc.exe" : "plo5svc");
const skip = !existsSync(exe) && "plo5svc not built (run `make plo5svc` in the repo root)";

// a rank path that never exists, so results don't depend on this machine
const args = ["--ranks", path.join(root, "no-such-rank-table.bin"), "--threads", "2"];

test("status, equity and errors", { skip }, async () => {
  const e = new Engine({ exe, args });
  try {
    const ready = await e.start();
    assert.equal(ready.event, "ready");
    assert.equal(ready.ranks, false);

    const st = await e.request("/api/status", "");
    assert.equal(st.status, 200);
    assert.equal(st.body.categories.length, 9);
    assert.equal(st.body.presets.length, 3);

    // AA vs a fixed hand on a flop: deterministic with a seed, equities sum to 100
    const q = new URLSearchParams({ players: "AhAdKcKd7c,9s8s7d6h5c", board: "QhJc2s", trials: "50000", maxenum: "0", seed: "7" });
    const r = await e.request("/api/equity", q.toString());
    assert.equal(r.status, 200);
    assert.equal(r.body.equity.length, 2);
    assert.ok(Math.abs(r.body.equity[0] + r.body.equity[1] - 100) < 1e-6);
    assert.ok(r.body.equity[0] > 50);

    const bad = await e.request("/api/equity", "players=AhAh2c3c4c,random");
    assert.equal(bad.status, 400);
    assert.match(bad.body.error, /duplicate|invalid/);

    const missing = await e.request("/api/nope", "");
    assert.equal(missing.status, 404);
  } finally {
    e.stop();
  }
});

test("board ranking streams progress", { skip }, async () => {
  const e = new Engine({ exe, args });
  try {
    const seen = [];
    const r = await e.request("/api/lookup", "pct=50&board=Kh7d2s", (p) => seen.push(p));
    assert.equal(r.status, 200);
    assert.equal(r.body.street, "flop");
    assert.ok(seen.length > 0, "expected progress events");
    assert.equal(seen.at(-1).what, "flop ranking");
    assert.equal(seen.at(-1).done, seen.at(-1).total);
  } finally {
    e.stop();
  }
});

test("cancelAll stops a long run and the engine keeps working", { skip }, async () => {
  const e = new Engine({ exe, args });
  try {
    await e.start();
    const long = e.request("/api/equity", "players=random,random,random,random,random,random&trials=2000000000&maxenum=0")
      .then(() => "finished", (err) => err);
    await new Promise((r) => setTimeout(r, 150));
    const t0 = Date.now();
    await e.cancelAll();
    const outcome = await long;
    assert.equal(outcome.cancelled, true);
    assert.ok(Date.now() - t0 < 2000, "cancel should be prompt");
    const st = await e.request("/api/status", "");
    assert.equal(st.status, 200);
  } finally {
    e.stop();
  }
});

test("a crashed engine is restarted", { skip }, async () => {
  const e = new Engine({ exe, args });
  try {
    await e.start();
    const downs = [];
    e.on("down", (err) => downs.push(err));
    e.proc.kill("SIGKILL");
    await new Promise((r) => setTimeout(r, 1200));
    assert.equal(downs.length, 1);
    const st = await e.request("/api/status", "");
    assert.equal(st.status, 200);
  } finally {
    e.stop();
  }
});
