# AuroraOS Native Desktop — Design

**Goal:** Replace the Firefox-kiosk web shell with a fully custom, native Aurora
desktop environment — Aurora's own DE, not an off-the-shelf one (no XFCE/GNOME),
not a browser. Reproduce the "daybreak" identity from `shell/index.html` as native
GTK3 surfaces.

## Architecture

Two layers:

1. **Compositor (window-management plumbing):** `labwc` (wlroots-based, already in
   extras). Software `pixman` renderer — no GPU/LLVM needed, ideal for the VM and
   commodity hardware. Provides move/resize/stack/focus for application windows.
   Configured + themed, but not user-visible as "labwc". Analogous to Mutter under
   GNOME Shell or Phoc under Phosh.

2. **Aurora Shell (100% custom, the visible desktop):** native C + GTK3 +
   `gtk-layer-shell`. Every visible desktop element is our code:
   - **Top bar** (layer-shell, anchored top): Aurora arc logo, workspace/clock
     center, right-side indicators (network, volume, brightness, battery, Aura
     button).
   - **Dock** (layer-shell, anchored bottom, auto-centered): pinned apps + running
     apps; click launches / focuses.
   - **App launcher** (centered popup): grid of installed apps parsed from
     `.desktop` files in the usual XDG dirs; type-to-filter.
   - **Wallpaper** (layer-shell, background layer): the aurora-horizon gradient
     (dawn palette; static gradient for v1, animated ribbons a later polish).
   - **Aura panel** (right-side slide-over): text entry → `aurorad` `/ask` →
     on-device LLM (llama.cpp) → response + executes returned UI/system actions.
   - **Theme:** `style.css` porting the daybreak tokens from `index.html`
     (aurora-spectrum accent, rose-gold horizon, Noto Sans, light surfaces).

## Components / files

- `shell/aurora-desktop/aurora-shell.c` — the shell (multiple layer-shell surfaces
  + launcher/Aura popups).
- `shell/aurora-desktop/style.css` — daybreak theme.
- `shell/aurora-desktop/aura-client.*` — tiny HTTP client to `aurorad` (raw socket,
  no libsoup dependency).
- `scripts/13-aurora-desktop.sh` — builds `gtk-layer-shell`, compiles + installs the
  shell, installs labwc config/theme/autostart, sets up the session + autologin.
- Reuses: `aurorad.py` (system bridge + `/ask`), `aura_llm.py`, `aura-tools.json`,
  llama.cpp `llama-server`, the bundled GGUF model — all already built by script 10's
  Aura section (that section is kept; only the Firefox/GTK-web-shell parts are dropped).

## Dependencies

New: `gtk-layer-shell` (small meson lib; needs gtk3 + wayland + wayland-protocols,
all present). Existing/reused: gtk3, wayland, wayland-protocols, wlroots, labwc,
seatd, libinput, libxkbcommon, foot (terminal), llama.cpp. **Dropped:** Firefox,
the whole `-D gallium` LLVM concern (compositor uses pixman; GTK3 uses its cairo
software path via `GSK`/cairo — no OpenGL required).

## Session / boot

- Auto-login the `aurora` user on tty1 → `aurora-session` starts `labwc`.
- labwc `autostart` launches: `aurora-shell` (the custom shell), `aurorad`,
  `aura-llm` (llama-server), and sets the background.
- Default systemd target stays `graphical`/multi-user with autologin getty override.

## Build increments (each ends in a bootable ISO to review)

1. **Foundation:** labwc + gtk-layer-shell + a minimal `aurora-shell` = top bar
   (logo + clock) + wallpaper. Boots to a custom Aurora desktop.
2. **Dock + launcher:** pinned/running apps, `.desktop` app grid, launch foot.
3. **Aura panel:** native assistant wired to aurorad/llama.
4. **Indicators + polish:** network/volume/brightness/battery, theming pass.

## Non-goals (v1)

- Writing a compositor from scratch (labwc suffices; revisit if a bespoke
  compositor is wanted later).
- Multi-monitor layout logic, animation-heavy effects, a settings app (later).
