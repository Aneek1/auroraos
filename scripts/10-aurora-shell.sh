#!/bin/bash
# AuroraOS 10 — the desktop: Wayland kiosk stack + Firefox + web shell + aurorad.
# Run INSIDE the chroot. This is the "BLFS express" phase — recipes are generic
# meson/autotools; on failure consult BLFS for the specific package.
set -e
STAMPS=/var/lib/aurora-build
cd /sources/extras

xt(){ SRCDIR=$(tar tf "$1" | head -1 | cut -d/ -f1); rm -rf "$SRCDIR"; tar xf "$1"; cd "$SRCDIR"; }
fin(){ cd /sources/extras; rm -rf "$SRCDIR"; }
mbuild(){ # meson-based
  local n=$1 opts=$2 tb
  [ -f $STAMPS/x-$n ] && return 0
  tb=$(ls ${n}-*.tar.* 2>/dev/null | head -1) || true
  [ -n "$tb" ] || tb=$(ls ${n}* | head -1)
  echo "==== extras: $n (meson) ===="
  xt "$tb"; mkdir -p build; cd build
  meson setup --prefix=/usr --buildtype=release $opts ..
  ninja; ninja install; cd ..; fin; touch $STAMPS/x-$n
}
abuild(){ # autotools-based
  local n=$1 opts=$2 tb
  [ -f $STAMPS/x-$n ] && return 0
  tb=$(ls ${n}-*.tar.* ${n}src*.tar.* 2>/dev/null | head -1)
  echo "==== extras: $n (autotools) ===="
  xt "$tb"
  ./configure --prefix=/usr --disable-static $opts
  make; make install; fin; touch $STAMPS/x-$n
}

# ---------- 1) Firefox GTK3 runtime deps ----------
abuild libffi
abuild libpng
abuild freetype "--enable-freetype-config --without-harfbuzz"
abuild fontconfig "--sysconfdir=/etc --localstatedir=/var --disable-docs"
mbuild pcre2 "-D pcre2grep-libz=false"
mbuild glib "-D introspection=disabled -D man-pages=disabled -D tests=false"
abuild jpegsrc "" || true   # libjpeg (jpegsrc.v9f)
[ -f $STAMPS/x-jpegsrc ] || { xt jpegsrc*.tar.gz; ./configure --prefix=/usr; make; make install; fin; touch $STAMPS/x-jpegsrc; }
mbuild cairo
mbuild harfbuzz "-D glib=enabled -D freetype=enabled -D tests=disabled -D docs=disabled"
mbuild pango "-D introspection=disabled"
mbuild gdk-pixbuf "-D introspection=disabled -D man=false"
mbuild atk "-D introspection=disabled" 2>/dev/null || mbuild atk ""
mbuild at-spi2-core "-D introspection=disabled -D systemd_user_dir=/usr/lib/systemd/user"
abuild dbus "--sysconfdir=/etc --localstatedir=/var --runstatedir=/run --with-systemduserunitdir=no --with-systemdsystemunitdir=no" 2>/dev/null || true
mbuild gtk "-D introspection=false -D demos=false -D examples=false -D tests=false -D man-pages=false -D media-gstreamer=disabled -D print-backends=none"

# ---------- 2) Wayland kiosk stack ----------
mbuild wayland "-D documentation=false -D tests=false"
mbuild wayland-protocols "-D tests=false"
abuild libdrm "" 2>/dev/null || mbuild libdrm "-D tests=false"
abuild pixman "" 2>/dev/null || mbuild pixman "-D demos=disabled -D tests=disabled"
abuild libepoxy "" 2>/dev/null || mbuild libepoxy ""
mbuild mesa "-D platforms=wayland -D glx=disabled -D vulkan-drivers= -D gallium-drivers=swrast,iris,radeonsi,virgl -D video-codecs="
abuild libevdev "--disable-static" 2>/dev/null || mbuild libevdev "-D tests=disabled -D documentation=disabled"
mbuild libinput "-D debug-gui=false -D tests=false -D documentation=false -D libwacom=false"
mbuild libxkbcommon "-D enable-docs=false -D enable-x11=false"
mbuild seatd 2>/dev/null || { [ -f $STAMPS/x-seatd ] || { xt 0.9.1.tar.gz; mkdir -p build; cd build; meson setup --prefix=/usr ..; ninja; ninja install; cd ..; fin; touch $STAMPS/x-seatd; }; }
mbuild libdisplay-info ""   # hard dep of wlroots 0.18 — build it FIRST (proven in the arm64 compile proof)
mbuild wlroots "-D examples=false -D xwayland=disabled -D backends=drm,libinput -D renderers=gles2"
mbuild cage "-D man-pages=disabled" 2>/dev/null || mbuild cage ""

# fonts
if [ ! -f $STAMPS/x-fonts ]; then
  mkdir -p /usr/share/fonts/noto
  # extract dir name varies by host repo (notofonts archive) — don't cd into it
  tar xf NotoSans*.tar.gz 2>/dev/null && \
    { find ./*NotoSans* -name '*.ttf' -exec cp {} /usr/share/fonts/noto/ \; ; rm -rf ./*NotoSans*/; } || true
  fc-cache -f || true
  touch $STAMPS/x-fonts
fi

# ---------- 3) Firefox (prebuilt binary) ----------
if [ ! -f $STAMPS/x-firefox ]; then
  echo "==== extras: firefox (binary) ===="
  tar xf firefox-*.tar.* -C /opt
  ln -sfv /opt/firefox/firefox /usr/bin/firefox
  touch $STAMPS/x-firefox
fi

# ---------- 4) AuroraOS shell + aurorad ----------
install -d /usr/share/aurora/shell /usr/lib/aurora
install -m644 /aurora/shell/index.html        /usr/share/aurora/shell/
install -m644 /aurora/shell/aurora-bridge.js  /usr/share/aurora/shell/
install -m755 /aurora/shell/aurorad.py        /usr/lib/aurora/aurorad
install -m755 /aurora/shell/aurora-session    /usr/bin/aurora-session

install -m644 /aurora/systemd/aurorad.service      /usr/lib/systemd/system/
install -m644 /aurora/systemd/aurora-shell.service /usr/lib/systemd/system/
install -m644 /aurora/systemd/seatd.service        /usr/lib/systemd/system/ 2>/dev/null || true

systemctl enable seatd aurorad aurora-shell
systemctl set-default graphical.target

# firefox kiosk profile (no first-run, dark, local file access)
install -d /var/lib/aurora/.mozilla/firefox/kiosk.default
cat > /var/lib/aurora/.mozilla/firefox/profiles.ini <<"EOF"
[Profile0]
Name=kiosk
IsRelative=1
Path=kiosk.default
Default=1
EOF
cat > /var/lib/aurora/.mozilla/firefox/kiosk.default/user.js <<"EOF"
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("datareporting.policy.firstRunURL", "");
user_pref("toolkit.telemetry.enabled", false);
user_pref("ui.systemUsesDarkTheme", 1);
user_pref("dom.security.https_only_mode", false);
user_pref("browser.tabs.inTitlebar", 1);
EOF
chown -R aurora:aurora /var/lib/aurora

echo "== 10 complete — exit chroot; optionally run 11-make-iso.sh, or reboot into AuroraOS =="
