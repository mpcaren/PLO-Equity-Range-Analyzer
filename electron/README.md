# PLO5 Equity Lab — desktop app

An Electron desktop app for the PLO5 engine: equity calculator, range
analyzer, hand finder, and the equity and stack-off trainers, in one
window with smooth, physical-feeling animations (cards fly from the deck
into their slots, results count up, the board and players reflow
smoothly).

It runs the C engine as a child process (`plo5svc`) and talks to it over
stdin/stdout, so there is no network port and no firewall prompt.

## Run it

You need [Node.js](https://nodejs.org) 20 or newer and the engine binary.

1. Build the engine from the repository root:

   ```bash
   make plo5svc
   ```

   On Windows with MSVC, `build.bat` builds `plo5svc.exe` along with the
   other tools.

2. Install and start the app:

   ```bash
   cd electron
   npm install
   npm start
   ```

`npm run dev` starts it with hot reload instead (Vite dev server plus
Electron).

### The preflop rank table

Preflop percentiles and preflop ranges need `plo5rank.bin`, a one-time
table of every starting hand ranked by equity. If you already built it
with `plo5setup` (or `plo5calc --gen-ranks`) in the repository root, the
app uses it. Otherwise open **Settings → Build now**: it shows progress
and saves the table for next time (a few minutes on a modern desktop CPU).
Everything else works without it.

## Package an installer

```bash
npm run dist
```

This produces an installer in `electron/release/` for the platform you
run it on: Windows (NSIS installer and portable `.exe`), macOS (`.dmg`)
or Linux (AppImage). The engine binary from the repository root is bundled
into the app, so build `plo5svc` for the same platform first. An
installed app keeps its rank table in the per-user data folder; use
**Settings → Use an existing file…** to import one you already have.

## Using it

- **Deal cards** by clicking the deck, or type them: `A` then `h` deals
  A♥ to the highlighted slot, which then moves on automatically. Paste a
  hand with Ctrl+V (`AhKhQd Jc Tc`, `A♥ K♠ 10d …` all work). Right-click
  a card, or press Backspace, to remove it.
- **Players** can be a fixed hand, a percentile **range** (0 = weakest,
  100 = strongest, on the current street) with optional flop/turn street
  filters, or a **random** hand.
- **Run panel**: pick Fast / Balanced / Precise, toggle auto-calculate,
  and press Enter (or Calculate). Esc cancels a long run.
- **Double board** splits the pot between two boards; equity is the
  expected pot share and "scoop" is winning both.
- **Hand finder** looks up the hand at a percentile or with a target
  equity vs a random hand, and deals it to a player.
- Ctrl+1…4 switches between Equity, Trainer, Stack-off and Settings.

## Layout

```
main/main.cjs      window, engine lifecycle, IPC (validated endpoint list)
main/engine.cjs    plo5svc child process: request queue, progress, cancel, restart
main/preload.cjs   window.plo5 bridge (the page has no Node access)
src/state.jsx      app state: settings, calculator, runs, rank-table job
src/lib/           card helpers, calculator reducer, engine client
src/components/    cards + deck, player card, range slider, charts, UI kit
src/views/         Calculator, Trainer (equity + stack-off), Settings
test/              node --test suites (reducer + engine bridge against plo5svc)
```

`npm test` runs the tests; the engine suite is skipped if `plo5svc` has
not been built.
