/* One player row: identity, mode, hand / range editor, and results. */

import { memo } from "react";
import { AnimatePresence, motion } from "motion/react";
import { CardRow } from "./Cards.jsx";
import { RangeSlider, RANGE_PRESETS } from "./RangeSlider.jsx";
import { AnimatedNumber, IconButton, PctInput, Segmented, softSpring, spring } from "./ui.jsx";
import { IconChevron, IconFilter, IconTrash, IconX } from "./Icons.jsx";
import { CATEGORY_COLORS, PLAYER_COLORS } from "../lib/cards.js";

const MODES = [
  { value: "hand", label: "Hand" },
  { value: "range", label: "Range" },
  { value: "random", label: "Random" },
];

const bandOn = (b) => b.lo > 0 || b.hi < 100;

export const PlayerCard = memo(function PlayerCard({
  p, pi, color, active, dispatch, dbl, res, stale, calculating, canRemove, categories, onHandTypes, selected,
}) {
  const patch = (patch) => dispatch({ type: "patchPlayer", p: pi, patch });
  const chained = !dbl && (bandOn(p.chain[0]) || bandOn(p.chain[1]));

  return (
    <motion.article
      layout
      className={`player ${selected ? "selected" : ""}`}
      style={{ "--pc": color }}
      initial={{ opacity: 0, y: 18, scale: 0.98 }}
      animate={{ opacity: 1, y: 0, scale: 1 }}
      exit={{ opacity: 0, x: -24, scale: 0.97, transition: { duration: 0.18 } }}
      transition={softSpring}
      onMouseDown={() => !selected && dispatch({ type: "selectPlayer", p: pi })}
    >
      <div className="player-stripe" />
      <div className="player-main">
        <div className="player-top">
          <motion.div layout="position" className="player-badge">P{pi + 1}</motion.div>
          <Segmented
            size="sm"
            value={p.mode}
            options={MODES}
            layoutKey={`mode-${p.id}`}
            onChange={(mode) => dispatch({ type: "setMode", p: pi, mode })}
          />
          <div className="player-tools">
            {p.mode === "hand" && (
              <IconButton icon={IconX} label="Clear hand" onClick={() => dispatch({ type: "clearPlayer", p: pi })} />
            )}
            {canRemove && (
              <IconButton icon={IconTrash} label="Remove player" className="danger"
                onClick={() => dispatch({ type: "removePlayer", p: pi })} />
            )}
          </div>
        </div>

        <AnimatePresence mode="popLayout" initial={false}>
          <motion.div
            key={p.mode}
            className="player-body"
            initial={{ opacity: 0, y: 8, filter: "blur(4px)" }}
            animate={{ opacity: 1, y: 0, filter: "blur(0px)" }}
            exit={{ opacity: 0, y: -6, filter: "blur(4px)", transition: { duration: 0.12 } }}
            transition={softSpring}
          >
            {p.mode === "hand" && (
              <div className="hand-line">
                <CardRow cards={p.cards} zone="p" p={pi} active={active} dispatch={dispatch} label={`Player ${pi + 1}`} />
                <AnimatePresence>
                  {res && res.pct != null && (
                    <motion.span
                      className="pct-chip"
                      initial={{ opacity: 0, scale: 0.8 }}
                      animate={{ opacity: 1, scale: 1 }}
                      exit={{ opacity: 0, scale: 0.8 }}
                      transition={spring}
                      title="Hand-strength percentile on the current street (100 = strongest)"
                    >
                      <AnimatedNumber value={res.pct} decimals={1} />
                      <small>pctile</small>
                    </motion.span>
                  )}
                </AnimatePresence>
              </div>
            )}

            {p.mode === "range" && (
              <div className="range-edit">
                <div className="range-line">
                  <PctInput value={p.lo} onChange={(v) => patch({ lo: Math.min(v, p.hi - 1) })} aria-label="Range low percentile" />
                  <RangeSlider lo={p.lo} hi={p.hi} color={color} onChange={(lo, hi) => patch({ lo, hi })} label={`Player ${pi + 1} range`} />
                  <PctInput value={p.hi} onChange={(v) => patch({ hi: Math.max(v, p.lo + 1) })} aria-label="Range high percentile" />
                </div>
                <div className="range-meta">
                  <div className="presets">
                    {RANGE_PRESETS.map((r) => (
                      <button key={r.label} type="button"
                        className={`preset ${p.lo === r.lo && p.hi === r.hi ? "on" : ""}`}
                        onClick={() => patch({ lo: r.lo, hi: r.hi })}>{r.label}</button>
                    ))}
                  </div>
                  {!dbl && (
                    <button type="button" className={`filters-toggle ${chained ? "chained" : ""}`}
                      onClick={() => patch({ filtersOpen: !p.filtersOpen })}
                      aria-expanded={p.filtersOpen}>
                      <IconFilter size={13} />
                      {chained ? "Street filters on" : "Street filters"}
                      <motion.span animate={{ rotate: p.filtersOpen ? 180 : 0 }} transition={spring} className="chev">
                        <IconChevron size={13} />
                      </motion.span>
                    </button>
                  )}
                </div>
                <AnimatePresence initial={false}>
                  {!dbl && p.filtersOpen && (
                    <motion.div
                      className="filters"
                      initial={{ height: 0, opacity: 0 }}
                      animate={{ height: "auto", opacity: 1 }}
                      exit={{ height: 0, opacity: 0 }}
                      transition={softSpring}
                    >
                      <p className="filters-help">
                        Keep only this slice of the previous street's survivors, re-ranked each street.
                        0–100 = no filter.
                      </p>
                      {["Flop", "Turn"].map((street, k) => (
                        <div className="filter-line" key={street}>
                          <span className="filter-name">{street}</span>
                          <PctInput value={p.chain[k].lo} aria-label={`${street} filter low`}
                            onChange={(v) => patch({ chain: p.chain.map((b, j) => (j === k ? { ...b, lo: Math.min(v, b.hi - 1) } : b)) })} />
                          <RangeSlider compact lo={p.chain[k].lo} hi={p.chain[k].hi} color={color} label={`${street} filter`}
                            onChange={(lo, hi) => patch({ chain: p.chain.map((b, j) => (j === k ? { lo, hi } : b)) })} />
                          <PctInput value={p.chain[k].hi} aria-label={`${street} filter high`}
                            onChange={(v) => patch({ chain: p.chain.map((b, j) => (j === k ? { ...b, hi: Math.max(v, b.lo + 1) } : b)) })} />
                        </div>
                      ))}
                    </motion.div>
                  )}
                </AnimatePresence>
              </div>
            )}

            {p.mode === "random" && (
              <div className="random-body">
                <div className="random-stack" aria-hidden>
                  {[0, 1, 2, 3, 4].map((i) => (
                    <motion.span key={i} className="card-back"
                      initial={{ rotate: 0, x: 0 }}
                      animate={{ rotate: (i - 2) * 7, x: (i - 2) * 9 }}
                      transition={{ ...spring, delay: i * 0.03 }} />
                  ))}
                </div>
                <span>A fresh random hand every trial</span>
              </div>
            )}
          </motion.div>
        </AnimatePresence>
      </div>

      <Results res={res} color={color} stale={stale} calculating={calculating} dbl={dbl}
        categories={categories} onHandTypes={onHandTypes} />
    </motion.article>
  );
});

