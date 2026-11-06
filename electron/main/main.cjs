/* main.cjs — Electron main process for PLO5 Equity Lab.
 *
 * Owns the window and the engine process (plo5svc). The renderer never
 * touches Node: it reaches the engine only through the narrow, validated
 * IPC surface exposed by preload.cjs.
 */
"use strict";

const { app, BrowserWindow, ipcMain, dialog, Menu, screen, shell } = require("electron");
const fs = require("node:fs");
const path = require("node:path");
const { Engine } = require("./engine.cjs");

const isMac = process.platform === "darwin";
const EXE = process.platform === "win32" ? "plo5svc.exe" : "plo5svc";
const REPO_ROOT = path.resolve(__dirname, "..", "..");
const DEV_URL = process.env.VITE_DEV_SERVER_URL || null;

/* ---- locating the engine, rank table and config ---------------------- */

const exists = (p) => { try { return fs.statSync(p).isFile(); } catch { return false; } };

function svcPath() {
  const c = [
    process.env.PLO5_SVC,
    app.isPackaged ? path.join(process.resourcesPath, "engine", EXE) : null,
    path.join(REPO_ROOT, EXE),
  ].filter(Boolean);
  return c.find(exists) || c[0];
}

/* The rank table is generated once per machine (~24 MB). Dev builds share
 * the repo-root copy with the other tools (plo5setup writes it there);
 * installed builds keep theirs in the per-user data folder. */
function rankCandidates() {
  const user = path.join(app.getPath("userData"), "plo5rank.bin");
  const repo = path.join(REPO_ROOT, "plo5rank.bin");
  const res = app.isPackaged ? path.join(process.resourcesPath, "engine", "plo5rank.bin") : null;
  return [process.env.PLO5_RANKS, ...(app.isPackaged ? [user, res] : [repo, user])].filter(Boolean);
}

function rankPath() {
  const c = rankCandidates();
  return c.find(exists) || c[0];
}

function configPath() {
  const c = [
    process.env.PLO5_CONFIG,
    app.isPackaged ? null : path.join(REPO_ROOT, "plo5config.ini"),
    path.join(app.getPath("userData"), "plo5config.ini"),
  ].filter(Boolean);
  return c.find(exists) || null;
}

let engine = null;
let engineSpec = null;

function startEngine() {
  if (engine) engine.stop();
  const exe = svcPath();
  const ranks = rankPath();
  const cfg = configPath();
  const args = ["--ranks", ranks];
  if (cfg) args.push("--config", cfg);
  engineSpec = { exe, ranks, cfg, found: exists(exe) };
  engine = new Engine({ exe, args, cwd: path.dirname(exe) });
  engine.on("down", (err) => {
    for (const w of BrowserWindow.getAllWindows())
      w.webContents.send("engine:down", String(err && err.message || err));
  });
  if (engineSpec.found) engine.start();
  return engineSpec;
}

/* ---- IPC ------------------------------------------------------------- */

const ENDPOINTS = new Set([
  "/api/equity", "/api/lookup", "/api/rankof", "/api/findeq",
  "/api/random", "/api/sprquiz", "/api/status", "/svc/info", "/svc/genranks",
]);

function encodeParams(params) {
  const q = new URLSearchParams();
  if (params && typeof params === "object")
    for (const [k, v] of Object.entries(params)) {
      if (!/^[a-z0-9]{1,24}$/i.test(k)) continue;
      if (typeof v === "string" || typeof v === "number" || typeof v === "boolean")
        q.set(k, typeof v === "boolean" ? (v ? "1" : "0") : String(v).slice(0, 2000));
    }
  return q.toString(); // percent-encodes tabs/newlines, so one line per request
}

