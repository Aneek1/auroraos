#!/bin/bash
# AuroraOS 02 — fetch all sources: LFS official list + AuroraOS extras
set -e
. "$(dirname "$0")/../config/build.conf"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
# LFS mirrors stall; don't hang 15 min (wget default) on a dead connection
WGET="wget -nc --timeout=30 --tries=5"

mkdir -pv "$LFS/sources"
chmod -v a+wt "$LFS/sources"
cd "$LFS/sources"

echo "== LFS $LFS_VERSION source list =="
$WGET "$LFS_BOOK/wget-list-systemd"
$WGET "$LFS_BOOK/md5sums"
$WGET --input-file=wget-list-systemd --continue --directory-prefix="$LFS/sources"

echo "== verifying md5sums =="
md5sum -c md5sums 2>&1 | grep -v ': OK$' || true
if md5sum -c md5sums >/dev/null 2>&1; then echo "all LFS sources OK"; else
  echo "!! some sources failed verification — delete the bad files and re-run"; exit 1; fi

echo "== AuroraOS extras (Wayland kiosk stack + Firefox binary) =="
mkdir -p extras && cd extras
grep -vE '^#|^$' "$REPO/config/extras.list" | while read -r url; do
  $WGET "$url"
done
FF_URL="https://download-installer.cdn.mozilla.net/pub/firefox/releases/${FIREFOX_VERSION}/${FIREFOX_PLATFORM}/en-US/firefox-${FIREFOX_VERSION}.tar.xz"
if curl -sfIL "$FF_URL" >/dev/null; then
  $WGET "$FF_URL"
else
  echo "!! No Firefox ${FIREFOX_VERSION} build for ${FIREFOX_PLATFORM} on Mozilla CDN."
  echo "!! Check https://ftp.mozilla.org/pub/firefox/releases/ for a version with a"
  echo "!! ${FIREFOX_PLATFORM} directory and set FIREFOX_VERSION in config/build.conf."
  exit 1
fi

echo "== done — proceed to 03-toolchain-pass1.sh =="
