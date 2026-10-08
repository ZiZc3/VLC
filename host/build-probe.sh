#!/usr/bin/env bash
# World 1: build vlc_probe against the static libvlc from ps5/configure-vlc.sh (TARGET=host).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=${VLC_PS5_WORK:-$HOME/ps5/vlc-ps5}
static=$work/static-host
prefix=$work/prefix-host
out=$work/out
mkdir -p "$out"

TARGET=host bash "$here/../ps5/gen-static-modules.sh" "$static"
gcc -O2 -c "$static/static_modules.c" -o "$static/static_modules.o"
gcc -O2 -Wall -I"$work/src/vlc-3.0.24/include" -I"$prefix/include" \
    -c "$here/vlc_probe.c" -o "$out/vlc_probe.o"
# g++ drives the link: libmatroska and libebml are C++.
g++ -o "$out/vlc_probe" "$out/vlc_probe.o" "$static/static_modules.o" @"$static/link.rsp" -lpthread
ls -la "$out/vlc_probe"
