/* Renderer-side engine client over Tauri commands (src-tauri/src/lib.rs).
 * The engine is the Rust `plo5` crate, running in-process. */

import { Channel, invoke, isTauri } from "@tauri-apps/api/core";

const inTauri = typeof window !== "undefined" && isTauri();

export const hasBridge = inTauri;

const ua = typeof navigator !== "undefined" ? navigator.userAgent : "";
export const platform = /Mac/i.test(ua) ? "darwin" : /Windows/i.test(ua) ? "win32" : "linux";

export class EngineError extends Error {}
export class Cancelled extends Error {
  constructor() { super("cancelled"); this.cancelled = true; }
}

/**
 * Call an engine endpoint (see engine/src/api.rs for parameters).
 * Resolves with the JSON body; throws EngineError / Cancelled.
 */
export async function call(endpoint, params = {}, onProgress) {
  if (!inTauri) throw new EngineError("desktop bridge unavailable");
  const channel = new Channel();
  if (onProgress) channel.onmessage = onProgress;
  const r = await invoke("engine_call", { endpoint, params, progress: !!onProgress, onProgress: channel });
  if (r.cancelled) throw new Cancelled();
  if (r.error) throw new EngineError(r.error);
  if (r.body && r.body.error) throw new EngineError(r.body.error);
  return r.body;
}

export const cancelAll = () => (inTauri ? invoke("engine_cancel") : Promise.resolve());
export const engineInfo = () => (inTauri ? invoke("engine_info") : Promise.resolve(null));
export const importRanks = () => (inTauri ? invoke("ranks_import") : Promise.resolve({ canceled: true }));
export const reveal = (which) => inTauri && invoke("app_reveal", { which });
/* the engine runs in-process: there is no separate process to go down */
export const onEngineDown = () => () => {};
