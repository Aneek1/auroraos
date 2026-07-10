#!/bin/bash
# AuroraOS 06 — full base system, run INSIDE the chroot (LFS 12.3 ch. 7.5–8).
# Recipe-table implementation. Test suites are skipped for build time; for a
# production system run the glibc/gcc/binutils suites per the book.
set -e
STAMPS=/var/lib/aurora-build; mkdir -p $STAMPS
stamp(){ [ -f $STAMPS/$1 ]; }; mark(){ touch $STAMPS/$1; }
cd /sources

# ---------- ch 7.5–7.6: directories + essential files ----------
if ! stamp ch7-skel; then
  mkdir -pv /{boot,home,mnt,opt,srv}
  mkdir -pv /etc/{opt,sysconfig} /lib/firmware /media/{floppy,cdrom}
  mkdir -pv /usr/{,local/}{include,src} /usr/lib/locale
  mkdir -pv /usr/local/{bin,lib,sbin} /usr/{,local/}share/{color,dict,doc,info,locale,man}
  mkdir -pv /usr/{,local/}share/{misc,terminfo,zoneinfo} /usr/{,local/}share/man/man{1..8}
  mkdir -pv /var/{cache,local,log,mail,opt,spool} /var/lib/{color,misc,locate}
  install -dv -m 0750 /root; install -dv -m 1777 /tmp /var/tmp
  ln -sfv /run /var/run; ln -sfv /run/lock /var/lock
  ln -sv /proc/self/mounts /etc/mtab 2>/dev/null || true
  cat > /etc/hosts <<EOF
127.0.0.1  localhost aurora
::1        localhost
EOF
  cat > /etc/passwd <<"EOF"
root:x:0:0:root:/root:/bin/bash
bin:x:1:1:Legacy User:/dev/null:/usr/bin/false
daemon:x:6:6:Daemon User:/dev/null:/usr/bin/false
messagebus:x:18:18:D-Bus Message Daemon User:/run/dbus:/usr/bin/false
systemd-journal-gateway:x:73:73:systemd Journal Gateway:/:/usr/bin/false
systemd-journal-remote:x:74:74:systemd Journal Remote:/:/usr/bin/false
systemd-journal-upload:x:75:75:systemd Journal Upload:/:/usr/bin/false
systemd-network:x:76:76:systemd Network Management:/:/usr/bin/false
systemd-resolve:x:77:77:systemd Resolver:/:/usr/bin/false
systemd-timesync:x:78:78:systemd Time Synchronization:/:/usr/bin/false
systemd-coredump:x:79:79:systemd Core Dumper:/:/usr/bin/false
uuidd:x:80:80:UUID Generation Daemon User:/dev/null:/usr/bin/false
systemd-oom:x:81:81:systemd Out Of Memory Daemon:/:/usr/bin/false
nobody:x:65534:65534:Unprivileged User:/dev/null:/usr/bin/false
EOF
  cat > /etc/group <<"EOF"
root:x:0:
bin:x:1:daemon
sys:x:2:
kmem:x:3:
tape:x:4:
tty:x:5:
daemon:x:6:
floppy:x:7:
disk:x:8:
lp:x:9:
dialout:x:10:
audio:x:11:
video:x:12:
utmp:x:13:
cdrom:x:15:
adm:x:16:
messagebus:x:18:
input:x:24:
mail:x:34:
kvm:x:61:
systemd-journal:x:23:
systemd-journal-gateway:x:73:
systemd-journal-remote:x:74:
systemd-journal-upload:x:75:
systemd-network:x:76:
systemd-resolve:x:77:
systemd-timesync:x:78:
systemd-coredump:x:79:
uuidd:x:80:
systemd-oom:x:81:
wheel:x:97:
seat:x:98:
users:x:999:
nogroup:x:65534:
EOF
  touch /var/log/{btmp,lastlog,faillog,wtmp}
  chgrp -v utmp /var/log/lastlog; chmod -v 664 /var/log/lastlog; chmod -v 600 /var/log/btmp
  mark ch7-skel
fi

# ---------- helpers ----------
xt(){ local tb; tb=$(ls $1-*.tar.* 2>/dev/null | grep -E "$1-[0-9]" | head -1)
      [ -n "$tb" ] || { echo "!! source for $1 not found"; exit 1; }
      SRCDIR=$(tar tf "$tb" | head -1 | cut -d/ -f1)
      rm -rf "$SRCDIR"; tar xf "$tb"; cd "$SRCDIR"; }
