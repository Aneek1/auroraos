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
abuild(){ # autotools-based; returns non-zero (no bogus stamp) if the source isn't
          # autotools, so `abuild X || mbuild X` callers correctly fall back to meson.
  local n=$1 opts=$2 tb
  [ -f $STAMPS/x-$n ] && return 0
  tb=$(ls ${n}-*.tar.* ${n}src*.tar.* 2>/dev/null | head -1)
  [ -n "$tb" ] || return 1
  echo "==== extras: $n (autotools) ===="
  xt "$tb"
  [ -f ./configure ] || { cd /sources/extras; return 1; }   # meson-only -> fall back
  ./configure --prefix=/usr --disable-static $opts && make && make install \
    && { fin; touch $STAMPS/x-$n; }
}

# ---------- 1) Firefox GTK3 runtime deps ----------
abuild libffi
abuild libpng
abuild freetype "--enable-freetype-config --without-harfbuzz"
abuild fontconfig "--sysconfdir=/etc --localstatedir=/var --disable-docs"
abuild pcre2 "--enable-unicode"   # pcre2 is autotools/CMake, not meson
mbuild glib "-D introspection=disabled -D man-pages=disabled -D tests=false"
abuild jpegsrc "" || true   # libjpeg (jpegsrc.v9f)
[ -f $STAMPS/x-jpegsrc ] || { xt jpegsrc*.tar.gz; ./configure --prefix=/usr; make; make install; fin; touch $STAMPS/x-jpegsrc; }
abuild pixman "" 2>/dev/null || mbuild pixman "-D demos=disabled -D tests=disabled"   # cairo needs pixman
mbuild cairo "-D xlib=enabled"
mbuild harfbuzz "-D glib=enabled -D freetype=enabled -D tests=disabled -D docs=disabled"
mbuild fribidi "-D docs=false -D tests=false"   # pango needs fribidi
mbuild pango "-D introspection=disabled"
mbuild gdk-pixbuf "-D introspection=disabled -D man=false -D gio_sniffing=false -D jpeg=disabled -D tiff=disabled -D builtin_loaders=png"   # avoid shared-mime-info + libjpeg-turbo/cmake; PNG is what GTK needs
mbuild atk "-D introspection=disabled" 2>/dev/null || mbuild atk ""
abuild libxml2 "--without-python"   # at-spi2-core needs libxml-2.0
mbuild at-spi2-core "-D introspection=disabled -D systemd_user_dir=/usr/lib/systemd/user"
abuild dbus "--sysconfdir=/etc --localstatedir=/var --runstatedir=/run --with-systemduserunitdir=no --with-systemdsystemunitdir=no" 2>/dev/null || true

# ---------- 2) Wayland kiosk stack ----------
mbuild wayland "-D documentation=false -D tests=false"
mbuild wayland-protocols "-D tests=false"
abuild libdrm "" 2>/dev/null || mbuild libdrm "-D tests=false"
# --- cmake (build tool) + LLVM (mesa llvmpipe software rendering) ---
# Heavy; both self-skip if already installed. cmake bootstraps without needing cmake.
if ! command -v cmake >/dev/null 2>&1 && [ ! -f $STAMPS/x-cmake ]; then
  echo "==== extras: cmake (bootstrap) ===="
  xt $(ls cmake-[0-9]*.tar.gz | head -1)
  ./bootstrap --prefix=/usr --parallel=$(nproc) -- -DCMAKE_USE_OPENSSL=OFF
  make; make install; fin; touch $STAMPS/x-cmake
fi
if ! command -v llvm-config >/dev/null 2>&1 && [ ! -f $STAMPS/x-llvm ]; then
  echo "==== extras: llvm (cmake/ninja, AArch64 dylib) ===="
  rm -rf llvmtree; mkdir llvmtree; cd llvmtree
  tar xf ../llvm-*.src.tar.xz; tar xf ../cmake-*.src.tar.xz; tar xf ../third-party-*.src.tar.xz
  mv llvm-*.src llvm; mv cmake-*.src cmake; mv third-party-*.src third-party
  cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON \
    -DLLVM_ENABLE_RTTI=ON -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_PARALLEL_LINK_JOBS=2
  ninja -C build; ninja -C build install; cd /sources/extras; rm -rf llvmtree; touch $STAMPS/x-llvm
