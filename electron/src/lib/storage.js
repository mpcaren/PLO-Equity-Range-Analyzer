/* localStorage helpers: per-machine conveniences only (settings, the last
 * calculator setup). Every access is guarded — storage can be unavailable
 * and the app must work without it. */

const PREFIX = "plo5lab:";

export function load(key, fallback) {
  try {
    const raw = window.localStorage.getItem(PREFIX + key);
    return raw == null ? fallback : JSON.parse(raw);
  } catch {
    return fallback;
  }
}

export function save(key, value) {
  try {
    window.localStorage.setItem(PREFIX + key, JSON.stringify(value));
  } catch {
    /* ignore */
  }
}
