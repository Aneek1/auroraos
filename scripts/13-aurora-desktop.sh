#!/bin/bash
# AuroraOS 13 — the NATIVE Aurora desktop (replaces the Firefox web-shell path).
# Wayland compositor (labwc, wlroots/pixman) + Aurora's own GTK3 shell + Aura LLM.
# Run INSIDE the chroot. Software rendering only (no GPU, no LLVM): mesa builds
# softpipe just to satisfy gbm/GLES for wlroots; the shell renders via GTK cairo.
set -e
STAMPS=/var/lib/aurora-build
mkdir -p "$STAMPS"
cd /sources/extras

xt(){ SRCDIR=$(tar tf "$1" | head -1 | cut -d/ -f1); rm -rf "$SRCDIR"; tar xf "$1"; cd "$SRCDIR"; }
fin(){ cd /sources/extras; rm -rf "$SRCDIR"; }
mbuild(){ # meson
  local n=$1 opts=$2 tb
  [ -f $STAMPS/x-$n ] && return 0
  tb=$(ls ${n}-*.tar.* 2>/dev/null | head -1) || true
  [ -n "$tb" ] || tb=$(ls ${n}* | head -1)
  echo "==== desktop: $n (meson) ===="
  xt "$tb"; mkdir -p build; cd build
  meson setup --prefix=/usr --buildtype=release $opts ..
  ninja; ninja install; cd ..; fin; touch $STAMPS/x-$n
}
abuild(){ # autotools
  local n=$1 opts=$2 tb
  [ -f $STAMPS/x-$n ] && return 0
  tb=$(ls ${n}-*.tar.* ${n}src*.tar.* 2>/dev/null | head -1)
  echo "==== desktop: $n (autotools) ===="
  xt "$tb"
  ./configure --prefix=/usr --disable-static $opts
  make; make install; fin; touch $STAMPS/x-$n
}

# ---------- 1) GTK3 runtime stack ----------
abuild libffi
abuild libpng
abuild freetype "--enable-freetype-config --without-harfbuzz"
abuild fontconfig "--sysconfdir=/etc --localstatedir=/var --disable-docs"
[ -f $STAMPS/x-jpegsrc ] || { xt jpegsrc*.tar.gz; ./configure --prefix=/usr; make; make install; fin; touch $STAMPS/x-jpegsrc; }
# NOTE: call the *correct* builder per package directly. Do NOT use
# `abuild X || mbuild X`: in a || context bash suspends `set -e` inside the
# function, so a failed ./configure is ignored and the function still stamps
# success at its end (a false-positive that silently skips the real build).
mbuild pixman "-D tests=disabled -D demos=disabled"
abuild pcre2 "--enable-unicode --enable-pcre2-16 --enable-pcre2-32"
mbuild glib "-D introspection=disabled -D man-pages=disabled -D tests=false"
mbuild harfbuzz "-D glib=enabled -D freetype=enabled -D tests=disabled -D docs=disabled"
mbuild cairo
mbuild fribidi "-D docs=false -D tests=false"
mbuild pango "-D introspection=disabled"
mbuild gdk-pixbuf "-D introspection=disabled -D man=false -D gio_sniffing=false"
mbuild atk "-D introspection=false"
mbuild dbus "-D systemd=disabled -D x11_autolaunch=disabled -D modular_tests=disabled -D doxygen_docs=disabled -D xml_docs=disabled -D apparmor=disabled -D selinux=disabled"
abuild libxml2 "--without-python --without-lzma"
mbuild at-spi2-core "-D introspection=disabled -D systemd_user_dir=/usr/lib/systemd/user"
# NB: libepoxy + gtk are built AFTER mesa (section 2) — they need EGL/GL headers.

