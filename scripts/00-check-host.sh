#!/bin/bash
# AuroraOS 00 — verify host requirements (LFS 12.3 ch. 2.2)
set -e
. "$(dirname "$0")/../config/build.conf"

echo "== AuroraOS host check =="
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }

ver_check(){
  if ! type -p "$2" &>/dev/null; then echo "ERROR: cannot find $2 ($1)"; BAD=1; return; fi
  # no head -n1 before the grep: perl's --version starts with a BLANK line
  v=$("$2" --version 2>&1 | grep -oE '[0-9]+\.[0-9.]+' | head -n1)
  if printf '%s\n' "$3" "$v" | sort --version-sort --check &>/dev/null; then
    printf "OK:    %-10s %s >= %s\n" "$1" "$v" "$3"
  else
    printf "ERROR: %-10s is TOO OLD (%s, need >= %s)\n" "$1" "$v" "$3"; BAD=1
  fi
}

ver_check Coreutils sort     8.1
ver_check Bash      bash     3.2
ver_check Binutils  ld       2.13.1
ver_check Bison     bison    2.7
ver_check Diffutils diff     2.8.1
ver_check Findutils find     4.2.31
ver_check Gawk      gawk     4.0.1
ver_check GCC       gcc      5.2
ver_check "GCC C++" g++      5.2
ver_check Grep      grep     2.5.1a
ver_check Gzip      gzip     1.3.12
ver_check M4        m4       1.4.10
ver_check Make      make     4.0
ver_check Patch     patch    2.5.4
ver_check Perl      perl     5.8.8
ver_check Python    python3  3.4
ver_check Sed       sed      4.1.5
ver_check Tar       tar      1.22
ver_check Texinfo   texi2any 5.0
ver_check Xz        xz       5.0.0

# /bin/sh must be bash
readlink -f /bin/sh | grep -q bash || { echo "ERROR: /bin/sh must link to bash (dpkg-reconfigure dash)"; BAD=1; }
# g++ compile test
echo 'int main(){}' > /tmp/dummy.c && g++ -o /tmp/dummy /tmp/dummy.c \
  && echo "OK:    g++ compiles" || { echo "ERROR: g++ cannot compile"; BAD=1; }
rm -f /tmp/dummy /tmp/dummy.c

nproc_n=$(nproc); mem=$(free -g | awk '/Mem:/{print $2}')
echo "Cores: $nproc_n  RAM: ${mem}G  (want >=4 cores, >=8G)"
[ -z "$BAD" ] && echo "== Host OK — proceed to 01-prepare-disk.sh ==" || { echo "== Fix errors above =="; exit 1; }
