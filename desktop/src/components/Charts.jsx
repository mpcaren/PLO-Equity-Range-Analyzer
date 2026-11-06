/* Equity donut + hand-type distribution modal. */

import { useState } from "react";
import { motion } from "motion/react";
import { AnimatedNumber, Modal, softSpring } from "./ui.jsx";
import { CATEGORY_COLORS } from "../lib/cards.js";

/* donut of pot shares; each segment springs to its new arc */
export function EquityDonut({ shares, colors, labels, size = 168, stroke = 16, stale }) {
  const [hover, setHover] = useState(null);
  const r = (size - stroke) / 2;
  const gap = shares.length > 1 ? 0.006 : 0;
  let acc = 0;
  const segs = shares.map((v, i) => {
    const frac = Math.max(0, v) / 100;
    const s = { i, start: acc, len: Math.max(0, frac - gap) };
    acc += frac;
    return s;
  });
  const focus = hover != null ? hover : shares.indexOf(Math.max(...shares));
  return (
    <div className={`donut ${stale ? "stale" : ""}`} style={{ width: size, height: size }}>
      <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`}>
        <circle cx={size / 2} cy={size / 2} r={r} fill="none" className="donut-track" strokeWidth={stroke} />
        {segs.map((s) => (
          <motion.circle
            key={s.i}
            cx={size / 2} cy={size / 2} r={r}
            fill="none"
            stroke={colors[s.i]}
            strokeWidth={hover === s.i ? stroke + 4 : stroke}
            strokeLinecap="butt"
            initial={{ pathLength: 0, pathOffset: 0 }}
            animate={{ pathLength: s.len, pathOffset: s.start }}
            transition={softSpring}
            style={{ rotate: -90, transformOrigin: "50% 50%" }}
            onMouseEnter={() => setHover(s.i)}
            onMouseLeave={() => setHover(null)}
          />
        ))}
      </svg>
      <div className="donut-center">
        {focus >= 0 && shares[focus] != null && (
          <>
            <span className="donut-lbl" style={{ color: colors[focus] }}>{labels[focus]}</span>
            <span className="donut-val"><AnimatedNumber value={shares[focus]} decimals={1} />%</span>
          </>
        )}
      </div>
    </div>
  );
}

export function HandTypesModal({ open, onClose, result, labels, colors, categories }) {
  if (!result) return <Modal open={false} onClose={onClose} title="" />;
  const boards = result.double
    ? [["Board A", result.handType], ["Board B", result.handTypeB]]
    : [[null, result.handType]];
  return (
    <Modal open={open} onClose={onClose} title="Hand type distribution" wide>
      <p className="modal-note">
        How often each player's best hand at showdown lands in each category, given the current board.
      </p>
      <div className="legend">
        {categories.map((c, i) => (
          <span key={c}><i style={{ background: CATEGORY_COLORS[i] }} />{c}</span>
        ))}
      </div>
      {boards.map(([bname, ht], bi) => (
        <div key={bi} className="ht-board">
          {bname && <h4>{bname}</h4>}
          {ht.map((dist, p) => (
            <div key={p} className="ht-row">
              <span className="ht-who" style={{ color: colors[p] }}>{labels[p]}</span>
              <div className="ht-bar">
                {dist.map((v, i) => (
                  <motion.span key={i} style={{ background: CATEGORY_COLORS[i] }}
                    initial={{ width: 0 }} animate={{ width: `${v}%` }}
                    transition={{ ...softSpring, delay: 0.05 + p * 0.04 }}
                    title={`${categories[i]} ${v.toFixed(2)}%`}>
                    {v >= 8 && <em>{v.toFixed(0)}%</em>}
                  </motion.span>
                ))}
              </div>
            </div>
          ))}
          <div className="ht-table-wrap">
            <table className="ht-table">
              <thead>
                <tr><th />{categories.map((c) => <th key={c}>{c}</th>)}</tr>
              </thead>
              <tbody>
                {ht.map((dist, p) => (
                  <tr key={p}>
                    <td style={{ color: colors[p] }}>{labels[p]}</td>
                    {dist.map((v, i) => <td key={i}>{v < 0.005 ? "—" : v.toFixed(2)}</td>)}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      ))}
    </Modal>
  );
}