# ---------- 2) Wayland + compositor stack (software) ----------
mbuild wayland "-D documentation=false -D tests=false"
mbuild wayland-protocols "-D tests=false"
mbuild libxkbcommon "-D enable-docs=false -D enable-x11=false"
mbuild libdrm "-D tests=false"
abuild libevdev "--disable-static"
abuild mtdev ""
mbuild libinput "-D debug-gui=false -D tests=false -D documentation=false -D libwacom=false"
mbuild seatd "-D man-pages=disabled -D examples=disabled"
# hwdata: data-only (pnp.ids) required by libdisplay-info; ships a plain ./configure
if [ ! -f $STAMPS/x-hwdata ]; then
  echo "==== desktop: hwdata (data) ===="
  tb=$(ls hwdata-*.tar.* 2>/dev/null | head -1); xt "$tb"
  ./configure --prefix=/usr --datadir=/usr/share
  make install
  fin; touch $STAMPS/x-hwdata
fi
mbuild libdisplay-info ""

# mesa: libgbm ONLY. Mesa 24.3 removed softpipe (swrast == llvmpipe, needs LLVM),
# so we build no GL drivers at all — just libgbm, which wlroots' DRM backend links
# against. wlroots renders in software with pixman and scans out via DRM dumb
# buffers (its gbm->dumb fallback), so zero GPU / zero LLVM is needed in the VM.
# python-mako + PyYAML are mesa build-time deps.
if [ ! -f $STAMPS/x-mako ]; then
  echo "==== desktop: python build deps (Mako + PyYAML, for mesa) ===="
  MK=$(ls /sources/extras/Mako-*.tar.* | head -1)
  YM=$(ls /sources/extras/pyyaml-*.tar.* /sources/extras/PyYAML-*.tar.* 2>/dev/null | head -1)
  pip3 wheel -w /tmp/pywheels --no-build-isolation --no-deps "$MK" "$YM"
  pip3 install --no-index --no-build-isolation --find-links /tmp/pywheels Mako PyYAML
  python3 -c "import mako, yaml; print('mako', mako.__version__, 'pyyaml', yaml.__version__)"  # abort if missing
  touch $STAMPS/x-mako
fi
mbuild mesa "-D gallium-drivers= -D vulkan-drivers= -D platforms=wayland -D glx=disabled -D egl=disabled -D gles1=disabled -D gles2=disabled -D opengl=false -D gbm=enabled -D llvm=disabled -D video-codecs= -D valgrind=disabled -D libunwind=disabled -D shared-glapi=disabled"

# GTK's wayland backend #includes epoxy/egl.h, which libepoxy only installs when
# built with EGL. Our LLVM-free mesa has no EGL, so hand libepoxy the Khronos EGL
# headers (from mesa's source) + a stub egl.pc. No EGL runtime is used — the
# desktop renders in software (GDK_GL=disable), epoxy just needs the header.
if [ ! -f $STAMPS/x-eglhdr ]; then
  echo "==== desktop: stub EGL headers (for libepoxy/GTK) ===="
  cd /sources/extras
  MT=$(ls mesa-*.tar.* | head -1)
  MD=$(tar tf "$MT" | head -1 | cut -d/ -f1)
  tar xf "$MT" "$MD/include/EGL" "$MD/include/KHR"
  cp -rv "$MD/include/EGL" /usr/include/
  cp -rv "$MD/include/KHR" /usr/include/
  rm -rf "$MD"
  cd /sources/extras
  mkdir -p /usr/lib/pkgconfig
  cat > /usr/lib/pkgconfig/egl.pc <<'PC'
prefix=/usr
includedir=${prefix}/include
Name: egl
Description: stub EGL headers for libepoxy dispatch (no runtime driver)
Version: 1.5
Cflags: -I${includedir}
Libs:
PC
  touch $STAMPS/x-eglhdr
fi

# libepoxy is a GL *dispatch* lib. Build WITH egl (uses the stub headers above) so
# epoxy/egl.h is installed; the actual EGL entrypoints resolve lazily at runtime
# (and stay unused because the desktop renders via cairo).
mbuild libepoxy "-D glx=no -D egl=yes -D x11=false -D tests=false"
mbuild gtk "-D introspection=false -D demos=false -D examples=false -D tests=false -D man=false -D x11_backend=false -D wayland_backend=true -D print_backends=file -D colord=no"