function registerIpc() {
  ipcMain.handle("engine:info", async () => {
    const info = {
      ...engineSpec,
      platform: process.platform,
      version: app.getVersion(),
      userData: app.getPath("userData"),
      ready: null,
      error: null,
    };
    if (!engineSpec.found) return info;
    try {
      info.ready = await engine.ready;
    } catch (e) {
      info.error = String(e.message || e);
    }
    return info;
  });

  ipcMain.handle("engine:call", async (event, req) => {
    const endpoint = req && req.endpoint;
    if (!ENDPOINTS.has(endpoint)) return { error: "unknown endpoint" };
    if (!engineSpec.found) return { error: "engine not found" };
    const token = req.token;
    const onProgress = token == null ? undefined : (p) => {
      if (!event.sender.isDestroyed())
        event.sender.send("engine:progress", { token, ...p });
    };
    try {
      return await engine.request(endpoint, encodeParams(req.params), onProgress);
    } catch (e) {
      if (e && e.cancelled) return { cancelled: true };
      return { error: String(e && e.message || e) };
    }
  });

  ipcMain.handle("engine:cancel", async () => {
    if (engineSpec.found) await engine.cancelAll().catch(() => {});
    return true;
  });

  /* copy an existing plo5rank.bin (e.g. built by plo5setup) into place */
  ipcMain.handle("ranks:import", async (event) => {
    const win = BrowserWindow.fromWebContents(event.sender);
    const r = await dialog.showOpenDialog(win, {
      title: "Locate plo5rank.bin",
      properties: ["openFile"],
      filters: [{ name: "PLO5 rank table", extensions: ["bin"] }],
    });
    if (r.canceled || !r.filePaths[0]) return { canceled: true };
    const src = r.filePaths[0];
    const dest = path.join(app.getPath("userData"), "plo5rank.bin");
    try {
      if (path.resolve(src) !== path.resolve(dest)) {
        fs.mkdirSync(path.dirname(dest), { recursive: true });
        fs.copyFileSync(src, dest);
      }
    } catch (e) {
      return { error: String(e.message || e) };
    }
    startEngine();
    return { ok: true };
  });

  ipcMain.handle("app:reveal", (_e, which) => {
    const p = which === "ranks" ? engineSpec.ranks : which === "engine" ? engineSpec.exe : null;
    if (p && exists(p)) shell.showItemInFolder(p);
    else if (p) shell.openPath(path.dirname(p));
  });
}

/* ---- window ---------------------------------------------------------- */

function createWindow() {
  // roomy by default: most of the screen, capped so it does not sprawl
  const wa = screen.getPrimaryDisplay().workAreaSize;
  const win = new BrowserWindow({
    width: Math.max(1080, Math.min(1520, Math.round(wa.width * 0.92))),
    height: Math.max(700, Math.min(1060, Math.round(wa.height * 0.94))),
    minWidth: 1080,
    minHeight: 700,
    show: false,
    backgroundColor: "#090d0b",
    title: "PLO5 Equity Lab",
    icon: path.join(__dirname, "..", "build", "icon.png"),
    titleBarStyle: "hidden",
    ...(isMac
      ? { trafficLightPosition: { x: 18, y: 16 } }
      : { titleBarOverlay: { color: "#0b100e", symbolColor: "#b8c4be", height: 48 } }),
    webPreferences: {
      preload: path.join(__dirname, "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      spellcheck: false,
    },
  });

  win.once("ready-to-show", () => win.show());

  // the app is a single page: never navigate away or open new windows
  win.webContents.on("will-navigate", (e, url) => {
    if (!DEV_URL || !url.startsWith(DEV_URL)) e.preventDefault();
  });
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (/^https:\/\//.test(url)) shell.openExternal(url);
    return { action: "deny" };
  });

  if (DEV_URL) win.loadURL(DEV_URL);
  else win.loadFile(path.join(__dirname, "..", "dist", "index.html"));
  return win;
}

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on("second-instance", () => {
    const w = BrowserWindow.getAllWindows()[0];
    if (w) { if (w.isMinimized()) w.restore(); w.focus(); }
  });

  app.whenReady().then(() => {
    if (!isMac) Menu.setApplicationMenu(null);
    startEngine();
    registerIpc();
    createWindow();
    app.on("activate", () => {
      if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
  });

  app.on("window-all-closed", () => {
    if (!isMac) app.quit();
  });

  app.on("before-quit", () => { if (engine) engine.stop(); });
}
