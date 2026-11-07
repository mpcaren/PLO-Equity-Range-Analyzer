# PLO5 Equity Lab — native app (Rust + Slint)

The same app as `../desktop`, rebuilt as a fully native Rust program: the
Rust PLO5 engine (`../engine`) plus an interface written in
[Slint](https://slint.dev). There is no browser engine, no HTML and no
JavaScript; the interface is drawn with the GPU (OpenGL), with a software
renderer as a fallback.

Everything from the web-based app is here: the equity calculator (hand,
range and random players, street filters, double board, dead cards, hand
finder, hand-type breakdown), the equity and stack-off trainers, settings,
and the in-app rank-table build. Results are identical, since both apps use
the same engine.

## Animations

Rebuilt natively rather than borrowed from a web library:

- **Cards fly** between the deck and their slots (dealing, removing,
  random boards, pasted and found hands) along a shallow arc with a little
  overshoot, growing or shrinking to the slot size. Slots and the deck
  report their window positions, and `src/anim.rs` drives the flight frame
  by frame on an overlay layer.
- **The selection ring glides** from slot to slot as you deal.
- **Sliding pills** in every segmented control and the sidebar, springy
  switches, chevrons that rotate.
- **Numbers count** to new values; equity bars, hand-type bars and the
  pot-share donut spring to their new sizes.
- Players, panels, toasts and the hand-type popup **fade and grow in**;
  removed players fade out and collapse; the Calculate button rolls over
  to Cancel with a live progress bar.
- Trainer hands **deal in with a flip**, the answer bar fills and the
  grade pops.

Settings → Animations → Reduced turns the motion off (fades stay).

## Run it

You need [Rust](https://rustup.rs). From the repository root:

```bash
cargo run --release -p plo5-native
```

On Linux you also need the usual desktop libraries (most desktops have
them): `libxkbcommon-x11`, `libfontconfig`, and OpenGL. If the window stays
blank on an old GPU or a remote desktop, use the software renderer:

```bash
SLINT_BACKEND=winit-software cargo run --release -p plo5-native
```

On Windows the same command builds `plo5-equity-lab-native.exe` in
`target\release\`. It is a single self-contained file (fonts are
embedded) that you can copy anywhere.

### The preflop rank table

Preflop percentiles and preflop ranges need `plo5rank.bin` (the same file
the C tools and the other apps use). A development build uses the one in
the repository root if `plo5setup` already made it. Otherwise open
**Settings → Build now** (a few minutes on a modern CPU, with progress and
cancel), or **Use an existing file…** to import one. Release builds keep
their copy, plus settings and your last setup, in the per-user data folder
(`%APPDATA%\PLO5 Equity Lab` on Windows, `~/.local/share/PLO5 Equity Lab`
on Linux, `~/Library/Application Support/PLO5 Equity Lab` on macOS).

## Using it

Same as the other app: click the deck or type cards (`A` then `h` = A♥),
paste a hand with Ctrl+V, right-click or Backspace to remove a card, Enter
to calculate, Esc to cancel, Ctrl+1…4 to switch views.

## Layout

```
src/main.rs         app state <-> UI sync, engine calls, keyboard, trainers
src/calc.rs         calculator state (dealing order, validation, request specs)
src/engine_host.rs  the engine on its own thread: queue, progress, cancel
src/anim.rs         card flights between the deck and the slots
src/persist.rs      settings, last setup, trainer stats; file locations
ui/data.slint       data model, theme, motion settings
ui/cards.slint      cards, slots, deck      ui/widgets.slint  controls
ui/calculator.slint, trainer.slint, settings.slint, app.slint   the views
ui/fonts/           Inter (SIL Open Font License, see Inter-LICENSE.txt)
```

`cargo test -p plo5-native` runs the calculator-state tests.

## Licensing note

Slint is available under GPLv3, a royalty-free licence for desktop apps,
or a commercial licence. This app uses the royalty-free licence, whose
condition is attribution: the "Made with Slint" badge in Settings → About.
Keep it there if you distribute the app. The PLO5 code itself stays MIT.
