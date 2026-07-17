#!/bin/bash
# DaybreakOS 23 — hardware GL for real machines. BUILD-BLIND: not testable in
# VirtualBox (which only does VMSVGA); proves out on real Intel/AMD/NVIDIA GPUs.
#
# The kernel already ships amdgpu + i915 + vmwgfx DRM built-in (=y), all libdrm_*
# helpers + libelf (radeonsi's dep) are present, and LLVM (radeonsi/llvmpipe
# backend) was built by scripts/22. So HW GL for Intel (iris) and AMD (radeonsi)
# needs only: mesa rebuilt WITH those gallium drivers + GPU firmware (installed
# separately). nouveau gallium is included too, but NVIDIA also needs the kernel
# nouveau module (CONFIG_DRM_NOUVEAU) — a follow-up kernel rebuild.
# Run INSIDE the chroot. ~10 min (mesa).
set -e
STAMPS=/var/lib/aurora-build
export PATH=/opt/cmake/bin:$PATH
cd /sources/extras

xt(){ SRCDIR=$(tar tf "$1" | sed 's|^\./||' | awk 'NF' | head -1 | cut -d/ -f1); rm -rf "$SRCDIR"; tar xf "$1"; cd "$SRCDIR"; }
fin(){ cd /sources/extras; rm -rf "$SRCDIR"; }

# stub /usr/bin/git bakes a bad SHA into git_sha1.h -> hide it (see scripts/22)
GITSTUB=/usr/bin/git; GITBAK=/usr/bin/git.hidden-for-glbuild
[ -f "$GITSTUB" ] && mv -f "$GITSTUB" "$GITBAK"
trap '[ -f "$GITBAK" ] && mv -f "$GITBAK" "$GITSTUB"' EXIT

echo "==== rebuild mesa with HW gallium drivers (iris,radeonsi,nouveau,llvmpipe) ===="
rm -f "$STAMPS/x-mesa"
# NOTE on iris (Intel): mesa hard-wires `with_intel_clc = ... or with_gallium_iris`
# (meson.build:308) so building iris pulls in intel_clc -> libclc -> clang +
# SPIRV-LLVM-Translator + SPIRV-Tools (a multi-GB toolchain). We build that chain
# separately (scripts/24) and add iris in a second mesa pass. This pass ships the
# clang-free drivers: radeonsi (AMD, uses our LLVM), nouveau (NVIDIA), llvmpipe.
GDRIVERS="${GDRIVERS:-radeonsi,nouveau,llvmpipe}"
MOPTS="-D gallium-drivers=$GDRIVERS -D vulkan-drivers= \
 -D platforms=x11,wayland -D glx=dri -D egl=enabled -D gbm=enabled -D opengl=true \
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

echo "==== installed DRI drivers ===="
ls -la /usr/lib/dri/
case "$GDRIVERS" in *radeonsi*) test -e /usr/lib/dri/radeonsi_dri.so || echo "WARN: radeonsi_dri.so missing";; esac
case "$GDRIVERS" in *nouveau*)  test -e /usr/lib/dri/nouveau_dri.so  || echo "WARN: nouveau_dri.so missing";;  esac
case "$GDRIVERS" in *iris*)     test -e /usr/lib/dri/iris_dri.so     || echo "WARN: iris_dri.so missing";;     esac
echo "== 23 complete (drivers: $GDRIVERS). Install GPU firmware + rebuild ISO. =="
