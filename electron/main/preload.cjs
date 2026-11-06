/* preload.cjs — the only bridge between the page and the main process.
 * Exposes window.plo5 with a handful of promise-returning calls. */
"use strict";

const { contextBridge, ipcRenderer } = require("electron");

const progressListeners = new Map(); // token -> fn
ipcRenderer.on("engine:progress", (_e, p) => {
  const fn = progressListeners.get(p.token);
  if (fn) fn(p);
});

const downListeners = new Set();
ipcRenderer.on("engine:down", (_e, msg) => { for (const fn of downListeners) fn(msg); });

let nextToken = 1;

contextBridge.exposeInMainWorld("plo5", {
  platform: process.platform,

  info: () => ipcRenderer.invoke("engine:info"),

  /** call an engine endpoint; onProgress({what, done, total}) is optional */
  call: async (endpoint, params, onProgress) => {
    const token = onProgress ? nextToken++ : null;
    if (token != null) progressListeners.set(token, onProgress);
    try {
      return await ipcRenderer.invoke("engine:call", { endpoint, params, token });
    } finally {
      if (token != null) progressListeners.delete(token);
    }
  },

  cancel: () => ipcRenderer.invoke("engine:cancel"),
  importRanks: () => ipcRenderer.invoke("ranks:import"),
  reveal: (which) => ipcRenderer.invoke("app:reveal", which),

  onEngineDown: (fn) => {
    downListeners.add(fn);
    return () => downListeners.delete(fn);
  },
});
