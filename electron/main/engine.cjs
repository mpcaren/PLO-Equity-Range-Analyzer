/* engine.cjs — runs plo5svc (the C engine) as a child process and
 * speaks its line protocol (see src/svc.c):
 *
 *   -> "<id>\t<endpoint>\t<query>\n"
 *   <- {"id":N,"status":200,"body":{..}} | {"id":N,"progress":{..}}
 *
 * Requests are queued by the service and answered in order. cancelAll()
 * kills the process (the only way to stop a long Monte Carlo run or a
 * rank-table build mid-flight) and starts a fresh one.
 */
"use strict";

const { spawn } = require("node:child_process");
const { EventEmitter } = require("node:events");
const readline = require("node:readline");

class CancelledError extends Error {
  constructor() {
    super("cancelled");
    this.cancelled = true;
  }
}

class Engine extends EventEmitter {
  /**
   * @param {object} opts
   * @param {string} opts.exe       path to plo5svc / plo5svc.exe
   * @param {string[]} [opts.args]  extra command-line arguments
   * @param {string} [opts.cwd]
   */
  constructor({ exe, args = [], cwd }) {
    super();
    this.exe = exe;
    this.args = args;
    this.cwd = cwd;
    this.proc = null;
    this.nextId = 1;
    this.pending = new Map(); // id -> { resolve, reject, onProgress }
    this.ready = null;        // promise resolved by the startup line
    this.readyInfo = null;
    this.stopping = false;
    this.crashes = 0;
  }

  start() {
    if (this.proc) return this.ready;
    this.stopping = false;
    const proc = spawn(this.exe, this.args, {
      cwd: this.cwd,
      stdio: ["pipe", "pipe", "pipe"],
      windowsHide: true,
    });
    this.proc = proc;

    let resolveReady, rejectReady;
    this.ready = new Promise((res, rej) => { resolveReady = res; rejectReady = rej; });
    this.ready.catch(() => {}); // observed via request()

    const rl = readline.createInterface({ input: proc.stdout });
    rl.on("line", (line) => {
      let msg;
      try { msg = JSON.parse(line); } catch { return; }
      if (msg.event === "ready") {
        this.readyInfo = msg;
        this.crashes = 0;
        resolveReady(msg);
        return;
      }
      const p = this.pending.get(msg.id);
      if (!p) return;
      if (msg.progress) {
        if (p.onProgress) p.onProgress(msg.progress);
        return;
      }
      this.pending.delete(msg.id);
      p.resolve({ status: msg.status, body: msg.body });
    });

    let stderr = "";
    proc.stderr.on("data", (d) => { stderr = (stderr + d).slice(-4000); });

    proc.on("error", (err) => {
      rejectReady(err);
      this._failAll(err);
      this.proc = null;
      this.emit("down", err);
    });

    proc.on("exit", (code, signal) => {
      if (this.proc !== proc) return; // an old process we replaced
      this.proc = null;
      const err = new Error(`engine exited (${signal || code})${stderr ? ": " + stderr.trim() : ""}`);
      rejectReady(err);
      this._failAll(err);
      if (!this.stopping) {
        // crashed: restart with backoff so the UI keeps working
        this.crashes++;
        this.emit("down", err);
        const delay = Math.min(5000, 250 * 2 ** this.crashes);
        setTimeout(() => { if (!this.proc && !this.stopping) this.start(); }, delay);
      }
    });

    return this.ready;
  }

  _failAll(err) {
    for (const [, p] of this.pending) p.reject(err);
    this.pending.clear();
  }

  /**
   * @param {string} endpoint  e.g. "/api/equity"
   * @param {string} query     url-encoded query string
   * @param {(p:{what:string,done:number,total:number})=>void} [onProgress]
   */
  async request(endpoint, query, onProgress) {
    if (!this.proc) this.start();
    const proc = this.proc;
    await this.ready;
    // cancelled or crashed while waiting for startup
    if (!proc || this.proc !== proc) throw new CancelledError();
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, onProgress });
      this.proc.stdin.write(`${id}\t${endpoint}\t${query}\n`);
    });
  }

  /** stop everything in flight and start a fresh engine */
  cancelAll() {
    const old = this.proc;
    const pending = this.pending;
    this.pending = new Map();
    this.proc = null;
    for (const [, p] of pending) p.reject(new CancelledError());
    if (old) {
      old.stdin.end();
      old.kill();
    }
    return this.start();
  }

  stop() {
    this.stopping = true;
    const old = this.proc;
    this.proc = null;
    this._failAll(new CancelledError());
    if (old) {
      old.stdin.end();
      old.kill();
    }
  }
}

module.exports = { Engine, CancelledError };
