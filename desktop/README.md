# PLO5 Equity Lab — desktop app

A [Tauri](https://tauri.app) desktop app over the Rust PLO5 engine
(`../engine`): equity calculator, range analyzer, hand finder, and the
equity and stack-off trainers, in one window with smooth, physical-feeling
animations (cards fly from the deck into their slots, results count up,
the board and players reflow smoothly).

The engine runs in-process on its own thread, so there is no helper
program, no network port and no firewall prompt. It is a port of the C
engine in `../src` that returns bit-identical results (see
`../engine/README.md`).

## Run it

You need [Rust](https://rustup.rs), [Node.js](https://nodejs.org) 20 or
newer, and the system libraries Tauri uses — see Tauri's
[prerequisites](https://tauri.app/start/prerequisites/). On Windows that
is the WebView2 runtime (already present on Windows 10/11) and the MSVC
build tools; on Debian/Ubuntu:

```bash
sudo apt install libwebkit2gtk-4.1-dev libgtk-3-dev librsvg2-dev libayatana-appindicator3-dev build-essential pkg-config
```

Then:

```bash
cd desktop
npm install
npm run dev
```

`npm run dev` starts the app with hot reload; the first build compiles the
Rust side and takes a few minutes.

### The preflop rank table

Preflop percentiles and preflop ranges need `plo5rank.bin`, a one-time
table of every starting hand ranked by equity (the same file the C tools
use). A development build uses the one in the repository root if
`plo5setup` already made it. Otherwise open **Settings → Build now**: it
shows progress, can be cancelled, and saves the table for next time (a few
minutes on a modern desktop CPU). Everything else works without it.

## Package an installer

```bash
npm run dist
```

This builds a release binary and the installers for the platform you run
it on, in `../target/release/bundle/`: MSI and NSIS setup on Windows,
`.app`/`.dmg` on macOS, `.deb`/`.rpm`/AppImage on Linux. An installed app
keeps its rank table in the per-user data folder; use **Settings → Use an
existing file…** to import one you already have.

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
src-tauri/src/lib.rs   engine thread, commands (engine_call / engine_cancel /
                       engine_info / ranks_import / app_reveal), window setup
src-tauri/tauri.conf.json, capabilities/   app config and permissions
src/state.jsx          app state: settings, calculator, runs, rank-table job
src/lib/               card helpers, calculator reducer, engine client
src/components/        cards + deck, player card, range slider, charts, UI kit
src/views/             Calculator, Trainer (equity + stack-off), Settings
test/                  node --test suite for the calculator reducer
```

`npm test` runs the UI tests; the engine's tests live with the engine
(`cargo test -p plo5 --release` from the repository root).
