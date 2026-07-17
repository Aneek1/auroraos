#!/bin/bash
# DaybreakOS 24 — Intel hardware GL (iris). BUILD-BLIND (not testable in VBox).
#
# iris is the deepest driver to add because mesa hard-wires:
#   with_gallium_iris -> with_intel_clc -> with_clc -> dependency('libclc')
# (mesa meson.build:308,811,814). So iris drags in a whole compiler chain that
# radeonsi/nouveau did not need:
#   clang  (libclang-cpp + Clang cmake pkg)      -- added to the scripts/22 LLVM
#   SPIRV-Headers + SPIRV-Tools                  -- SPIR-V assembler/validator
#   SPIRV-LLVM-Translator (libLLVMSPIRVLib)      -- LLVM IR -> SPIR-V
#   libclc (spirv-mesa3d builtins)               -- OpenCL builtins for intel_clc
# Then mesa is rebuilt adding iris to the driver set.
#
# PREREQ: scripts/22 done (LLVM 18.1.8 X86;AMDGPU + mesa GL stack) and the SPIRV
# sources fetched into /sources/extras on the container HOST (chroot has no net):
#   SPIRV-Headers  @ vulkan-sdk-1.3.290.0   (github KhronosGroup/SPIRV-Headers)
#   SPIRV-Tools    @ vulkan-sdk-1.3.290.0   (github KhronosGroup/SPIRV-Tools)
#   SPIRV-LLVM-Translator @ TAG v18.1.6     (NOT the llvm_release_180 branch HEAD —
#       it chases newer FP8 header enums than 1.3.290 has -> compile errors)
#   libclc-18.1.8.src.tar.xz                (github llvm/llvm-project releases)
# Run INSIDE the chroot. LONG: clang dominates (~30-45 min @ -j3; Sema TUs are
# RAM-heavy, -j6 OOM-kills cc1plus). Everything else ~30 min total.
set -e
export PATH=/opt/cmake/bin:/bin:/usr/bin:/sbin:/usr/sbin
export PKG_CONFIG_PATH=/usr/lib/pkgconfig:/usr/share/pkgconfig
cd /sources/extras
LLVMSRC=llvm-project-18.1.8.src   # persisted from scripts/22 (tree + bld/)

# the stub /usr/bin/git bakes a bad SHA into git_sha1.h -> hide during mesa build
GITSTUB=/usr/bin/git; GITBAK=/usr/bin/git.hidden-for-glbuild

# ---------- 1) clang (add to the persisted LLVM bld; reuses object cache) ----------
if [ ! -x /usr/bin/clang ]; then
  echo "==== clang 18.1.8 (in-tree add) — long, -j3 to avoid OOM ===="
  cd "$LLVMSRC"
  cmake -G Ninja -S llvm -B bld -DLLVM_ENABLE_PROJECTS=clang
  ninja -C bld -j3 clang libclang-cpp.so      # NOTE target is libclang-cpp.so not libclang-cpp
  ninja -C bld install
  cd /sources/extras
  clang --version | head -1
fi

# ---------- 2) SPIRV-Headers (header-only) ----------
if [ ! -f /usr/include/spirv/unified1/spirv.h ]; then
  echo "==== SPIRV-Headers ===="
  cmake -G Ninja -S SPIRV-Headers -B SPIRV-Headers/b -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
  ninja -C SPIRV-Headers/b install
fi

# ---------- 3) SPIRV-Tools (shared) ----------
if [ ! -f /usr/lib/pkgconfig/SPIRV-Tools.pc ]; then
  echo "==== SPIRV-Tools ===="
  cmake -G Ninja -S SPIRV-Tools -B SPIRV-Tools/b \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
    -DSPIRV-Headers_SOURCE_DIR=/sources/extras/SPIRV-Headers \
    -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DBUILD_SHARED_LIBS=ON
  ninja -C SPIRV-Tools/b; ninja -C SPIRV-Tools/b install