# wlroots: software (pixman) renderer, DRM+libinput backends. Links libgbm but
# scans out via DRM dumb buffers under VirtualBox (no GPU).
# renderers= (empty) → wlroots builds only its always-on pixman software renderer
# (gles2/vulkan need EGL/GL we don't have). allocators=auto → gbm link, dumb-buffer
# fallback at runtime under VirtualBox.
mbuild wlroots "-D examples=false -D xwayland=disabled -D backends=drm,libinput -D renderers= -D allocators=auto"

# libsfdo: labwc 0.8.x dep (basedir/desktop/icon spec helpers)
mbuild libsfdo ""

# labwc (window manager). archive tarball unpacks to labwc-<ver>/
if [ ! -f $STAMPS/x-labwc ]; then
  echo "==== desktop: labwc (meson) ===="
  tb=$(ls labwc-*.tar.* 2>/dev/null | head -1); [ -n "$tb" ] || tb=$(ls 0.8.4.tar.gz 2>/dev/null | head -1)
  xt "$tb"; mkdir -p build; cd build
  meson setup --prefix=/usr --buildtype=release -D xwayland=disabled ..
  ninja; ninja install; cd ..; fin; touch $STAMPS/x-labwc
fi

# ---------- 3) gtk-layer-shell (for the Aurora shell surfaces) ----------
mbuild gtk-layer-shell "-D examples=false -D docs=false -D tests=false -D introspection=false -D vapi=false"

# ---------- 4) terminal (foot + deps) ----------
mbuild tllist ""
mbuild fcft "-D docs=disabled"
# foot 1.21 wants fcft>=3.3.1; treat the terminal as optional so a version skew
# doesn't block the desktop. (set -e in the subshell prevents a false stamp.)
( set -e; mbuild foot "-D docs=disabled -D themes=false -D terminfo=disabled" ) \
  || echo "WARN: foot terminal skipped (optional) — desktop builds without it"

# ---------- 5) Aura on-device LLM (llama-server + model + registry) ----------
if [ ! -f $STAMPS/x-llama ]; then
  if command -v cmake >/dev/null 2>&1; then
    echo "==== desktop: llama.cpp (cmake) ===="
    tb=$(ls llama.cpp-*.tar.gz 2>/dev/null | head -1); xt "$tb"
    case "${AURORA_ARCH:-$(uname -m)}" in
      aarch64) GA="-DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod";;
      *)       GA="-DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2";;
    esac
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=OFF $GA \
          -DLLAMA_CURL=OFF -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_SERVER=ON
    cmake --build build --config Release -j"$(nproc)" --target llama-server
    install -Dm755 build/bin/llama-server /opt/aura/bin/llama-server
    # If cmake produced shared libs (non-static build), install them to /usr/lib —
    # the build tree under /sources (the binary's rpath) is excluded from the
    # squashfs, so at runtime the libs must be on the default search path.
    for so in $(find build -name '*.so*' -type f 2>/dev/null); do
      install -Dm755 "$so" "/usr/lib/$(basename "$so")"
    done
    ldconfig 2>/dev/null || true
    fin; touch $STAMPS/x-llama
  else
    echo "WARN: cmake not present in target — skipping llama-server build."
    echo "      The Aura panel will run once llama-server is installed to /opt/aura/bin."
  fi
fi
install -Dm644 /aurora/config/aura-tools.json /opt/aura/config/aura-tools.json
install -Dm644 /aurora/shell/aura_llm.py       /usr/lib/aurora/aura_llm.py
install -Dm755 /aurora/shell/aurorad.py         /usr/lib/aurora/aurorad

# Aurora Store catalog (pipe-delimited; read by both aurorad and aurora-shell)
install -Dm644 /aurora/store/catalog /usr/share/aurora/store/catalog

# CA certificates — the LFS base ships none, so Python's ssl/urllib can't verify
# github.com and every Aurora Store download fails. Install a Mozilla CA bundle at
# OpenSSL's default path (Python reads get_default_verify_paths().openssl_cafile
# = /etc/ssl/cert.pem) plus the common ca-certificates.crt location.
if [ -f /aurora/config/cacert.pem ]; then
  install -Dm644 /aurora/config/cacert.pem /etc/ssl/cert.pem
  install -Dm644 /aurora/config/cacert.pem /etc/ssl/certs/ca-certificates.crt
