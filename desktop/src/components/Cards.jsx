/* Playing cards, card slots and the deck.
 *
 * Every live card carries layoutId "card-<id>", shared between the deck
 * and whatever slot holds it, so dealing a card makes it fly from the
 * deck into place (and back again when it is removed). The highlight
 * ring of the active slot is one shared element too, so it glides from
 * slot to slot as you deal. */

import { memo } from "react";
import { motion } from "motion/react";
import { cardLabel, idRank, idSuit, SUIT_SYM } from "../lib/cards.js";
import { spring } from "./ui.jsx";

const rankText = (id) => (idRank(id) === "T" ? "10" : idRank(id));

export const PlayingCard = memo(function PlayingCard({ id, size = "sm", layoutId, className = "", ...rest }) {
  const s = idSuit(id);
  return (
    <motion.div
      layoutId={layoutId}
      className={`card card-${size} s-${s} ${className}`}
      transition={spring}
      aria-label={cardLabel(id)}
      {...rest}
    >
      <span className="c-rank">{rankText(id)}</span>
      <span className="c-suit">{SUIT_SYM[s]}</span>
      <span className="c-pip">{SUIT_SYM[s]}</span>
    </motion.div>
  );
});

/* a card that flips in when it mounts (trainer hands, tool results) */
export function DealtCard({ id, size = "lg", delay = 0 }) {
  return (
    <PlayingCard
      id={id}
      size={size}
      initial={{ rotateY: -90, y: -14, opacity: 0 }}
      animate={{ rotateY: 0, y: 0, opacity: 1 }}
      transition={{ ...spring, delay }}
    />
  );
}

export function CardSlot({ card, active, onFocus, onClear, size = "sm", label, placeholder }) {
  return (
    <div
      className={`slot slot-${size} ${active ? "active" : ""} ${card != null ? "filled" : ""}`}
      onClick={onFocus}
      onContextMenu={(e) => { e.preventDefault(); if (card != null) onClear(); }}
      role="button"
      tabIndex={-1}
      aria-label={label + (card != null ? `: ${cardLabel(card)}` : ": empty")}
      title={card != null ? "Click to select · right-click to remove" : "Click to select this slot"}
    >
      {active && <motion.div layoutId="active-ring" className="slot-ring" transition={spring} />}
      {card == null && placeholder && <span className="slot-ph">{placeholder}</span>}
      {card != null && <PlayingCard id={card} size={size} layoutId={`card-${card}`} />}
    </div>
  );
}

export function CardRow({ cards, zone, p, active, dispatch, size = "sm", label, placeholders }) {
  const isActive = (i) => active && active.zone === zone && active.i === i && (zone !== "p" || active.p === p);
  return (
    <div className="card-row">
      {cards.map((c, i) => (
        <CardSlot
          key={i}
          card={c}
          size={size}
          label={`${label} card ${i + 1}`}
          placeholder={placeholders && placeholders[i]}
          active={isActive(i)}
          onFocus={() => dispatch({ type: "focus", slot: { zone, p, i } })}
          onClear={() => dispatch({ type: "clearSlot", slot: { zone, p, i } })}
        />
      ))}
    </div>
  );
}

const DECK_ROWS = [3, 2, 1, 0]; // spades, hearts, diamonds, clubs

export const Deck = memo(function Deck({ used, onPick }) {
  return (
    <div className="deck" role="grid" aria-label="Deck">
      {DECK_ROWS.map((s) => (
        <div className="deck-row" role="row" key={s}>
          {Array.from({ length: 13 }, (_, col) => {
            const id = (12 - col) * 4 + s;
            const isUsed = used.has(id);
            return (
              <div className="deck-cell" key={id} role="gridcell">
                {isUsed ? (
                  <button
                    type="button"
                    className={`card card-xs ghost s-${idSuit(id)}`}
                    onClick={() => onPick(id)}
                    title={`${cardLabel(id)} — in use, click to return it`}
                    aria-label={`${cardLabel(id)}, in use`}
                  >
                    <span className="c-rank">{rankText(id)}</span>
                    <span className="c-suit">{SUIT_SYM[idSuit(id)]}</span>
                  </button>
                ) : (
                  <PlayingCard
                    id={id}
                    size="xs"
                    layoutId={`card-${id}`}
                    className="pickable"
                    role="button"
                    tabIndex={0}
                    onClick={() => onPick(id)}
                    onKeyDown={(e) => { if (e.key === " ") { e.preventDefault(); onPick(id); } }}
                    whileHover={{ y: -4, transition: { duration: 0.15 } }}
                    whileTap={{ scale: 0.92 }}
                  />
                )}
              </div>
            );
          })}
        </div>
      ))}
    </div>
  );
});

/* compact inline hand, e.g. tool results */
export function HandChips({ hand, ids, animateIn = true }) {
  const list = ids || [];
  return (
    <span className="hand-chips">
      {list.map((id, i) => (
        <motion.span
          key={`${hand || ""}-${id}`}
          className={`chip-card s-${idSuit(id)}`}
          initial={animateIn ? { opacity: 0, y: -6, rotateX: 60 } : false}
          animate={{ opacity: 1, y: 0, rotateX: 0 }}
          transition={{ ...spring, delay: i * 0.035 }}
        >
          {rankText(id)}
          {SUIT_SYM[idSuit(id)]}
        </motion.span>
      ))}
    </span>
  );
}
