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
# ftp.gnu.org rate-limits bulk downloads; ftpmirror.gnu.org is GNU's own
# redirector to a nearby mirror (md5sums still verifies every file)
sed -e 's|ftp.gnu.org/gnu/|ftpmirror.gnu.org/gnu/|' \
    -e 's|https://prdownloads.sourceforge.net/expat/expat-2.6.4.tar.xz|https://github.com/libexpat/libexpat/releases/download/R_2_6_4/expat-2.6.4.tar.xz|' \
    wget-list-systemd > wget-list-mirrored
$WGET --input-file=wget-list-mirrored --continue --directory-prefix="$LFS/sources"

echo "== verifying md5sums =="
md5sum -c md5sums 2>&1 | grep -v ': OK$' || true
if md5sum -c md5sums >/dev/null 2>&1; then echo "all LFS sources OK"; else
  echo "!! some sources failed verification — delete the bad files and re-run"; exit 1; fi

echo "== AuroraOS extras (Wayland kiosk stack + Firefox binary) =="
mkdir -p extras && cd extras
# --content-disposition so GitHub/codeberg /archive/ urls save with their proper
# repo-prefixed name (e.g. labwc-0.8.4.tar.gz), not the bare tag (0.8.4.tar.gz)
# that the meson build globs would miss.
grep -vE '^#|^$' "$REPO/config/extras.list" | while read -r url; do
  $WGET --content-disposition "$url"
done
# SourceForge mirror redirects (freetype, libpng) append a ?viasf=... query the
# build globs would miss; strip it so the tarball has its plain name.
for f in *\?*; do [ -e "$f" ] && mv -n "$f" "${f%%\?*}"; done
FF_URL="https://download-installer.cdn.mozilla.net/pub/firefox/releases/${FIREFOX_VERSION}/${FIREFOX_PLATFORM}/en-US/firefox-${FIREFOX_VERSION}.tar.xz"
if curl -sfIL "$FF_URL" >/dev/null; then
  $WGET "$FF_URL"
else
  echo "!! No Firefox ${FIREFOX_VERSION} build for ${FIREFOX_PLATFORM} on Mozilla CDN."
  echo "!! Check https://ftp.mozilla.org/pub/firefox/releases/ for a version with a"
  echo "!! ${FIREFOX_PLATFORM} directory and set FIREFOX_VERSION in config/build.conf."
  exit 1
fi

# ---- Aura LLM model (bundled, offline-first) ----
AURA_MODEL_DIR="$LFS/opt/aura/models"
AURA_MODEL="Llama-3.2-1B-Instruct-Q4_K_M.gguf"
AURA_MODEL_URL="https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/${AURA_MODEL}"
mkdir -p "$AURA_MODEL_DIR"
if [ ! -f "$AURA_MODEL_DIR/$AURA_MODEL" ]; then
  echo "==== downloading Aura model ($AURA_MODEL, ~0.8 GB) ===="
  wget --timeout=30 --tries=5 -O "$AURA_MODEL_DIR/$AURA_MODEL" "$AURA_MODEL_URL"
fi
# Integrity: GGUF files start with the ASCII magic "GGUF"; reject truncated/HTML error pages.
if [ "$(head -c 4 "$AURA_MODEL_DIR/$AURA_MODEL")" != "GGUF" ]; then
  echo "!! Aura model is not a valid GGUF (download failed?). Removing."; rm -f "$AURA_MODEL_DIR/$AURA_MODEL"; exit 1
fi
sz=$(stat -c%s "$AURA_MODEL_DIR/$AURA_MODEL")
[ "$sz" -gt 500000000 ] || { echo "!! Aura model too small ($sz bytes) — incomplete."; exit 1; }
echo "Aura model OK ($sz bytes)."

echo "== done — proceed to 03-toolchain-pass1.sh =="
