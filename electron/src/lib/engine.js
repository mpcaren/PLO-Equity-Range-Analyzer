/* Renderer-side engine client: thin wrapper over window.plo5 (preload). */

const bridge = typeof window !== "undefined" ? window.plo5 : undefined;

export const hasBridge = !!bridge;
export const platform = bridge ? bridge.platform : "web";

export class EngineError extends Error {}
export class Cancelled extends Error {
  constructor() { super("cancelled"); this.cancelled = true; }
}

/**
 * Call an engine endpoint (see src/api.c / src/svc.c for parameters).
 * Resolves with the JSON body; throws EngineError / Cancelled.
 */
export async function call(endpoint, params = {}, onProgress) {
  if (!bridge) throw new EngineError("desktop bridge unavailable");
  const r = await bridge.call(endpoint, params, onProgress);
  if (r.cancelled) throw new Cancelled();
  if (r.error) throw new EngineError(r.error);
  if (r.body && r.body.error) throw new EngineError(r.body.error);
  return r.body;
}

export const cancelAll = () => (bridge ? bridge.cancel() : Promise.resolve());
export const engineInfo = () => (bridge ? bridge.info() : Promise.resolve(null));
export const importRanks = () => (bridge ? bridge.importRanks() : Promise.resolve({ canceled: true }));
export const reveal = (which) => bridge && bridge.reveal(which);
export const onEngineDown = (fn) => (bridge ? bridge.onEngineDown(fn) : () => {});
