#!/bin/bash
# DaybreakOS 22 — software GL stack so Xwayland can serve GLX. Steam's UI (VGUI2)
# calls glXChooseVisual; that fails because Xwayland was built with no GL renderer
# (no glamor) and the host mesa was driverless. This builds a software GL path:
#   xorgproto + libXxf86vm  -> mesa GLX build deps
#   LLVM (X86, dylib, RTTI) -> the llvmpipe software rasterizer backend
#   mesa rebuilt            -> gallium llvmpipe + GLX(dri) + EGL + x11 platform
#   Xwayland rebuilt        -> glamor (uses the new EGL/GL) so GLX visuals exist
# Run INSIDE the chroot. LONG: LLVM dominates (~20-40 min on 20 cores).
set -e
STAMPS=/var/lib/aurora-build
mkdir -p "$STAMPS"
export PATH=/opt/cmake/bin:$PATH
cd /sources/extras

xt(){ SRCDIR=$(tar tf "$1" | sed 's|^\./||' | awk 'NF' | head -1 | cut -d/ -f1); rm -rf "$SRCDIR"; tar xf "$1"; cd "$SRCDIR"; }
fin(){ cd /sources/extras; rm -rf "$SRCDIR"; }

# ---------- 1) xorgproto (X protocol headers incl glproto/xf86vmproto) ----------
if [ ! -f "$STAMPS/gl-xorgproto" ]; then
  echo "==== xorgproto ===="
  xt xorgproto-2024.1.tar.xz
  mkdir -p b && cd b && meson setup --prefix=/usr .. && ninja install
  cd ..; fin; touch "$STAMPS/gl-xorgproto"
fi

# ---------- 2) libXxf86vm ----------
if [ ! -f "$STAMPS/gl-xxf86vm" ]; then
  echo "==== libXxf86vm ===="
  xt libXxf86vm-1.1.5.tar.xz
  ./configure --prefix=/usr --disable-static
  make; make install
  fin; touch "$STAMPS/gl-xxf86vm"
fi

# ---------- 3) LLVM 18.1.8 (llvmpipe backend) ----------
if [ ! -x /usr/bin/llvm-config ]; then
  echo "==== LLVM 18.1.8 — long build ($(nproc) cores) ===="
  xt llvm-project-18.1.8.src.tar.xz     # -> llvm-project-18.1.8.src/
  cmake -G Ninja -S llvm -B bld \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_BUILD_LLVM_DYLIB=ON \
    -DLLVM_LINK_LLVM_DYLIB=ON \
    -DLLVM_ENABLE_RTTI=ON \
    -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF \
    -DLLVM_ENABLE_PROJECTS=
  ninja -C bld
  ninja -C bld install
  cd ..; fin
  llvm-config --version
fi

# The image ships a 30-byte STUB /usr/bin/git that echoes a fake "0000000\n".
# mesa's bin/git_sha1_gen.py runs `git rev-parse HEAD`, gets that newline-bearing
# string, and bakes  #define MESA_GIT_SHA1 " (git-0000000<NL>)"  into git_sha1.h
# -> "missing terminating character" C errors. Hide the stub so the generator
# falls back to an empty SHA (correct for a tarball build). Restored at the end.
GITSTUB=/usr/bin/git; GITBAK=/usr/bin/git.hidden-for-glbuild
[ -f "$GITSTUB" ] && mv -f "$GITSTUB" "$GITBAK"
restore_git(){ [ -f "$GITBAK" ] && mv -f "$GITBAK" "$GITSTUB"; }
trap restore_git EXIT

# ---------- 4) rebuild mesa with llvmpipe + GLX + EGL + x11 platform ----------
echo "==== rebuild mesa (llvmpipe + glx=dri + egl + x11,wayland) ===="
rm -f "$STAMPS/x-mesa"
MOPTS="-D gallium-drivers=llvmpipe -D vulkan-drivers= -D platforms=x11,wayland \
 -D glx=dri -D egl=enabled -D gbm=enabled -D opengl=true \
 -D gles1=disabled -D gles2=enabled -D shared-glapi=enabled \
 -D llvm=enabled -D shared-llvm=enabled -D draw-use-llvm=true \
 -D video-codecs= -D valgrind=disabled -D libunwind=disabled"
xt mesa-24.3.4.tar.xz
mkdir -p b && cd b
# shellcheck disable=SC2086
meson setup --prefix=/usr --buildtype=release --reconfigure $MOPTS .. \
  || meson setup --prefix=/usr --buildtype=release $MOPTS ..
ninja; ninja install
cd ..; fin
touch "$STAMPS/x-mesa"
test -f /usr/lib/libGL.so || { echo "!! libGL.so not installed — mesa GLX build failed"; exit 1; }

# ---------- 5) rebuild Xwayland WITH glamor ----------
echo "==== rebuild Xwayland 23.2.7 (glamor=true) ===="
xt xwayland-23.2.7.tar.xz
mkdir -p b && cd b
meson setup --prefix=/usr --buildtype=release \
  -D glamor=true -D xvfb=false -D dri3=true -D secure-rpc=false \
  -D xwayland_eglstream=false ..
ninja; ninja install
cd ..; fin
ldd /usr/bin/Xwayland | grep -iqE 'epoxy|GL' \
  && echo "Xwayland now links GL (glamor on)" \
  || echo "WARN: Xwayland still has no GL — check glamor build"

echo "== 22 complete — software GL + Xwayland glamor. Re-squash + rebuild ISO / live-patch. =="