else
  echo "!! config/cacert.pem missing — Aurora Store HTTPS downloads will fail cert verify"
fi

# ---------- 5b) SVG icon support (libcroco + librsvg, C — no Rust) ----------
# The LFS base has no SVG pixbuf loader, so app icons (mostly SVG) can't render.
# Build the C librsvg 2.40 against the base libs; it installs the gdk-pixbuf SVG
# loader. librsvg 2.40 needs a one-line patch for libxml2 >= 2.12 (xmlError is
# const now). Sources are expected in /sources (add them to 02-download-sources).
if [ ! -f /usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-svg.so ]; then
  cd /sources
  for t in libcroco-0.6.13 librsvg-2.40.21; do
    [ -d "$t" ] || { [ -f "$t.tar.xz" ] && tar xf "$t.tar.xz"; }
  done
  if [ -d libcroco-0.6.13 ] && [ -d librsvg-2.40.21 ]; then
    ( cd libcroco-0.6.13 && ./configure --prefix=/usr --disable-static && make -j"$(nproc)" && make install )
    ( cd librsvg-2.40.21 \
        && sed -i 's/rsvg_xml_noerror (void \*data, xmlErrorPtr error)/rsvg_xml_noerror (void *data, const xmlError *error)/' rsvg-css.c \
        && ./configure --prefix=/usr --disable-static --disable-introspection --enable-pixbuf-loader \
        && make -j"$(nproc)" && make install )
    gdk-pixbuf-query-loaders --update-cache
  else
    echo "!! librsvg/libcroco sources not in /sources — SVG app icons will fall back to glyphs"
  fi
fi

# ---------- 6) compile the Aurora shell ----------
echo "==== building aurora-shell ===="
install -d /usr/share/aurora/desktop /usr/bin
cc /aurora/shell/aurora-desktop/aurora-shell.c -O2 -o /usr/bin/aurora-shell \
   $(pkg-config --cflags --libs gtk+-3.0 gtk-layer-shell-0) -lm
install -Dm644 /aurora/shell/aurora-desktop/style.css /usr/share/aurora/desktop/style.css

# Aurora Settings app (native GTK3, own toplevel) + its launcher entry
echo "==== building aurora-settings ===="
cc /aurora/shell/aurora-desktop/aurora-settings.c -O2 -o /usr/bin/aurora-settings \
   $(pkg-config --cflags --libs gtk+-3.0)
cat > /usr/share/applications/aurora-settings.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=Settings
Comment=System settings
Exec=/usr/bin/aurora-settings
Icon=preferences-system
Categories=System;Settings;
Terminal=false
EOF

# ---------- 7) labwc session + autostart + theme ----------
install -d /etc/xdg/labwc /var/lib/aurora/.config/labwc
cat > /etc/xdg/labwc/rc.xml <<'EOF'
<?xml version="1.0"?>
<labwc_config>
  <theme><cornerRadius>8</cornerRadius></theme>
  <keyboard>
    <keybind key="A-Tab"><action name="NextWindow"/></keybind>
    <keybind key="A-F4"><action name="Close"/></keybind>
    <keybind key="W-q"><action name="Close"/></keybind>
    <keybind key="W-Return"><action name="Execute"><command>foot</command></action></keybind>
  </keyboard>
  <mouse>
    <context name="Title">
      <mousebind button="Left" action="Drag"><action name="Move"/></mousebind>
      <mousebind button="Left" action="DoubleClick"><action name="ToggleMaximize"/></mousebind>
    </context>
    <context name="TitleBar">
      <mousebind button="Left" action="Drag"><action name="Move"/></mousebind>
    </context>
    <context name="Close">
      <mousebind button="Left" action="Click"><action name="Close"/></mousebind>
    </context>
    <context name="Iconify">
      <mousebind button="Left" action="Click"><action name="Iconify"/></mousebind>
    </context>
    <context name="Maximize">
      <mousebind button="Left" action="Click"><action name="ToggleMaximize"/></mousebind>
    </context>
    <context name="Client">
      <mousebind button="Left" action="Press"><action name="Focus"/></mousebind>
      <mousebind button="Left" action="Press"><action name="Raise"/></mousebind>
    </context>
    <context name="Root">
      <mousebind button="Right" action="Press"><action name="ShowMenu"><menu>root-menu</menu></action></mousebind>
    </context>
  </mouse>
