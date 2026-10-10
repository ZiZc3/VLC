#!/usr/bin/env bash
# Build VLC for PS5 as a title: dist/<titleId>/ with a signed eboot.bin, the
# fonts, imports.txt and an empty media/ folder. No jailbreak, no whitelist: the
# app keeps everything in its own folder (/app0).
#
#   bash ps5/build-title.sh
#
# Needs: TARGET=ps5 ps5/deps/build-deps.sh and TARGET=ps5 ps5/configure-vlc.sh first.
# The link is PS5_Vulkan's title recipe (tools/radv-link.sh), as WARP, XPSemu,
# QUICK3 and PS5_VulkanTemplate's link-title.sh use it: its CRT, the SDK's libc++
# and platform layer, RADV, then ps5-native-tool converts and signs.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(dirname "$here")
work_root=${VLC_PS5_WORK:-$HOME/ps5/vlc-ps5}
vk=${PS5_VULKAN:-$HOME/ps5/PS5_Vulkan}
sdk=${PS5_PAYLOAD_SDK:-$vk/.deps/native/ps5-payload-sdk}
archive=${RADV_ARCHIVE:-$vk/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
native=$vk/tooling/native
tool=$vk/build/host/ps5-native-tool
param=$here/sce_sys/param.json
prefix=$work_root/prefix-ps5
vlc_src=$work_root/src/vlc-3.0.24
static=$work_root/static-ps5
work=$work_root/build-title
export PS5_CLANG=${PS5_CLANG:-$(command -v clang || command -v clang-18)}

for file in "$archive" "$tool" "$param" "$sdk/bin/prospero-lld" "$vk/runtime/libc.prx" \
        "$work_root/build-vlc-ps5/src/.libs/libvlccore.a"; do
    [[ -e $file ]] || { echo "missing $file" >&2; exit 2; }
done
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vk/tooling/prospero-clang18" "$@"; }

rm -rf "$work"
mkdir -p "$work/obj" "$work/stubs"

# 1. The plugin table and the library list, read with the PS5 toolchain's nm.
#    Plus our own plugins (ps5/modules): ps5http, http(s) through the console.
TARGET=ps5 NM="$sdk/bin/prospero-nm" APP_MODULES="vlc_entry__ps5http"     bash "$here/gen-static-modules.sh" "$static"

# 2. Our objects, in an archive so --exclude-libs=ALL keeps them out of the
#    title's exports (the converter refuses application exports).
bash "$here/compile-shaders.sh"
cflags=(-O2 -march=znver2 -Wall -Wno-unused-result -Wno-unused-function
    -DVK_NO_PROTOTYPES -DIMGUI_IMPL_VULKAN_USE_VOLK
    -I"$repo/src" -I"$repo/third_party/imgui" -I"$repo/third_party/volk" -I"$repo/third_party/stb"
    -I"$vk/.deps/native/radv-release/include" -I"$vlc_src/include" -I"$prefix/include")
cxxflags=(-std=c++17 -fno-exceptions -fno-rtti -Wno-missing-field-initializers)
cc "${cflags[@]}" -c "$static/static_modules.c" -o "$work/obj/static_modules.o"
cc "${cflags[@]}" -std=c11 -c "$repo/third_party/volk/volk.c" -o "$work/obj/volk.o"
cc "${cflags[@]}" -std=gnu11 -c "$repo/src/platform_ps5.c" -o "$work/obj/platform_ps5.o"
cc "${cflags[@]}" -std=gnu11 -c "$here/modules/ps5http.c" -o "$work/obj/ps5http.o"
for f in imgui imgui_draw imgui_tables imgui_widgets; do
    cc "${cflags[@]}" "${cxxflags[@]}" -c "$repo/third_party/imgui/$f.cpp" -o "$work/obj/$f.o"
done
cc "${cflags[@]}" "${cxxflags[@]}" -c "$repo/third_party/imgui/backends/imgui_impl_vulkan.cpp" \
    -o "$work/obj/imgui_impl_vulkan.o"
for f in gfx image prefs player subconv library network osub lang web ui main; do
    cc "${cflags[@]}" "${cxxflags[@]}" -c "$repo/src/$f.cc" -o "$work/obj/$f.o"
done
"$sdk/bin/prospero-ar" rcs "$work/libprobe.a" "$work"/obj/*.o

cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$work/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_cpp_runtime.cpp" -o "$work/app_cpp_runtime.o"

stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vk/$source" -o "$work/${library}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/stubs/${library}.so" "$work/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# 3. VLC's archives for lld: -lfoo becomes our prefix's libfoo.a; the system
#    libraries (pthread, dl, atomic, ...) are the SDK's .so stubs, linked below.
vlc_inputs=()
while read -r item; do
    case $item in
    -Wl,--start-group | -Wl,--end-group | -L*) ;;
    # The RADV archive (linked whole) carries its own zlib: that one serves us too.
    -lz) ;;
    -l*)
        lib=$prefix/lib/lib${item#-l}.a
        if [[ -f $lib ]]; then vlc_inputs+=("$lib"); else echo "  (system) $item"; fi
        ;;
    *) vlc_inputs+=("$item") ;;
    esac
done < "$static/link.rsp"
vlc_inputs+=("$prefix/lib/libps5compat.a")
# GNU libiconv: subtitle files that aren't UTF-8 (src/subconv.cc)
vlc_inputs+=("$prefix/lib/libiconv.a")

# 4. Link.
# shellcheck source=/dev/null
source "$vk/tools/radv-link.sh"
radv_link_recipe "$vk" "$sdk" "$archive" || exit 2
# Name lookups: the recipe binds getaddrinfo to the platform's stand-in, which
# refuses every lookup (numeric addresses too); ours (ps5/compat/vlc_resolver.c)
# answers numbers, mDNS and DNS. Swap the binding.
for i in "${!radv_link_flags[@]}"; do
    case ${radv_link_flags[$i]} in
    --defsym=getaddrinfo=ps5_getaddrinfo) radv_link_flags[$i]=--defsym=getaddrinfo=vlcps5_getaddrinfo ;;
    --defsym=freeaddrinfo=ps5_freeaddrinfo) radv_link_flags[$i]=--defsym=freeaddrinfo=vlcps5_freeaddrinfo ;;
    esac
done
radv_link_flags+=(--wrap=mmap --wrap=munmap)       # anonymous memory (ps5/compat/mmap.c)
radv_link_flags+=(--wrap=geteuid --wrap=getegid)   # real IDs (ps5/compat/libc_stubs.c)
# libc functions the console leaves unresolved for a title, bound to
# ps5/compat/libc_missing.c and kept local (no exports), as XPSemu does.
mapfile -t missing_libc < <(grep -oE 'xemu_ps5_[a-z_0-9]+\(' "$here/compat/libc_missing.c" |
    sed -e 's/^xemu_ps5_//' -e 's/($//' | sort -u)
{
    printf '{\n    local:\n'
    for name in "${missing_libc[@]}"; do
        radv_link_flags+=("--defsym=$name=xemu_ps5_$name")
        printf '        %s;\n' "$name"
    done
    # stdio's macros take the real functions (ps5/compat/vlc_missing.c).
    radv_link_flags+=("--defsym=__isthreaded=vlc_ps5_isthreaded")
    printf '        __isthreaded;\n'
    printf '};\n'
} > "$work/libc-missing-local.map"
radv_link_flags+=(--version-script "$work/libc-missing-local.map")
# localeconv from the platform layer, as PS5_VulkanTemplate's link-title.sh binds it.
if "$sdk/bin/llvm-nm" --defined-only "$sdk/target/lib/libps5platform.a" 2>/dev/null |
        grep -q " T ps5_localeconv$" &&
        [[ " ${radv_link_flags[*]} " != *" --defsym=localeconv=ps5_localeconv "* ]]; then
    printf '{\n    local:\n        localeconv;\n};\n' > "$work/localeconv-local.map"
    radv_link_flags+=(--defsym=localeconv=ps5_localeconv --version-script "$work/localeconv-local.map")
fi

# -u vlc_static_modules: libvlccore names the plugin table only weakly
# (src/modules/bank.c), and a weak reference pulls nothing out of an archive:
# without it the title links with no plugins at all.
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --error-limit=0 \
    --eh-frame-hdr --no-dynamic-linker -z nodynamic-undefined-weak "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL \
    -e _start -o "$work/llvm-pie.elf" \
    -u vlc_static_modules \
    "$work/app_crt.o" "$work/app_cpp_runtime.o" \
    --start-group "$work/libprobe.a" "${vlc_inputs[@]}" --end-group \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so 2>&1 | tee "$work/link.log" | grep -E "error|warning" | head -60
[[ ${PIPESTATUS[0]} -eq 0 && -f $work/llvm-pie.elf ]] || { echo "link failed: $work/link.log" >&2; exit 1; }
# Guard: the plugins really are in (vmem's description is a string only it has).
grep -aq "Video memory output" "$work/llvm-pie.elf" ||
    { echo "no VLC plugins in the title: vlc_static_modules wasn't linked" >&2; exit 1; }

"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf

# 5. The title folder.
app=$repo/dist/$title_id
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module" "$app/media"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$app/eboot.bin" > /dev/null
cp "$param" "$app/sce_sys/param.json"
for asset in icon0.png pic0.dds pic1.dds; do
    if [[ -f $here/sce_sys/$asset ]]; then cp "$here/sce_sys/$asset" "$app/sce_sys/$asset"
    elif [[ -f $vk/sce_sys/$asset ]]; then cp "$vk/sce_sys/$asset" "$app/sce_sys/$asset"; fi
done
(cd "$vk/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vk/runtime/libc.prx" "$app/sce_module/libc.prx"
# Every GOT slot and its symbol, for the startup import report.
llvm-readelf-18 -r --wide "$work/llvm-pie.elf" |
    awk '$3 == "R_X86_64_GLOB_DAT" || $3 == "R_X86_64_JUMP_SLOT" { print $1, $5 }' \
    > "$app/imports.txt"
# Fonts: the interface's, and VLC's subtitle fonts (patches/0001's paths).
mkdir -p "$app/fonts"
cp "$repo/assets/fonts/"Inter-*.ttf "$repo/assets/fonts/Inter-OFL.txt" "$app/fonts/"
# The interface: Selawik (Microsoft's open Segoe UI-like font, OFL), the classic look.
cp "$repo/assets/fonts/"Selawik-*.ttf "$repo/assets/fonts/Selawik-OFL.txt" "$app/fonts/"
# The text viewer (.txt, .nfo art): DejaVu Sans Mono, box drawing and shades included.
cp "$repo/assets/fonts/DejaVuSansMono.ttf" "$repo/assets/fonts/DejaVu-LICENSE.txt" "$app/fonts/"
mkdir -p "$app/assets"
cp "$repo/assets/vlc-logo.png" "$app/assets/"
# The interface languages (src/lang.cc)
mkdir -p "$app/assets/lang"
cp "$repo/assets/lang/"*.txt "$app/assets/lang/"
# The first run's pictures of the two looks (src/ui.cc, setup)
mkdir -p "$app/assets/setup"
cp "$repo/assets/setup/"*.png "$app/assets/setup/"
cp "$repo/assets/fonts/Inter-SemiBold.ttf" "$app/fonts/subtitle.ttf"
cp "$repo/assets/fonts/Inter-Regular.ttf" "$app/fonts/mono.ttf"
# Noto fonts for the scripts Inter lacks (Arabic, Hebrew, CJK, Thai, ...):
# subtitles (patches/0005) and file names in the interface.
mkdir -p "$app/fonts/fallback"
cp "$repo/assets/fonts/fallback/"* "$app/fonts/fallback/"
# HTTPS: Mozilla's CA list (curl.se/ca), where GnuTLS looks for it (/app0/certs).
mkdir -p "$app/certs"
cp "$repo/assets/certs/cacert.pem" "$app/certs/"
# Two demo clips, so the first start has something to play (WITH_DEMO=0: none).
if [[ ${WITH_DEMO:-1} == 1 ]]; then
    [[ -f $work_root/media/sintel-h264-aac.mp4 ]] &&
        cp "$work_root/media/sintel-h264-aac.mp4" "$app/media/Sintel trailer.mp4"
    [[ -f $work_root/media/hevc10.mp4 ]] &&
        cp "$work_root/media/hevc10.mp4" "$app/media/HEVC 10-bit test.mp4"
fi
sha=$(sha256sum "$app/eboot.bin" | cut -c1-12)
mkdir -p "$repo/builds"
cp "$work/llvm-pie.elf" "$repo/builds/llvm-pie-$sha.elf"
# Only the newest three are kept (~140 MB each): enough to read a crash from
# the build on the console and the two before it.
ls -t "$repo"/builds/llvm-pie-*.elf | tail -n +4 | xargs -r rm -f --
printf '==> %s: %s (eboot.bin %s bytes, %s; %s imports)\n' "$title_id" "$app" \
    "$(stat -c %s "$app/eboot.bin")" "$sha" "$(wc -l < "$app/imports.txt")"
