#!/bin/bash
# DaybreakOS 19 — Flatpak (the route to Steam + thousands of desktop apps).
# Run INSIDE the chroot with /sources/extras populated (see tarball list below).
#
# Why flatpak and not dpkg/apt: Debian packages target Debian's exact
# glibc/layout and would overwrite the LFS base; flatpak apps bring their own
# runtime (incl. the i386 compat Steam needs on our pure-64-bit system).
#
# Tarballs: curl-8.12.1, libseccomp-2.6.0, yaml-0.2.5, json-glib-1.10.6,
# libxmlb-0.3.22, libxslt-1.1.42, AppStream-1.0.4, libassuan-3.0.2,
# gpgme-1.24.2, fuse-3.16.2, libarchive-3.7.7, libostree-2024.10,
# bubblewrap-0.11.0, xdg-dbus-proxy-0.1.6, flatpak-1.16.1
# Plus: pyparsing wheel (pip3 install --no-index) for flatpak's
# variant-schema-compiler; fetch on the host, docker cp in (no net in chroot).
#
# Kernel: needs CONFIG_FUSE_FS=y (portals/revokefs) — rebuild if unset.
# CONFIG_USER_NS + CONFIG_SECCOMP_FILTER were already =y (unprivileged bwrap).
set -e
cd /sources/extras
export MAKEFLAGS="-j$(nproc)"

xt(){ D=$(tar tf "$1" | sed 's|^\./||' | awk 'NF' | head -1 | cut -d/ -f1)
      rm -rf "$D"; tar xf "$1"; cd "$D"; }
fin(){ cd /sources/extras; rm -rf "$D"; }

# stub itstool: AppStream only uses it to join .mo translations into its
# metainfo XML; we ship untranslated, so copy -j IN to -o OUT.
cat > /usr/bin/itstool <<'PYEOF'
#!/usr/bin/env python3
import shutil, sys
args = sys.argv[1:]; src = out = None; i = 0
while i < len(args):
    if args[i] == "-j" and i + 1 < len(args): src = args[i+1]; i += 2
    elif args[i] == "-o" and i + 1 < len(args): out = args[i+1]; i += 2
    else: i += 1
if src and out: shutil.copyfile(src, out); sys.exit(0)
sys.exit("stub itstool: only '-j IN -o OUT' is supported")
PYEOF
chmod 755 /usr/bin/itstool

CA=$(ls /etc/ssl/cert.pem /etc/ssl/certs/ca-bundle.crt 2>/dev/null | head -1)
xt curl-8.12.1.tar.xz
./configure --prefix=/usr --with-openssl --without-libpsl --disable-static \
            ${CA:+--with-ca-bundle=$CA}
make && make install; fin

xt libseccomp-2.6.0.tar.gz
./configure --prefix=/usr --disable-static; make && make install; fin

xt yaml-0.2.5.tar.gz
./configure --prefix=/usr --disable-static; make && make install; fin

xt json-glib-1.10.6.tar.xz; mkdir b; cd b
meson setup --prefix=/usr --buildtype=release -Dintrospection=disabled \
      -Dgtk_doc=disabled -Dtests=false ..
ninja && ninja install; cd ..; fin

xt libxmlb-0.3.22.tar.xz; mkdir b; cd b
meson setup --prefix=/usr --buildtype=release -Dintrospection=false \
      -Dgtkdoc=false -Dtests=false -Dstemmer=false ..
ninja && ninja install; cd ..; fin

xt libxslt-1.1.42.tar.xz
./configure --prefix=/usr --disable-static --without-python
make && make install; fin

xt AppStream-1.0.4.tar.xz
# no docbook-xsl in the image and no meson switch for man pages: drop docs/
sed -i "s#subdir('docs/')##" meson.build
mkdir b; cd b
meson setup --prefix=/usr --buildtype=release -Dstemming=false -Dgir=false \
      -Dapidocs=false -Ddocs=false -Dsystemd=false -Dcompose=false ..
ninja && ninja install; cd ..; fin

xt libassuan-3.0.2.tar.bz2
./configure --prefix=/usr; make && make install; fin

xt gpgme-1.24.2.tar.bz2
./configure --prefix=/usr --enable-languages=   # bindings off; C lib always built
make -C src && make -C src install              # tests/ needs a gpg binary we don't ship
fin

xt fuse-3.16.2.tar.gz; mkdir b; cd b
meson setup --prefix=/usr --buildtype=release -Dexamples=false -Dtests=false ..
ninja && ninja install; chmod u+s /usr/bin/fusermount3; cd ..; fin

xt libarchive-3.7.7.tar.xz
./configure --prefix=/usr --disable-static --without-expat
make && make install; fin

xt libostree-2024.10.tar.xz
./configure --prefix=/usr --with-curl --without-soup3 --with-openssl \
            --disable-gtk-doc --without-selinux --without-avahi --disable-man
make && make install; fin

xt bubblewrap-0.11.0.tar.xz; mkdir b; cd b
meson setup --prefix=/usr --buildtype=release -Dman=disabled -Dtests=false ..
ninja && ninja install; cd ..; fin

xt xdg-dbus-proxy-0.1.6.tar.xz; mkdir b; cd b
meson setup --prefix=/usr --buildtype=release -Dman=disabled -Dtests=false ..
ninja && ninja install; cd ..; fin

xt flatpak-1.16.1.tar.xz; mkdir b; cd b
meson setup --prefix=/usr --buildtype=release \
      -Dsystem_helper=disabled -Dselinux_module=disabled \
      -Dman=disabled -Ddocbook_docs=disabled -Dgir=disabled \
      -Dtests=false -Dinstalled_tests=false \
      -Dsystem_bubblewrap=bwrap -Dsystem_dbus_proxy=xdg-dbus-proxy ..
ninja && ninja install; cd ..; fin

# Flathub, system-wide. --no-gpg-verify because we ship no GnuPG binary;
# transport integrity comes from HTTPS (curl + CA bundle).
flatpak remote-add --if-not-exists --no-gpg-verify \
        flathub https://dl.flathub.org/repo/ || true

flatpak --version
echo "== 19 complete — flatpak ready. Steam: sudo flatpak install flathub com.valvesoftware.Steam =="