fi

# ---------- 4) SPIRV-LLVM-Translator (libLLVMSPIRVLib + llvm-spirv) ----------
if [ ! -x /usr/bin/llvm-spirv ]; then
  echo "==== SPIRV-LLVM-Translator (v18.1.6) ===="
  cmake -G Ninja -S SPIRV-LLVM-Translator -B SPIRV-LLVM-Translator/b \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
    -DLLVM_DIR=/usr/lib/cmake/llvm \
    -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR=/sources/extras/SPIRV-Headers \
    -DLLVM_SPIRV_INCLUDE_TESTS=OFF -DBUILD_SHARED_LIBS=ON
  ninja -C SPIRV-LLVM-Translator/b; ninja -C SPIRV-LLVM-Translator/b install
fi

# ---------- 5) libclc (spirv-mesa3d builtins) ----------
if [ ! -f /usr/share/pkgconfig/libclc.pc ]; then
  echo "==== libclc 18.1.8 (standalone tarball) ===="
  rm -rf libclc-18.1.8.src; tar xf libclc-18.1.8.src.tar.xz; cd libclc-18.1.8.src
  # CRITICAL: pass LLVM_DIR so libclc uses find_package(LLVM) instead of
  # synthesizing its own LLVM-Config.cmake (that fabrication is emitted once per
  # target -> ninja "defined as an output multiple times" hard error).
  cmake -B build -G Ninja -S . \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_SKIP_INSTALL_RPATH=ON \
    -DLLVM_DIR=/usr/lib/cmake/llvm \
    -DLIBCLC_TARGETS_TO_BUILD="spirv-mesa3d-;spirv64-mesa3d-"
  ninja -C build -j6; ninja -C build install
  cd /sources/extras
fi

# ---------- 6) rebuild mesa adding iris ----------
echo "==== rebuild mesa: iris,radeonsi,nouveau,llvmpipe ===="
[ -f "$GITSTUB" ] && mv -f "$GITSTUB" "$GITBAK"
trap '[ -f "$GITBAK" ] && mv -f "$GITBAK" "$GITSTUB"' EXIT
# purge stray lib64 mesa artifacts (meson auto-picks lib64 once /usr/lib64 exists)
rm -rf /usr/lib64/dri
rm -f /usr/lib64/libgallium* /usr/lib64/libGL* /usr/lib64/libEGL* /usr/lib64/libgbm* \
      /usr/lib64/libglapi* /usr/lib64/libGLESv2* /usr/lib64/libOSMesa* 2>/dev/null || true
rm -rf mesa-24.3.4; tar xf mesa-24.3.4.tar.xz; cd mesa-24.3.4
MOPTS="-D gallium-drivers=iris,radeonsi,nouveau,llvmpipe -D vulkan-drivers= \
 -D intel-clc=enabled -D platforms=x11,wayland -D glx=dri -D egl=enabled \
 -D gbm=enabled -D opengl=true -D gles1=disabled -D gles2=enabled \
 -D shared-glapi=enabled -D llvm=enabled -D shared-llvm=enabled -D draw-use-llvm=true \
 -D video-codecs= -D valgrind=disabled -D libunwind=disabled"
mkdir -p b && cd b
# shellcheck disable=SC2086 -- FORCE libdir=lib (LFS loader uses /usr/lib)
meson setup --prefix=/usr --libdir=lib --buildtype=release $MOPTS ..
ninja -j6; ninja install
cd /sources/extras

echo "==== installed DRI drivers ===="
ls -la /usr/lib/dri/
for d in iris radeonsi nouveau; do
  test -e /usr/lib/dri/${d}_dri.so && echo "  OK ${d}_dri.so" || echo "  WARN ${d}_dri.so missing"
done
echo "== 24 complete (iris + radeonsi + nouveau + llvmpipe). Rebuild ISO. =="
