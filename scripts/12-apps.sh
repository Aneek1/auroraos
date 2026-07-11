#!/bin/bash
# AuroraOS 12 — external applications: labwc compositor + foot terminal +
# AppImage (fuse3) + Nix first-boot helper + real launcher session.
# Run INSIDE the chroot, AFTER 10-aurora-shell.sh. See
# docs/superpowers/specs/2026-07-10-external-apps-design.md
set -e
STAMPS=/var/lib/aurora-build
cd /sources/extras

# same build helpers as script 10
xt(){ SRCDIR=$(tar tf "$1" | head -1 | cut -d/ -f1); rm -rf "$SRCDIR"; tar xf "$1"; cd "$SRCDIR"; }
fin(){ cd /sources/extras; rm -rf "$SRCDIR"; }
mbuild(){ # meson: mbuild <name> "<opts>"
  local n=$1 opts=$2 tb
  [ -f $STAMPS/x-$n ] && { echo "-- $n already built"; return 0; }
  tb=$(ls ${n}-*.tar.* 2>/dev/null | head -1); [ -n "$tb" ] || tb=$(ls ${n}*.tar.* | head -1)
  echo "==== app-stack: $n (meson) ===="
  xt "$tb"; mkdir -p build; cd build
  meson setup --prefix=/usr --buildtype=release $opts ..
  ninja; ninja install; cd ..; fin; touch $STAMPS/x-$n
}

# ---------- 1) terminal: tllist -> fcft -> foot ----------
mbuild tllist ""
mbuild fcft "-D run-shaping=disabled -D test-text-shaping=false"
mbuild foot "-D grapheme-clustering=disabled -D docs=disabled -D themes=false"

# ---------- 2) AppImage runtime (fuse3) ----------
# tarball is fuse-3.x (not fuse3-); non-critical for the shell, so don't let it block labwc
mbuild fuse "-D examples=false -D tests=false -D useroot=false" || { cd /sources/extras; echo "!! fuse (AppImage) skipped — non-critical"; }

# ---------- 3) compositor: labwc (wlroots already built in script 10) ----------
mbuild labwc "-D man-pages=disabled -D icon=disabled"   # icon=disabled drops the libsfdo (git-only) dep

# ---------- 4) labwc config for the 'aurora' user ----------
CFG=/var/lib/aurora/.config/labwc
install -d -o aurora -g aurora "$CFG" /var/lib/aurora/Apps

cat > "$CFG/environment" <<"EOF"
MOZ_ENABLE_WAYLAND=1
XDG_CURRENT_DESKTOP=labwc
EOF

# rc.xml: maximize + de-decorate the shell so native windows float above it
cat > "$CFG/rc.xml" <<"EOF"
<?xml version="1.0"?>
<labwc_config>
  <core><gap>0</gap></core>
  <theme><cornerRadius>8</cornerRadius></theme>
  <keyboard>
    <keybind key="W-Return"><action name="Execute" command="foot"/></keybind>
    <keybind key="W-q"><action name="Close"/></keybind>
    <keybind key="A-Tab"><action name="NextWindow"/></keybind>
    <keybind key="W-d"><action name="ToggleShowDesktop"/></keybind>
  </keyboard>
  <windowRules>
    <!-- the Aurora shell: fullscreen base layer, no decorations -->
    <windowRule identifier="firefox" serverDecoration="no">
      <action name="Maximize"/>
    </windowRule>
  </windowRules>
</labwc_config>
EOF

# autostart: launch the shell fullscreen with no browser chrome, so it looks/feels
# like the OS desktop (labwc still floats native app windows on top of it).
cat > "$CFG/autostart" <<"EOF"
firefox --kiosk "file:///usr/share/aurora/shell/index.html" &
EOF
chown -R aurora:aurora /var/lib/aurora/.config /var/lib/aurora/Apps

# ---------- 5) Nix first-boot helper (needs network; can't run in chroot) ----------
cat > /usr/bin/aurora-get-nix <<"EOF"
#!/bin/sh
# Install the Nix package manager (single-user) for app installs. Needs network.
set -e
echo "Installing Nix (single-user)…"
sh <(curl -L https://nixos.org/nix/install) --no-daemon
echo
echo "Done. Start a new terminal, then install apps with e.g.:"
echo "   nix profile install nixpkgs#neovim"
echo "   nix profile install nixpkgs#librewolf"
echo "They appear in the Aurora launcher automatically (via ~/.nix-profile)."
EOF
chmod 755 /usr/bin/aurora-get-nix

echo "== 12 complete — session now uses labwc; run 11-make-iso.sh to (re)bake =="
