/* Dual-handle percentile slider (0 = weakest, 100 = strongest).
 * Drag a handle, drag the band to slide the whole range, click the track
 * to move the nearest handle; arrow keys nudge the focused handle. */

import { useRef, useState } from "react";
import { motion } from "motion/react";
import { spring } from "./ui.jsx";

const clamp = (v) => Math.max(0, Math.min(100, Math.round(v)));

export function RangeSlider({ lo, hi, color, onChange, compact = false, label = "range" }) {
  const ref = useRef(null);
  const drag = useRef(null);
  const [dragging, setDragging] = useState(null);

  const pctAt = (clientX) => {
    const r = ref.current.getBoundingClientRect();
    return ((clientX - r.left) / r.width) * 100;
  };

  const onPointerDown = (e) => {
    if (e.button !== 0) return;
    const p = pctAt(e.clientX);
    const target = e.target.dataset.part;
    let mode;
    if (target === "lo" || target === "hi") mode = target;
    else if (target === "band" && hi - lo < 100) mode = "band";
    else mode = Math.abs(p - lo) <= Math.abs(p - hi) ? "lo" : "hi";
    drag.current = { mode, start: p, lo0: lo, hi0: hi };
    setDragging(mode);
    e.currentTarget.setPointerCapture(e.pointerId);
    if (mode !== "band") apply(p);
  };

  const apply = (p) => {
    const d = drag.current;
    if (!d) return;
    if (d.mode === "lo") onChange(Math.min(clamp(p), hi - 1), hi);
    else if (d.mode === "hi") onChange(lo, Math.max(clamp(p), lo + 1));
    else {
      const w = d.hi0 - d.lo0;
      const nlo = Math.max(0, Math.min(100 - w, Math.round(d.lo0 + (p - d.start))));
      onChange(nlo, nlo + w);
    }
  };

  const end = () => { drag.current = null; setDragging(null); };

  const onKey = (which) => (e) => {
    const step = e.shiftKey ? 10 : 1;
    let d = 0;
    if (e.key === "ArrowLeft" || e.key === "ArrowDown") d = -step;
    else if (e.key === "ArrowRight" || e.key === "ArrowUp") d = step;
    else return;
    e.preventDefault();
    if (which === "lo") onChange(Math.max(0, Math.min(hi - 1, lo + d)), hi);
    else onChange(lo, Math.min(100, Math.max(lo + 1, hi + d)));
  };

  return (
    <div
      ref={ref}
      className={`rs ${compact ? "rs-compact" : ""} ${dragging ? "dragging" : ""}`}
      style={{ "--pc": color }}
      onPointerDown={onPointerDown}
      onPointerMove={(e) => drag.current && apply(pctAt(e.clientX))}
      onPointerUp={end}
      onPointerCancel={end}
    >
      <div className="rs-track" />
      <motion.div
        className="rs-band"
        data-part="band"
        animate={{ left: `${lo}%`, width: `${hi - lo}%` }}
        transition={dragging ? { duration: 0 } : spring}
      />
      {[["lo", lo], ["hi", hi]].map(([which, v]) => (
        <motion.div
          key={which}
          className={`rs-thumb ${dragging === which ? "active" : ""}`}
          data-part={which}
          role="slider"
          tabIndex={0}
          aria-label={`${label} ${which === "lo" ? "lower" : "upper"} bound`}
          aria-valuemin={0}
          aria-valuemax={100}
          aria-valuenow={v}
          onKeyDown={onKey(which)}
          animate={{ left: `${v}%` }}
          transition={dragging ? { duration: 0 } : spring}
        >
          {!compact && <span className="rs-tip">{v}</span>}
        </motion.div>
      ))}
    </div>
  );
}

export const RANGE_PRESETS = [
  { label: "Top 5%", lo: 95, hi: 100 },
  { label: "Top 15%", lo: 85, hi: 100 },
  { label: "Top 30%", lo: 70, hi: 100 },
  { label: "Top 50%", lo: 50, hi: 100 },
  { label: "Any", lo: 0, hi: 100 },
];
