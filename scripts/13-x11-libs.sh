#!/bin/bash
# AuroraOS 13 — X11 client libraries + alsa-lib. The prebuilt Firefox binary links
# libX11/libxcb/libXext/… and libasound even in Wayland mode, so they must exist at
# runtime or Firefox fails to load XPCOM. Run INSIDE the chroot, AFTER 10 (and 12).
set -e
STAMPS=/var/lib/aurora-build
cd /sources/extras

xt(){ SRCDIR=$(tar tf "$1" | head -1 | cut -d/ -f1); rm -rf "$SRCDIR"; tar xf "$1"; cd "$SRCDIR"; }
fin(){ cd /sources/extras; rm -rf "$SRCDIR"; }
# xb <name> "<extra configure opts>" — autotools X lib
xb(){ local n=$1 opts=$2 tb
  [ -f $STAMPS/x-$n ] && { echo "-- $n done"; return 0; }
  tb=$(ls ${n}-*.tar.* 2>/dev/null | head -1); [ -n "$tb" ] || { echo "!! no tarball for $n"; exit 1; }
  echo "==== x11: $n ===="
  xt "$tb"
  ./configure --prefix=/usr --sysconfdir=/etc --disable-static $opts
  make; make install; fin; touch $STAMPS/x-$n
}

# xorgproto — headers, meson-only
if [ ! -f $STAMPS/x-xorgproto ]; then
  echo "==== x11: xorgproto (meson) ===="
  xt "$(ls xorgproto-*.tar.* | head -1)"; mkdir -p build; cd build
  meson setup --prefix=/usr ..; ninja; ninja install; cd ..; fin; touch $STAMPS/x-xorgproto
fi

# build order (BLFS Xorg libraries)
xb xtrans ""
xb libXau ""
xb libXdmcp ""
xb xcb-proto ""
xb libxcb "--without-doc"
xb libX11 ""
xb libXext ""
xb libXrender ""
xb libXfixes ""
xb libXi ""
xb libXrandr ""
xb libXcursor ""
xb libXcomposite ""
xb libXdamage ""
xb libXinerama ""

# audio: alsa-lib (Firefox needs libasound.so.2)
xb alsa-lib "--without-debug"

# xkeyboard-config: keymap DATA in /usr/share/X11/xkb. Without it libxkbcommon can't
# compile a keymap and wlroots/labwc SEGV on the resulting NULL keymap (meson-only).
if [ ! -f $STAMPS/x-xkeyboard-config ]; then
  echo "==== x11: xkeyboard-config (meson) ===="
  xt "$(ls xkeyboard-config-*.tar.* | head -1)"; mkdir -p build; cd build
  meson setup --prefix=/usr ..; ninja; ninja install; cd ..; fin; touch $STAMPS/x-xkeyboard-config
fi

ldconfig
echo "== 13 complete — X11 client libs + alsa installed; re-bake with 11-make-iso.sh =="
