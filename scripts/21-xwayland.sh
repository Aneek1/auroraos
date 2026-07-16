#!/bin/bash
# DaybreakOS 21 — enable XWayland so X11-only apps (Steam and its games) run.
# Run INSIDE the chroot, AFTER 13-aurora-desktop.sh.
#
# Why: 13 built wlroots + labwc with `-D xwayland=disabled`, so there is no live
# X display. Steam (X11-only) then dies with:
#   "must provide the DISPLAY environment variable to the D-Bus session bus
#    activation environment".
# Xwayland the binary is already in the image and runs; what was missing is
#   (a) wlroots' xwayland backend (needs the xcb-icccm lib — from xcb-util-wm),
#   (b) labwc built against that backend,
#   (c) DISPLAY wired into the session + the D-Bus activation environment.
#
# wlroots 0.18 xwayland requires: xcb, xcb-composite, xcb-render, xcb-res,
# xcb-xfixes (all already present) + xcb-icccm (MISSING). xcb-errors is optional
# (nicer X error strings) and built best-effort.
set -e
STAMPS=/var/lib/aurora-build
cd /sources/extras

xt(){ SRCDIR=$(tar tf "$1" | sed 's|^\./||' | awk 'NF' | head -1 | cut -d/ -f1); rm -rf "$SRCDIR"; tar xf "$1"; cd "$SRCDIR"; }
fin(){ cd /sources/extras; rm -rf "$SRCDIR"; }

fetch(){ # fetch $1(url) -> local tarball if not already present
  local url=$1 f; f=$(basename "$url")
  [ -f "$f" ] && { echo "have $f"; return 0; }
  echo "downloading $f"
  wget -q --no-check-certificate -O "$f" "$url" || { echo "!! download failed: $url"; return 1; }
}

# ---------- 1) xcb-util-wm (provides xcb-icccm + xcb-ewmh) ----------
if [ ! -f /usr/lib/pkgconfig/xcb-icccm.pc ]; then
  fetch https://xcb.freedesktop.org/dist/xcb-util-wm-0.4.2.tar.xz
  xt xcb-util-wm-0.4.2.tar.xz
  ./configure --prefix=/usr --disable-static
  make; make install
  fin
fi

# ---------- 2) xcb-util-errors (optional — nicer X error decoding) ----------
if [ ! -f /usr/lib/pkgconfig/xcb-errors.pc ]; then
  ( set -e
    fetch https://xcb.freedesktop.org/dist/xcb-util-errors-1.0.1.tar.xz
    xt xcb-util-errors-1.0.1.tar.xz
    ./configure --prefix=/usr --disable-static
    make; make install
    fin
  ) || echo "WARN: xcb-util-errors skipped (optional)"
fi

# sanity: xcb-icccm must now be discoverable, else wlroots xwayland won't enable
pkg-config --exists xcb-icccm || { echo "!! xcb-icccm still missing — aborting"; exit 1; }

# ---------- 3) rebuild wlroots WITH xwayland ----------
echo "==== rebuild wlroots (xwayland=enabled) ===="
rm -f "$STAMPS/x-wlroots"
xt "$(ls wlroots-*.tar.* | head -1)"
mkdir -p build; cd build
meson setup --prefix=/usr --buildtype=release --reconfigure \
  -D examples=false -D xwayland=enabled -D backends=drm,libinput \
  -D renderers= -D allocators=auto .. \
  || meson setup --prefix=/usr --buildtype=release \
       -D examples=false -D xwayland=enabled -D backends=drm,libinput \
       -D renderers= -D allocators=auto ..
ninja; ninja install
cd ..; fin
touch "$STAMPS/x-wlroots"
grep -q 'have_xwayland=true' /usr/lib/pkgconfig/wlroots-0.18.pc \
  || { echo "!! wlroots still has_xwayland=false — aborting"; exit 1; }

# ---------- 4) rebuild labwc WITH xwayland ----------
echo "==== rebuild labwc (xwayland=enabled) ===="
rm -f "$STAMPS/x-labwc"
xt "$(ls labwc-*.tar.* 2>/dev/null | head -1)"
mkdir -p build; cd build
meson setup --prefix=/usr --buildtype=release --reconfigure -D xwayland=enabled .. \
  || meson setup --prefix=/usr --buildtype=release -D xwayland=enabled ..
ninja; ninja install
cd ..; fin
touch "$STAMPS/x-labwc"

# ---------- 5) DISPLAY wiring ----------
# CRITICAL: do NOT export DISPLAY in aurora-session (the environment labwc is
# launched from). wlroots' wlr_backend_autocreate() treats a set DISPLAY as
# "nest inside this X server" and selects the X11 backend — which we do NOT
# compile (backends=drm,libinput). wlr_backend_autocreate() then returns NULL
# and labwc aborts at startup → black screen. labwc (xwayland enabled) starts
# its OWN Xwayland and assigns DISPLAY itself; children inherit it.
# (defensive: strip any stale DISPLAY export a previous run may have added)
sed -i '/^export DISPLAY=/d' /usr/bin/aurora-session

# labwc autostart: once Xwayland is up, push the compositor env (incl. DISPLAY)
# into the D-Bus session activation environment so portal/steam D-Bus activation
# sees it. Small delay lets lazy Xwayland assign DISPLAY first.
if ! grep -q 'dbus-update-activation-environment' /etc/xdg/labwc/autostart; then
  sed -i '1a ( sleep 2; dbus-update-activation-environment --all 2>/dev/null || dbus-update-activation-environment DISPLAY WAYLAND_DISPLAY XDG_CURRENT_DESKTOP 2>/dev/null ) &' \
    /etc/xdg/labwc/autostart
fi

# Give X11 apps launched BY the shell (Steam) a DISPLAY. The shell itself stays
# Wayland (GDK_BACKEND=wayland from aurora-session), but children like Steam
# inherit DISPLAY=:0 — the display wlroots' lazy Xwayland reserves first. This
# is safe here (unlike exporting into labwc's own env, which breaks backend
# autodetect); the shell is a child of labwc, not labwc itself.
if ! grep -q 'env DISPLAY=:0 /usr/bin/aurora-shell' /etc/xdg/labwc/autostart; then
  sed -i 's#^/usr/bin/aurora-shell &#env DISPLAY=:0 /usr/bin/aurora-shell \&#' /etc/xdg/labwc/autostart
fi

echo "== 21 complete — XWayland enabled. Re-squash + rebuild ISO (or live-patch). =="
