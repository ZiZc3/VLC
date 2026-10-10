#!/usr/bin/env bash
# VLC-PS5's changes to the console's Vulkan driver (patches/mesa/): applied to
# the RADV source tree PS5_Vulkan's tools/build-radv.sh made, built, and
# installed where ps5/build-title.sh links it from. Run after build-radv.sh
# (which puts back the fork's own files when its revision changes).
#
#   bash ps5/radv-patches.sh
#
# patches/mesa/0001: HDR10 output (the framebuffers switched in place, the
# output's dynamic range read back) for src/player.cc.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(dirname "$here")
vk=${PS5_VULKAN:-$HOME/ps5/PS5_Vulkan}
src=$vk/.deps/work/radv-src
build=$vk/.deps/work/radv-build-ps5-release
install=$vk/.deps/native/radv-release
ninja=${NINJA:-$(command -v ninja || echo "$HOME/.local/bin/ninja")}

[[ -d $src && -f $build/build.ninja ]] || { echo "build RADV first: $vk/tools/build-radv.sh release" >&2; exit 2; }
for p in "$repo"/patches/mesa/*.patch; do
    # each file keeps its fork original beside it, so a patch goes on once
    target=$(sed -n 's/^+++ b\///p' "$p" | head -1)
    [[ -f $src/$target.orig-vlcps5 ]] || cp "$src/$target" "$src/$target.orig-vlcps5"
    cp "$src/$target.orig-vlcps5" "$src/$target"
    patch -d "$src" -p1 -s < "$p"
    echo "applied $(basename "$p")"
done
export PATH="$vk/.deps/work/radv-clc-bin:$PATH"
"$ninja" -C "$build" src/amd/vulkan/libvulkan_radeon.a > "$build.vlcps5.log" 2>&1 ||
    { grep -E "error|FAILED" "$build.vlcps5.log" | head -20 >&2; exit 1; }
cp "$build/src/amd/vulkan/libvulkan_radeon.a" "$install/lib/libvulkan_radeon.ps5.a"
printf 'VLC-PS5 patches: %s\n' "$(cd "$repo/patches/mesa" && ls *.patch | tr '\n' ' ')" >> "$install/PROVENANCE.txt"
echo "==> RADV with VLC-PS5's patches in $install"