</labwc_config>
EOF
cat > /etc/xdg/labwc/autostart <<'EOF'
# AuroraOS session autostart (labwc)
/usr/lib/aurora/aura-llm-launch &
/usr/bin/python3 /usr/lib/aurora/aurorad &
/usr/bin/aurora-shell &
EOF

# helper that starts llama-server with the bundled model (if present)
cat > /usr/lib/aurora/aura-llm-launch <<'EOF'
#!/bin/sh
# On-device LLM server for Aura. Libs live in /usr/lib (the build tree under
# /sources, the binary's rpath, is excluded from the squashfs at runtime).
export LD_LIBRARY_PATH="/usr/lib:${LD_LIBRARY_PATH}"
# Pick the largest bundled model (prefers e.g. a 3B over a 1B if both exist).
M=$(ls -S /opt/aura/models/*.gguf 2>/dev/null | head -1)
[ -x /opt/aura/bin/llama-server ] && [ -n "$M" ] && [ -f "$M" ] && \
  exec /opt/aura/bin/llama-server --model "$M" --host 127.0.0.1 --port 8080 --ctx-size 2048
EOF
chmod +x /usr/lib/aurora/aura-llm-launch

# the session entry point: launch labwc for the aurora user
cat > /usr/bin/aurora-session <<'EOF'
#!/bin/sh
# Robust runtime dir: logind normally provides XDG_RUNTIME_DIR (/run/user/UID),
# but if the system bus/logind is slow it may be unset or unwritable — labwc
# then fails with "unable to open wayland socket". Guarantee a writable dir.
RT="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if ! mkdir -p "$RT" 2>/dev/null || [ ! -w "$RT" ]; then
  RT="/tmp/aurora-rt-$(id -u)"
  mkdir -p "$RT"
fi
chmod 700 "$RT"
export XDG_RUNTIME_DIR="$RT"
export XDG_CURRENT_DESKTOP=Aurora
export GDK_BACKEND=wayland
export GDK_GL=disable
export WLR_RENDERER=pixman
export WLR_NO_HARDWARE_CURSORS=1
export AURORAD_PORT=7212
export AURA_TOOLS=/opt/aura/config/aura-tools.json
export AURA_LLM_URL=http://127.0.0.1:8080/v1/chat/completions
exec labwc
EOF
chmod +x /usr/bin/aurora-session

# ---------- 8) autologin the aurora user on tty1 -> aurora-session ----------
# give the kiosk user a shell + a login that starts the desktop
usermod -s /bin/bash aurora 2>/dev/null || true
mkdir -p /etc/systemd/system/getty@tty1.service.d
cat > /etc/systemd/system/getty@tty1.service.d/autologin.conf <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin aurora --noissue %I $TERM
EOF
# start the desktop on login at tty1
cat > /var/lib/aurora/.bash_profile <<'EOF'
# AuroraOS: start the desktop automatically on the first VT
if [ -z "$WAYLAND_DISPLAY" ] && [ "$(tty)" = "/dev/tty1" ]; then
  printf '\033[2J\033[H'   # blank the VT so no login text flashes pre-desktop
  exec /usr/bin/aurora-session
fi
EOF
# suppress login(1)'s "Last login:" line — boot should be silent like macOS
touch /var/lib/aurora/.hushlogin
chown -R aurora:aurora /var/lib/aurora
systemctl set-default multi-user.target

echo "== 13 complete — native Aurora desktop installed. Re-squash + rebuild ISO. =="
