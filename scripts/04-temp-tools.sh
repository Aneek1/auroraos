#!/bin/bash
# AuroraOS 04 — cross-compiled temporary tools (LFS 12.3 ch. 6)
set -e
. "$(dirname "$0")/../config/build.conf"

sudo -u lfs env -i HOME=/home/lfs TERM="$TERM" \
  LFS="$LFS" LFS_TGT="$LFS_TGT" MAKEFLAGS="$MAKEFLAGS" \
  PATH="$LFS/tools/bin:/usr/bin:/bin" CONFIG_SITE="$LFS/usr/share/config.site" \
  bash -e <<'LFSEOF'
cd $LFS/sources
STAMPS=$LFS/var/lib/aurora-build
stamp(){ [ -f $STAMPS/$1 ]; }; mark(){ touch $STAMPS/$1; }
CFG="--host=$LFS_TGT --build=$(sh gcc-*/config.guess 2>/dev/null || echo $(uname -m)-pc-linux-gnu)"

# generic cross build: name | extra configure args | extra make-install args
xbuild(){
  local name=$1 conf=$2 inst=$3 tb
  stamp tmp-$name && return 0
  tb=$(ls $name-*.tar.* | head -1)
  rm -rf ${tb%.tar*}; tar xf $tb; cd ${tb%.tar*}
  eval ./configure --prefix=/usr $CFG $conf
  make
  make DESTDIR=$LFS $inst install
  cd $LFS/sources; rm -rf ${tb%.tar*}; mark tmp-$name; echo "== tmp $name done =="
}

xbuild m4        ""
# ncurses needs host tic first (LFS 6.3)
if ! stamp tmp-ncurses; then
  tb=$(ls ncurses-*.tar.gz | head -1); rm -rf ${tb%.tar.gz}; tar xf $tb; cd ${tb%.tar.gz}
  mkdir build && pushd build && ../configure AWK=gawk && make -C include && make -C progs tic && popd
  ./configure --prefix=/usr $CFG --mandir=/usr/share/man --with-manpage-format=normal \
    --with-shared --without-normal --with-cxx-shared --without-debug --without-ada \
    --disable-stripping AWK=gawk
  make && make DESTDIR=$LFS TIC_PATH=$(pwd)/build/progs/tic install
  ln -sv libncursesw.so $LFS/usr/lib/libncurses.so
  sed -e 's/^#if.*XOPEN.*$/#if 1/' -i $LFS/usr/include/curses.h
  cd $LFS/sources; rm -rf ${tb%.tar.gz}; mark tmp-ncurses; echo "== tmp ncurses done =="
fi
xbuild bash      "--without-bash-malloc bash_cv_strtold_broken=no" && ln -sfv bash $LFS/bin/sh
xbuild coreutils "--enable-install-program=hostname --enable-no-install-program=kill,uptime" \
  && mv -v $LFS/usr/bin/chroot $LFS/usr/sbin 2>/dev/null || true
xbuild diffutils "gl_cv_func_strcasecmp_works=y"
# file needs its own host binary (LFS 6.7)
if ! stamp tmp-file; then
  tb=$(ls file-*.tar.gz | head -1); rm -rf ${tb%.tar.gz}; tar xf $tb; cd ${tb%.tar.gz}
  mkdir build && pushd build && ../configure --disable-bzlib --disable-libseccomp \
    --disable-xzlib --disable-zlib && make && popd
  ./configure --prefix=/usr $CFG
  make FILE_COMPILE=$(pwd)/build/src/file && make DESTDIR=$LFS install
  rm -v $LFS/usr/lib/libmagic.la 2>/dev/null || true
  cd $LFS/sources; rm -rf ${tb%.tar.gz}; mark tmp-file; echo "== tmp file done =="
fi
xbuild findutils "--localstatedir=/var/lib/locate"
xbuild gawk      ""
xbuild grep      ""
xbuild gzip      ""
xbuild make      "--without-guile"
xbuild patch     ""
xbuild sed       ""
xbuild tar       ""
xbuild xz        "--disable-static --docdir=/usr/share/doc/xz"

# ---- binutils pass 2 (LFS 6.16) ----
if ! stamp tmp-binutils2; then
  tb=$(ls binutils-*.tar.xz | head -1); rm -rf ${tb%.tar.xz}; tar xf $tb; cd ${tb%.tar.xz}
  sed '6009s/$add_dir//' -i ltmain.sh
  mkdir -v build && cd build
  ../configure --prefix=/usr $CFG --disable-nls --enable-shared --enable-gprofng=no \
    --disable-werror --enable-64-bit-bfd --enable-new-dtags --enable-default-hash-style=gnu
  make && make DESTDIR=$LFS install
  rm -v $LFS/usr/lib/lib{bfd,ctf,ctf-nobfd,opcodes,sframe}.{a,la} 2>/dev/null || true
  cd $LFS/sources; rm -rf ${tb%.tar.xz}; mark tmp-binutils2; echo "== binutils pass2 done =="
fi

# ---- gcc pass 2 (LFS 6.17) ----
if ! stamp tmp-gcc2; then
  tb=$(ls gcc-*.tar.xz | head -1); rm -rf ${tb%.tar.xz}; tar xf $tb; cd ${tb%.tar.xz}
  tar xf ../mpfr-*.tar.xz && mv mpfr-* mpfr
  tar xf ../gmp-*.tar.xz  && mv gmp-*  gmp
  tar xf ../mpc-*.tar.gz  && mv mpc-*  mpc
  case $(uname -m) in x86_64) sed -e '/m64=/s/lib64/lib/' -i.orig gcc/config/i386/t-linux64 ;; esac
  sed '/thread_header =/s/@.*@/gthr-posix.h/' -i libgcc/Makefile.in libstdc++-v3/include/Makefile.in
  mkdir -v build && cd build
  ../configure $CFG --target=$LFS_TGT LDFLAGS_FOR_TARGET=-L$PWD/$LFS_TGT/libgcc \
    --prefix=/usr --with-build-sysroot=$LFS --enable-default-pie --enable-default-ssp \
    --disable-nls --disable-multilib --disable-libatomic --disable-libgomp \
    --disable-libquadmath --disable-libsanitizer --disable-libssp --disable-libvtv \
    --enable-languages=c,c++
  make && make DESTDIR=$LFS install
  ln -sfv gcc $LFS/usr/bin/cc
  cd $LFS/sources; rm -rf ${tb%.tar.xz}; mark tmp-gcc2; echo "== gcc pass2 done =="
fi
echo "== 04 complete — run 05-enter-chroot.sh (as root) =="
LFSEOF