fin(){ cd /sources; rm -rf "$SRCDIR"; }
generic(){ ./configure --prefix=/usr $2; make; make install; }

# ---------- ch 7.7–7.13: chroot temporary tools ----------
if ! stamp ch7-tools; then
  xt gettext;  ./configure --disable-shared; make
    cp -v gettext-tools/src/{msgfmt,msgmerge,xgettext} /usr/bin; fin
  xt bison;    generic bison "--docdir=/usr/share/doc/bison"; fin
  xt perl;     sh Configure -des -Dprefix=/usr -Dvendorprefix=/usr \
                 -Duseshrplib -Dprivlib=/usr/lib/perl5/core_perl \
                 -Darchlib=/usr/lib/perl5/core_perl \
                 -Dsitelib=/usr/lib/perl5/site_perl \
                 -Dsitearch=/usr/lib/perl5/site_perl \
                 -Dvendorlib=/usr/lib/perl5/vendor_perl \
                 -Dvendorarch=/usr/lib/perl5/vendor_perl
               make; make install; fin
  xt Python;   ./configure --prefix=/usr --enable-shared --without-ensurepip; make; make install; fin
  xt texinfo;  generic texinfo; fin
  xt util-linux; mkdir -pv /var/lib/hwclock
    ./configure --libdir=/usr/lib --runstatedir=/run --disable-chfn-chsh \
      --disable-login --disable-nologin --disable-su --disable-setpriv \
      --disable-runuser --disable-pylibmount --disable-static \
      --disable-liblastlog2 --without-python ADJTIME_PATH=/var/lib/hwclock/adjtime
    make; make install; fin
  rm -rf /usr/share/{info,man,doc}/*
  find /usr/{lib,libexec} -name \*.la -delete
  rm -rf /tools
  mark ch7-tools; echo "== chroot temp tools done =="
fi

# ---------- ch 8: the base system ----------
# bld NAME  -> runs the case-recipe once, stamped
bld(){
  stamp sys-$1 && return 0
  echo "==== building $1 ===="
  xt $1
  case $1 in
    man-pages) rm -v man3/crypt* 2>/dev/null||true; make prefix=/usr install;;
    iana-etc)  cp -v services protocols /etc;;
    glibc)
      patch -Np1 -i ../glibc-*-fhs-1.patch
      mkdir build; cd build; echo "rootsbindir=/usr/sbin" > configparms
      ../configure --prefix=/usr --disable-werror --enable-kernel=5.4 \
        --enable-stack-protector=strong --disable-nscd libc_cv_slibdir=/usr/lib
      make; touch /etc/ld.so.conf
      sed '/test-installation/s@$(PERL)@echo not running@' -i ../Makefile
      make install
      sed '/RTLDLIST=/s@/usr@@g' -i /usr/bin/ldd
      make localedata/install-locales -j1 SUPPORTED-LOCALES="en_US.UTF-8/UTF-8 en_SG.UTF-8/UTF-8 C.UTF-8/UTF-8" || \
        localedef -i en_US -f UTF-8 en_US.UTF-8
      cat > /etc/nsswitch.conf <<"EOF"
passwd: files systemd
group: files systemd
shadow: files systemd
hosts: files dns
networks: files
protocols: files
services: files
ethers: files
rpc: files
EOF
      ln -sfv ../usr/share/zoneinfo/Asia/Singapore /etc/localtime 2>/dev/null || true
      cat > /etc/ld.so.conf <<"EOF"
/usr/local/lib
/opt/lib
include /etc/ld.so.conf.d/*.conf
EOF
      mkdir -pv /etc/ld.so.conf.d; cd ..;;
    zlib)      generic zlib; rm -f /usr/lib/libz.a;;
    bzip2)
      sed -i 's@\(ln -s -f \)$(PREFIX)/bin/@\1@' Makefile
      make -f Makefile-libbz2_so; make clean; make; make PREFIX=/usr install
      cp -av libbz2.so.* /usr/lib; ln -sv libbz2.so.1.0.8 /usr/lib/libbz2.so
      cp -v bzip2-shared /usr/bin/bzip2
      for i in /usr/bin/{bzcat,bunzip2}; do ln -sfv bzip2 $i; done
      rm -fv /usr/lib/libbz2.a;;
    xz)        generic xz "--disable-static --docdir=/usr/share/doc/xz";;
    lz4)       make BUILD_STATIC=no PREFIX=/usr; make BUILD_STATIC=no PREFIX=/usr install;;
    zstd)      make prefix=/usr; make prefix=/usr install; rm -v /usr/lib/libzstd.a;;
    file)      generic file;;
    readline)  generic readline "--disable-static --with-curses";;
    m4)        generic m4;;
    bc)        CC=gcc ./configure --prefix=/usr -G -O3 -r; make; make install;;
    flex)      generic flex "--docdir=/usr/share/doc/flex --disable-static"
               ln -sv flex /usr/bin/lex;;
    binutils)
      mkdir build; cd build
      ../configure --prefix=/usr --sysconfdir=/etc --enable-ld=default \
        --enable-plugins --enable-shared --disable-werror --enable-64-bit-bfd \
        --enable-new-dtags --with-system-zlib --enable-default-hash-style=gnu
      make tooldir=/usr; make tooldir=/usr install
      rm -rfv /usr/lib/lib{bfd,ctf,ctf-nobfd,gprofng,opcodes,sframe}.a /usr/share/doc/gprofng 2>/dev/null||true
      cd ..;;
    gmp)       generic gmp "--enable-cxx --disable-static --docdir=/usr/share/doc/gmp";;
    mpfr)      generic mpfr "--disable-static --enable-thread-safe";;
    mpc)       generic mpc "--disable-static";;
    attr)      generic attr "--disable-static --sysconfdir=/etc";;
    acl)       generic acl "--disable-static";;
    libcap)    sed -i '/install -m.*STA/d' libcap/Makefile
               make prefix=/usr lib=lib; make prefix=/usr lib=lib install;;
    libxcrypt) generic libxcrypt "--enable-hashes=strong,glibc --enable-obsolete-api=no --disable-static --disable-failure-tokens";;
    shadow)
      sed -i 's/groups$(EXEEXT) //' src/Makefile.in
      find man -name Makefile.in -exec sed -i 's/groups\.1 / /;s/getspnam\.3 / /;s/passwd\.5 / /' {} \;
      sed -e 's:#ENCRYPT_METHOD DES:ENCRYPT_METHOD YESCRYPT:' \
          -e 's:/var/spool/mail:/var/mail:' -e '/PATH=/{s@/sbin:@@;s@/bin:@@}' \
          -i etc/login.defs
      ./configure --sysconfdir=/etc --disable-static --with-{b,yes}crypt --without-libbsd \
        --with-group-name-max-length=32
      make; make exec_prefix=/usr install; make -C man install-man
      pwconv; grpconv; mkdir -p /etc/default; useradd -D --gid 999 || true;;
    gcc)
      case $(uname -m) in x86_64) sed -e '/m64=/s/lib64/lib/' -i.orig gcc/config/i386/t-linux64;; esac
      mkdir build; cd build
      ../configure --prefix=/usr LD=ld --enable-languages=c,c++ \
        --enable-default-pie --enable-default-ssp --enable-host-pie \
        --disable-multilib --disable-bootstrap --disable-fixincludes --with-system-zlib
      make; make install
      ln -svr /usr/bin/cpp /usr/lib 2>/dev/null||true
      ln -sfv gcc.1 /usr/share/man/man1/cc.1 2>/dev/null||true
      ln -sfv ../../libexec/gcc/$(gcc -dumpmachine)/$(gcc -dumpversion)/liblto_plugin.so /usr/lib/bfd-plugins/ 2>/dev/null||true
      # sanity
      echo 'int main(){}' > dummy.c; cc dummy.c -v -Wl,--verbose &> dummy.log
      readelf -l a.out | grep -q ': /usr/lib/ld-linux' || { echo "!! gcc sanity FAILED — see LFS 8.29"; exit 1; }
      rm dummy.c a.out dummy.log
      mkdir -pv /usr/share/gdb/auto-load/usr/lib
      mv -v /usr/lib/*gdb.py /usr/share/gdb/auto-load/usr/lib 2>/dev/null||true
      cd ..;;
    pkgconf)   generic pkgconf "--disable-static"
               ln -sv pkgconf /usr/bin/pkg-config;;
    ncurses)   ./configure --prefix=/usr --mandir=/usr/share/man --with-shared \
                 --without-debug --without-normal --with-cxx-shared --enable-pc-files \
                 --with-pkg-config-libdir=/usr/lib/pkgconfig
               make; make DESTDIR=$PWD/dest install
               install -vm755 dest/usr/lib/libncursesw.so.6.5 /usr/lib
               rm -v dest/usr/lib/libncursesw.so.6.5
               sed -e 's/^#if.*XOPEN.*$/#if 1/' -i dest/usr/include/curses.h
               cp -av dest/* /
               for lib in ncurses form panel menu; do
                 ln -sfv lib${lib}w.so /usr/lib/lib${lib}.so
                 ln -sfv ${lib}w.pc /usr/lib/pkgconfig/${lib}.pc
               done
               ln -sfv libncursesw.so /usr/lib/libcurses.so;;
    sed|psmisc|grep|gdbm|gperf|expat|less|libpipeline|make|patch|man-db) generic $1;;
    gettext)   generic gettext "--disable-static --docdir=/usr/share/doc/gettext"
               chmod -v 0755 /usr/lib/preloadable_libintl.so;;
    bison)     generic bison "--docdir=/usr/share/doc/bison";;
    bash)      generic bash "--without-bash-malloc --with-installed-readline --docdir=/usr/share/doc/bash"
               ;;
    libtool)   generic libtool; rm -fv /usr/lib/libltdl.a;;
    inetutils) ./configure --prefix=/usr --bindir=/usr/bin --localstatedir=/var \
                 --disable-logger --disable-whois --disable-rcp --disable-rexec \
                 --disable-rlogin --disable-rsh --disable-servers
               make; make install
               mv -v /usr/{,s}bin/ifconfig 2>/dev/null||true;;
    perl)      sh Configure -des -Dprefix=/usr -Dvendorprefix=/usr \
                 -Dprivlib=/usr/lib/perl5/core_perl -Darchlib=/usr/lib/perl5/core_perl \
                 -Dsitelib=/usr/lib/perl5/site_perl -Dsitearch=/usr/lib/perl5/site_perl \
                 -Dvendorlib=/usr/lib/perl5/vendor_perl -Dvendorarch=/usr/lib/perl5/vendor_perl \
                 -Dman1dir=/usr/share/man/man1 -Dman3dir=/usr/share/man/man3 \
                 -Dpager="/usr/bin/less -isR" -Duseshrplib -Dusethreads
               make; make install;;
    XML-Parser) perl Makefile.PL; make; make install;;
    intltool)  sed -i 's:\\\${:\\\$\\{:' intltool-update.in; generic intltool;;
    autoconf)  generic autoconf;;
    automake)  generic automake "--docdir=/usr/share/doc/automake";;
    openssl)   ./config --prefix=/usr --openssldir=/etc/ssl --libdir=lib shared zlib-dynamic
               make; sed -i '/INSTALL_LIBS/s/libcrypto.a libssl.a//' Makefile
               make MANSUFFIX=ssl install;;
    kmod)      mkdir build; cd build
               meson setup --prefix=/usr .. -D manpages=false; ninja; ninja install; cd ..;;
    elfutils)  ./configure --prefix=/usr --disable-debuginfod --enable-libdebuginfod=dummy
               make; make -C libelf install
               install -vm644 config/libelf.pc /usr/lib/pkgconfig
               rm /usr/lib/libelf.a 2>/dev/null||true;;
    libffi)    generic libffi "--disable-static --with-gcc-arch=native";;
    Python)    ./configure --prefix=/usr --enable-shared --with-system-expat --enable-optimizations
               make; make install
               ln -sfv python3 /usr/bin/python;;
    flit_core) pip3 install --no-index --no-build-isolation --find-links . flit_core || \
               python3 -m pip install --no-index --find-links . flit_core;;
    wheel)     pip3 install --no-index --find-links . wheel;;
    setuptools) pip3 install --no-index --find-links . setuptools;;
    ninja)     python3 configure.py --bootstrap
               install -vm755 ninja /usr/bin/;;
    meson)     pip3 install --no-index --find-links . meson || { python3 setup.py install; };;
    coreutils) patch -Np1 -i ../coreutils-*-i18n-*.patch 2>/dev/null||true
               autoreconf -fv 2>/dev/null||true
               FORCE_UNSAFE_CONFIGURE=1 ./configure --prefix=/usr \
                 --enable-no-install-program=kill,uptime
               make; make install
               mv -v /usr/bin/chroot /usr/sbin
               mv -v /usr/share/man/man1/chroot.1 /usr/share/man/man8/chroot.8 2>/dev/null||true
               sed -i 's/"1"/"8"/' /usr/share/man/man8/chroot.8 2>/dev/null||true;;
    diffutils|gawk|findutils) generic $1;;
    groff)     PAGE=A4 generic groff;;
    grub)      unset {C,CPP,CXX,LD}FLAGS
               case $(uname -m) in aarch64) GT=arm64;; *) GT=x86_64;; esac
               ./configure --prefix=/usr --sysconfdir=/etc --disable-efiemu --disable-werror \
                 --with-platform=efi --target=$GT --program-prefix=""
               make; make install
               mv -v /etc/bash_completion.d/grub /usr/share/bash-completion/completions 2>/dev/null||true;;
    gzip)      generic gzip;;
    iproute2)  make NETNS_RUN_DIR=/run/netns; make SBINDIR=/usr/sbin install;;
    kbd)       patch -Np1 -i ../kbd-*-backspace-1.patch 2>/dev/null||true
               sed -i '/RESIZECONS_PROGS=/s/yes/no/' configure
               sed -i 's/resizecons.8 //' docs/man/man8/Makefile.in
               generic kbd "--disable-vlock";;
    texinfo)   generic texinfo;;
    vim)       echo '#define SYS_VIMRC_FILE "/etc/vimrc"' >> src/feature.h
               ./configure --prefix=/usr; make; make install
               ln -sv vim /usr/bin/vi
               cat > /etc/vimrc <<"EOF"
set nocompatible
set backspace=2
set mouse=
syntax on
EOF
               ;;
    MarkupSafe) pip3 install --no-index --find-links . MarkupSafe;;
    Jinja2)    pip3 install --no-index --find-links . Jinja2;;
    systemd)   sed -i -e 's/GROUP="render"/GROUP="video"/' -e 's/GROUP="sgx", //' rules.d/50-udev-default.rules.in
               mkdir build; cd build
               meson setup --prefix=/usr --buildtype=release \
                 -D default-dnssec=no -D firstboot=false -D install-tests=false \
                 -D ldconfig=false -D sysusers=false -D rpmmacrosdir=no \
                 -D homed=disabled -D userdb=false -D man=disabled \
                 -D mode=release -D pamconfdir=no -D dev-kvm-mode=0660 \
                 -D nobody-group=nogroup -D sysupdate=disabled -D ukify=disabled ..
               ninja; ninja install
               systemd-machine-id-setup
               systemctl preset-all 2>/dev/null||true
               cd ..;;
    dbus)      mkdir build; cd build
               meson setup --prefix=/usr --buildtype=release --wrap-mode=nofallback \
                 -D systemd=enabled ..
               ninja; ninja install
               ln -sfv /etc/machine-id /var/lib/dbus; cd ..;;
    procps-ng) generic procps-ng "--docdir=/usr/share/doc/procps-ng --disable-static --disable-kill --with-systemd";;
    util-linux) ./configure --bindir=/usr/bin --libdir=/usr/lib --runstatedir=/run \
                 --sbindir=/usr/sbin --disable-chfn-chsh --disable-login --disable-nologin \
                 --disable-su --disable-setpriv --disable-runuser --disable-pylibmount \
                 --disable-liblastlog2 --disable-static --without-python \
                 ADJTIME_PATH=/var/lib/hwclock/adjtime
               make; make install;;
    e2fsprogs) mkdir -v build; cd build
               ../configure --prefix=/usr --sysconfdir=/etc --enable-elf-shlibs \
                 --disable-libblkid --disable-libuuid --disable-uuidd --disable-fsck
               make; make install
               rm -fv /usr/lib/{libcom_err,libe2p,libext2fs,libss}.a; cd ..;;
    *) generic $1;;
  esac
  fin; mark sys-$1
}

# build order (LFS ch. 8) — libffi/Python before meson/ninja; systemd late
for p in man-pages iana-etc glibc zlib bzip2 xz lz4 zstd file readline m4 bc flex \
         binutils gmp mpfr mpc attr acl libcap libxcrypt shadow gcc pkgconf ncurses \
         sed psmisc gettext bison grep bash libtool gdbm gperf expat inetutils less \
         perl XML-Parser intltool autoconf automake openssl kmod elfutils libffi \
         Python flit_core wheel setuptools ninja meson coreutils diffutils gawk \
         findutils groff grub gzip iproute2 kbd libpipeline make patch texinfo vim \
         MarkupSafe Jinja2 systemd dbus man-db procps-ng util-linux e2fsprogs; do
  bld $p
done

# cleanup (LFS 8.79)
if ! stamp ch8-clean; then
  rm -rf /tmp/{*,.*} 2>/dev/null||true
  find /usr/lib /usr/libexec -name \*.la -delete 2>/dev/null||true
  find /usr -depth -name $(uname -m)-lfs-linux-gnu\* | xargs rm -rf 2>/dev/null||true
  userdel -r tester 2>/dev/null||true
  mark ch8-clean
fi
echo "== 06 complete — run /aurora/scripts/07-system-config.sh =="