fi
# mesa AFTER llvm (needs llvm-config) and BEFORE libepoxy (epoxy's egl needs mesa headers)
# mesa's Python build deps (code generators): Mako + PyYAML
[ -f $STAMPS/x-pymesa ] || {
  pip3 install --no-index --no-build-isolation "$(ls [Mm]ako-*.tar.gz | head -1)"
  pip3 install --no-index --no-build-isolation "$(ls [Pp]y[Yy][Aa][Mm][Ll]-*.tar.gz pyyaml-*.tar.gz 2>/dev/null | head -1)"
  touch $STAMPS/x-pymesa
}
mbuild mesa "-D platforms=wayland -D glx=disabled -D egl=enabled -D gles2=enabled -D gbm=enabled -D vulkan-drivers= -D gallium-drivers=swrast,virgl -D video-codecs= -D llvm=enabled -D shared-llvm=enabled"
mbuild libepoxy "-D glx=yes -D x11=true -D egl=yes -D tests=false"
abuild libevdev "--disable-static" 2>/dev/null || mbuild libevdev "-D tests=disabled -D documentation=disabled"
abuild mtdev ""   # libinput needs mtdev
mbuild libinput "-D debug-gui=false -D tests=false -D documentation=false -D libwacom=false"
mbuild libxkbcommon "-D enable-docs=false -D enable-x11=true"
mbuild seatd 2>/dev/null || { [ -f $STAMPS/x-seatd ] || { xt 0.9.1.tar.gz; mkdir -p build; cd build; meson setup --prefix=/usr ..; ninja; ninja install; cd ..; fin; touch $STAMPS/x-seatd; }; }
[ -f $STAMPS/x-hwdata ] || { xt "$(ls hwdata-*.tar.gz | head -1)"; ./configure --prefix=/usr --libdir=/usr/lib --datadir=/usr/share; make install; fin; touch $STAMPS/x-hwdata; }   # libdisplay-info needs hwdata pnp.ids
mbuild libdisplay-info ""   # hard dep of wlroots 0.18 — build it FIRST (proven in the arm64 compile proof)
mbuild wlroots "-D examples=false -D xwayland=disabled -D backends=drm,libinput -D renderers=gles2"
mbuild cage "-D man-pages=disabled" 2>/dev/null || mbuild cage ""

# GTK3 is built AFTER the wayland stack: the prebuilt Firefox links this system GTK3
# and must run under cage (wayland), so GTK3 needs wayland/epoxy/mesa/xkbcommon present.
mbuild gtk "-D introspection=false -D demos=false -D examples=false -D tests=false -D man=false -D print_backends=file -D x11_backend=true -D wayland_backend=true"   # x11_backend needs the script-13 X11 libs; the prebuilt Firefox needs gdk_x11_* symbols

# fonts — the shell's UI font is Noto Sans. Install the prebuilt hinted TTFs
# (the old notofonts *source* archive shipped no .ttf, so text rendered as tofu).
if [ ! -f $STAMPS/x-fonts ]; then
  install -d /usr/share/fonts/noto
  install -m644 NotoSans-*.ttf /usr/share/fonts/noto/
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

# ==== Aura on-device LLM: build llama-server, install model + registry ====
if [ ! -f $STAMPS/x-llama ]; then
  echo "==== extras: llama.cpp (cmake) ===="
  tb=$(ls llama.cpp-*.tar.gz 2>/dev/null | head -1)
  xt "$tb"
  # ggml's cmake REQUIREs `git` only to stamp a build version; the base system has no
  # git package, so provide a stub returning the known tag. Scoped to this build via PATH.
  mkdir -p gitstub
  printf '#!/bin/sh\ncase "$*" in\n  *"rev-list --count"*) echo 4589;;\n  *"rev-parse --short"*) echo b4589;;\n  *) echo b4589;;\nesac\n' > gitstub/git
  chmod +x gitstub/git
  export PATH="$PWD/gitstub:$PATH"
  # NOT -DGGML_NATIVE=ON: it emits -mcpu=native+... which gcc rejects if it doesn't
  # know the CPU's MIDR (fails in the QEMU arm64 proof AND can fail on a fresh M4).
  # armv8.2-a+dotprod is a safe Apple-Silicon baseline (all M-series have dotprod;
  # big int8 matmul speedup) — proven to compile on aarch64. Bump to +i8mm/higher
  # for more perf if your toolchain accepts it.
  cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=OFF -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod \
        -DLLAMA_CURL=OFF -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_SERVER=ON
  cmake --build build --config Release -j"$(nproc)" --target llama-server
  install -Dm755 build/bin/llama-server /opt/aura/bin/llama-server
  fin; touch $STAMPS/x-llama
