#!/bin/bash
# AuroraOS 03 — cross toolchain (LFS 12.3 ch. 5): binutils p1, gcc p1,
# linux headers, glibc, libstdc++. Run as root on the host; builds as user lfs.
set -e
. "$(dirname "$0")/../config/build.conf"

# lfs user (LFS ch. 4.3)
getent group lfs >/dev/null || groupadd lfs
id lfs &>/dev/null || useradd -s /bin/bash -g lfs -m -k /dev/null lfs
mkdir -pv "$LFS"/{etc,var,usr/{bin,lib,sbin},tools}
case "$AURORA_ARCH" in x86_64) mkdir -pv "$LFS/lib64" ;; esac
for i in bin lib sbin; do ln -sfv usr/$i "$LFS/$i"; done
chown -R lfs "$LFS"/{usr,tools,var,etc,sources} "$LFS/lib64" 2>/dev/null || true

# everything below runs as lfs with a clean env
sudo -u lfs env -i HOME=/home/lfs TERM="$TERM" \
  LFS="$LFS" LFS_TGT="$LFS_TGT" MAKEFLAGS="$MAKEFLAGS" \
  AURORA_ARCH="$AURORA_ARCH" LDSO="$LDSO" \
  PATH="$LFS/tools/bin:/usr/bin:/bin" CONFIG_SITE="$LFS/usr/share/config.site" \
  bash -e <<'LFSEOF'
cd $LFS/sources
STAMPS=$LFS/var/lib/aurora-build; mkdir -p $STAMPS
stamp(){ [ -f $STAMPS/$1 ]; }; mark(){ touch $STAMPS/$1; }
xt(){ rm -rf ${1%.tar*}; tar xf $1; cd ${1%.tar*}; }   # extract+enter
fin(){ cd $LFS/sources; rm -rf $1; }

# ---- binutils pass 1 (LFS 5.2) ----
if ! stamp binutils-p1; then
  P=$(ls binutils-*.tar.xz | head -1); xt $P
  mkdir -v build && cd build
  ../configure --prefix=$LFS/tools --with-sysroot=$LFS --target=$LFS_TGT \
               --disable-nls --enable-gprofng=no --disable-werror --enable-new-dtags \
               --enable-default-hash-style=gnu
  make && make install
  fin ${P%.tar.xz}; mark binutils-p1; echo "== binutils pass1 done =="
fi

# ---- gcc pass 1 (LFS 5.3) ----
if ! stamp gcc-p1; then
  P=$(ls gcc-*.tar.xz | head -1); xt $P
  tar xf ../mpfr-*.tar.xz && mv mpfr-* mpfr
  tar xf ../gmp-*.tar.xz  && mv gmp-*  gmp
  tar xf ../mpc-*.tar.gz  && mv mpc-*  mpc
  case $(uname -m) in x86_64) sed -e '/m64=/s/lib64/lib/' -i.orig gcc/config/i386/t-linux64 ;; esac
  mkdir -v build && cd build
  ../configure --target=$LFS_TGT --prefix=$LFS/tools --with-glibc-version=2.41 \
    --with-sysroot=$LFS --with-newlib --without-headers --enable-default-pie \
    --enable-default-ssp --disable-nls --disable-shared --disable-multilib \
    --disable-threads --disable-libatomic --disable-libgomp --disable-libquadmath \
    --disable-libssp --disable-libvtv --disable-libstdcxx \
    --enable-languages=c,c++
  make && make install
  cd ..
  cat gcc/limitx.h gcc/glimits.h gcc/limity.h > \
    $(dirname $($LFS_TGT-gcc -print-libgcc-file-name))/include/limits.h
  fin ${P%.tar.xz}; mark gcc-p1; echo "== gcc pass1 done =="
fi

# ---- linux API headers (LFS 5.4) ----
if ! stamp headers; then
  P=$(ls linux-*.tar.xz | head -1); xt $P
  make mrproper && make headers
  find usr/include -type f ! -name '*.h' -delete
  cp -rv usr/include $LFS/usr
  fin ${P%.tar.xz}; mark headers; echo "== headers done =="
fi

# ---- glibc (LFS 5.5) ----
if ! stamp glibc; then
  P=$(ls glibc-*.tar.xz | head -1); xt $P
  case "$AURORA_ARCH" in
    x86_64)
      ln -sfv ../lib/ld-linux-x86-64.so.2 $LFS/lib64
      ln -sfv ../lib/ld-linux-x86-64.so.2 $LFS/lib64/ld-lsb-x86-64.so.3 ;;
    aarch64) : ;;  # loader is /lib/ld-linux-aarch64.so.1 via the lib->usr/lib symlink; no lib64
  esac
  patch -Np1 -i ../glibc-*-fhs-1.patch
  mkdir -v build && cd build
  echo "rootsbindir=/usr/sbin" > configparms
  ../configure --prefix=/usr --host=$LFS_TGT --build=$(../scripts/config.guess) \
    --enable-kernel=5.4 --with-headers=$LFS/usr/include \
    --disable-nscd libc_cv_slibdir=/usr/lib
  make && make DESTDIR=$LFS install
  sed '/RTLDLIST=/s@/usr@@g' -i $LFS/usr/bin/ldd
  # sanity check
  echo 'int main(){}' | $LFS_TGT-gcc -xc -
  readelf -l a.out | grep -q "$LDSO" || { echo "!! glibc sanity FAILED (want $LDSO)"; exit 1; }
  rm a.out
  fin ${P%.tar.xz}; mark glibc; echo "== glibc done, sanity OK =="
fi

# ---- libstdc++ (LFS 5.6) ----
if ! stamp libstdcxx; then
  P=$(ls gcc-*.tar.xz | head -1); xt $P
  mkdir -v build && cd build
  ../libstdc++-v3/configure --host=$LFS_TGT --build=$(../config.guess) \
    --prefix=/usr --disable-multilib --disable-nls --disable-libstdcxx-pch \
    --with-gxx-include-dir=/tools/$LFS_TGT/include/c++/$(echo ${P} | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)
  make && make DESTDIR=$LFS install
  rm -v $LFS/usr/lib/lib{stdc++{,exp,fs},supc++}.la 2>/dev/null || true
  fin ${P%.tar.xz}; mark libstdcxx; echo "== libstdc++ done =="
fi
echo "== 03 complete — proceed to 04-temp-tools.sh =="
LFSEOF