function Results({ res, color, stale, calculating, dbl, categories, onHandTypes }) {
  return (
    <div className={`results ${stale ? "stale" : ""} ${calculating ? "busy" : ""}`}>
      {res ? (
        <>
          <div className="eq-head">
            <AnimatedNumber value={res.equity} decimals={2} className="eq-num" />
            <span className="eq-unit">%</span>
            <span className="eq-lbl">equity</span>
          </div>
          <div className="eq-bar">
            <motion.div className="eq-fill" style={{ background: color }}
              initial={{ width: 0 }} animate={{ width: `${Math.max(0, Math.min(100, res.equity))}%` }}
              transition={softSpring} />
          </div>
          <div className="eq-sub">
            <span>{dbl ? "scoop" : "win"} <b><AnimatedNumber value={res.win} decimals={1} />%</b></span>
            <span>tie <b><AnimatedNumber value={res.tie} decimals={1} />%</b></span>
            {res.ci95 > 0 && <span className="dim">±{res.ci95.toFixed(2)}</span>}
          </div>
          <button type="button" className="ht-mini" onClick={onHandTypes} title="Hand type distribution — click for details">
            {res.handType.map((v, i) => (
              <motion.span key={i} style={{ background: CATEGORY_COLORS[i] }}
                initial={false} animate={{ width: `${v}%` }} transition={softSpring}
                title={`${categories[i]} ${v.toFixed(1)}%`} />
            ))}
          </button>
        </>
      ) : (
        <div className="results-empty">
          <div className="eq-bar skeleton"><span /></div>
          <span>{calculating ? "Calculating…" : "No result yet"}</span>
        </div>
      )}
    </div>
  );
}

export { PLAYER_COLORS };