fi

# aura assets: the shared tool registry + the LLM module live next to the shell
install -Dm644 /aurora/config/aura-tools.json /opt/aura/config/aura-tools.json
install -Dm644 /aurora/shell/aura_llm.py /usr/lib/aurora/aura_llm.py

# ---------- 4) AuroraOS shell + aurorad ----------
install -d /usr/share/aurora/shell /usr/lib/aurora
install -m644 /aurora/shell/index.html        /usr/share/aurora/shell/
install -m644 /aurora/shell/aurora-bridge.js  /usr/share/aurora/shell/
install -m755 /aurora/shell/aurorad.py        /usr/lib/aurora/aurorad
install -m755 /aurora/shell/aurora-session    /usr/bin/aurora-session

install -m644 /aurora/systemd/aurora-shell.service /usr/lib/systemd/system/
install -m644 /aurora/systemd/seatd.service        /usr/lib/systemd/system/ 2>/dev/null || true

systemctl enable seatd aurora-shell
systemctl set-default graphical.target

# ---------- Aura systemd units (llama-server first, aurorad with LLM env) ----------
# Replaces the legacy /aurora/systemd/aurorad.service: aurorad now Wants the model
# and gets AURA_TOOLS / AURA_LLM_URL from the environment.
cat > /etc/systemd/system/aura-llm.service <<'EOF'
[Unit]
Description=Aura on-device LLM (llama.cpp)
After=network.target
[Service]
User=aurora
ExecStart=/opt/aura/bin/llama-server --model /opt/aura/models/Llama-3.2-1B-Instruct-Q4_K_M.gguf --host 127.0.0.1 --port 8080 --ctx-size 4096
Restart=on-failure
[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/aurorad.service <<'EOF'
[Unit]
Description=AuroraOS system bridge
After=network.target aura-llm.service
Wants=aura-llm.service
[Service]
Environment=AURA_TOOLS=/opt/aura/config/aura-tools.json
Environment=AURA_LLM_URL=http://127.0.0.1:8080/v1/chat/completions
ExecStart=/usr/bin/python3 /usr/lib/aurora/aurorad
Restart=on-failure
[Install]
WantedBy=multi-user.target
EOF

systemctl enable aura-llm.service aurorad.service

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
// no first-run / welcome / onboarding tab — boot straight into the shell (kiosk)
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.startup.firstrunSkipsHomepage", true);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("startup.homepage_welcome_url", "");
user_pref("startup.homepage_welcome_url.additional", "");
user_pref("trailhead.firstrun.didSeeAboutWelcome", true);
user_pref("browser.messaging-system.whatsNewPanel.enabled", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("browser.warnOnQuit", false);
user_pref("browser.aboutConfig.showWarning", false);
// performance on software rendering (VM llvmpipe / no GPU): force SwGL WebRender,
// kill animations/smooth-scroll, fewer content processes.
user_pref("gfx.webrender.software", true);
user_pref("gfx.canvas.accelerated", false);
user_pref("layers.acceleration.disabled", true);
user_pref("general.smoothScroll", false);
user_pref("toolkit.cosmeticAnimations.enabled", false);
user_pref("browser.tabs.animate", false);
user_pref("ui.prefersReducedMotion", 1);
user_pref("image.animation_mode", "none");
user_pref("dom.ipc.processCount", 2);
EOF
chown -R aurora:aurora /var/lib/aurora

echo "== 10 complete — exit chroot; optionally run 11-make-iso.sh, or reboot into AuroraOS =="
