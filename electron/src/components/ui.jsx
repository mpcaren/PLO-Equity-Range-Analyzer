/* Shared UI primitives. Motion presets live here so the whole app moves
 * with one consistent feel. */

import { useEffect, useId, useRef } from "react";
import { AnimatePresence, animate, motion, useMotionValue, useTransform } from "motion/react";
import { IconX } from "./Icons.jsx";

export const spring = { type: "spring", stiffness: 520, damping: 38, mass: 0.9 };
export const softSpring = { type: "spring", stiffness: 260, damping: 30 };
export const ease = [0.22, 1, 0.36, 1];

/* segmented control with a pill that glides between options */
export function Segmented({ value, options, onChange, size = "md", className = "", layoutKey }) {
  const auto = useId();
  const id = layoutKey || auto;
  return (
    <div className={`seg seg-${size} ${className}`} role="radiogroup">
      {options.map((o) => {
        const on = o.value === value;
        return (
          <button
            key={String(o.value)}
            type="button"
            role="radio"
            aria-checked={on}
            className={on ? "on" : ""}
            onClick={() => onChange(o.value)}
            title={o.title}
          >
            {on && <motion.span layoutId={`seg-${id}`} className="seg-pill" transition={spring} />}
            <span className="seg-label">{o.label}</span>
            {o.sub && <span className="seg-sub">{o.sub}</span>}
          </button>
        );
      })}
    </div>
  );
}

export function Switch({ checked, onChange, label, hint }) {
  return (
    <label className="switch-row">
      <span className="switch-text">
        <span>{label}</span>
        {hint && <small>{hint}</small>}
      </span>
      <button
        type="button"
        role="switch"
        aria-checked={checked}
        className={`switch ${checked ? "on" : ""}`}
        onClick={() => onChange(!checked)}
      >
        <motion.span className="knob" layout transition={spring} />
      </button>
    </label>
  );
}

/* number that tweens to its new value */
export function AnimatedNumber({ value, decimals = 2, suffix = "", className = "" }) {
  const mv = useMotionValue(value ?? 0);
  const text = useTransform(mv, (v) => v.toFixed(decimals) + suffix);
  useEffect(() => {
    if (value == null) return;
    const c = animate(mv, value, { duration: 0.7, ease });
    return () => c.stop();
  }, [value, mv]);
  return <motion.span className={className}>{text}</motion.span>;
}

export function Panel({ title, actions, children, className = "", ...rest }) {
  return (
    <motion.section className={`panel ${className}`} {...rest}>
      {(title || actions) && (
        <header className="panel-head">
          {title && <h2>{title}</h2>}
          <div className="panel-actions">{actions}</div>
        </header>
      )}
      {children}
    </motion.section>
  );
}

export function IconButton({ icon: Icon, label, onClick, disabled, className = "", size = 16, ...rest }) {
  return (
    <motion.button
      type="button"
      className={`icon-btn ${className}`}
      onClick={onClick}
      disabled={disabled}
      title={label}
      aria-label={label}
      whileTap={disabled ? undefined : { scale: 0.88 }}
      {...rest}
    >
      <Icon size={size} />
    </motion.button>
  );
}

export function Button({ children, variant = "ghost", icon: Icon, className = "", ...rest }) {
  return (
    <motion.button
      type="button"
      className={`btn btn-${variant} ${className}`}
      whileTap={rest.disabled ? undefined : { scale: 0.96 }}
      {...rest}
    >
      {Icon && <Icon size={15} />}
      {children}
    </motion.button>
  );
}

export const Kbd = ({ children }) => <kbd className="kbd">{children}</kbd>;

export function Spinner({ size = 14 }) {
  return <span className="spinner" style={{ width: size, height: size }} />;
}

export function ProgressRing({ value, size = 44, stroke = 4 }) {
  const r = (size - stroke) / 2;
  return (
    <svg width={size} height={size} className="ring" viewBox={`0 0 ${size} ${size}`}>
      <circle cx={size / 2} cy={size / 2} r={r} className="ring-track" strokeWidth={stroke} fill="none" />
      <motion.circle
        cx={size / 2} cy={size / 2} r={r}
        className="ring-fill" strokeWidth={stroke} fill="none" strokeLinecap="round"
        initial={{ pathLength: 0 }}
        animate={{ pathLength: Math.max(0.001, Math.min(1, value)) }}
        transition={softSpring}
        style={{ rotate: -90, transformOrigin: "50% 50%" }}
      />
    </svg>
  );
}

export function Modal({ open, onClose, title, children, wide }) {
  const ref = useRef(null);
  useEffect(() => {
    if (!open) return;
    const onKey = (e) => { if (e.key === "Escape") { e.stopPropagation(); onClose(); } };
    window.addEventListener("keydown", onKey, true);
    ref.current && ref.current.focus();
    return () => window.removeEventListener("keydown", onKey, true);
  }, [open, onClose]);
  return (
    <AnimatePresence>
      {open && (
        <motion.div
          className="overlay"
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          exit={{ opacity: 0 }}
          transition={{ duration: 0.2 }}
          onMouseDown={(e) => { if (e.target === e.currentTarget) onClose(); }}
        >
          <motion.div
            ref={ref}
            tabIndex={-1}
            role="dialog"
            aria-modal="true"
            aria-label={title}
            className={`modal ${wide ? "wide" : ""}`}
            initial={{ opacity: 0, y: 24, scale: 0.96 }}
            animate={{ opacity: 1, y: 0, scale: 1 }}
            exit={{ opacity: 0, y: 12, scale: 0.98 }}
            transition={softSpring}
          >
            <header className="modal-head">
              <h3>{title}</h3>
              <IconButton icon={IconX} label="Close (Esc)" onClick={onClose} />
            </header>
            <div className="modal-body">{children}</div>
          </motion.div>
        </motion.div>
      )}
    </AnimatePresence>
  );
}

export function Toasts({ items }) {
  return (
    <div className="toasts" aria-live="polite">
      <AnimatePresence initial={false}>
        {items.map((t) => (
          <motion.div
            key={t.id}
            layout
            className={`toast toast-${t.kind}`}
            initial={{ opacity: 0, y: 16, scale: 0.95 }}
            animate={{ opacity: 1, y: 0, scale: 1 }}
            exit={{ opacity: 0, scale: 0.95, transition: { duration: 0.15 } }}
            transition={spring}
          >
            {t.text}
          </motion.div>
        ))}
      </AnimatePresence>
    </div>
  );
}

/* number input that clamps to 0..100 and commits on every valid keystroke */
export function PctInput({ value, onChange, min = 0, max = 100, step = 1, className = "", ...rest }) {
  return (
    <input
      type="number"
      className={`num ${className}`}
      min={min}
      max={max}
      step={step}
      value={value}
      onChange={(e) => {
        const v = e.target.value === "" ? min : Number(e.target.value);
        if (!Number.isNaN(v)) onChange(Math.max(min, Math.min(max, v)));
      }}
      {...rest}
    />
  );
}
