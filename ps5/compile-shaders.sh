#!/usr/bin/env bash
# GLSL in shaders/ -> SPIR-V arrays in src/gen/ (spv_<name>_<stage>).
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$repo/src/gen"
for s in "$repo"/shaders/*.vert "$repo"/shaders/*.frag; do
    name=$(basename "$s" | tr . _)
    out=$repo/src/gen/$name.h
    [[ -f $out && $out -nt $s ]] && continue
    glslangValidator -V --target-env vulkan1.1 --vn "spv_$name" "$s" -o "$out" >/dev/null
done
