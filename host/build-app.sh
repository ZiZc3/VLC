#!/usr/bin/env bash
# Build the VLC-PS5 app for Linux (the test build: offscreen Vulkan, scripted pad).
#   bash host/build-app.sh      -> $VLC_PS5_WORK/out/vlc-ps5-host
#   VLCPS5_ASAN=1 bash host/build-app.sh -> out/vlc-ps5-host-asan (AddressSanitizer +
#   UndefinedBehaviorSanitizer on our code, for stress tests)
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(dirname "$here")
work=${VLC_PS5_WORK:-$HOME/ps5/vlc-ps5}
vk=${PS5_VULKAN:-$HOME/ps5/PS5_Vulkan}
static=$work/static-host
prefix=$work/prefix-host
obj=$work/obj-host
out=$work/out
name=vlc-ps5-host
san=()
if [[ ${VLCPS5_ASAN:-0} == 1 ]]; then
    obj=$work/obj-host-asan
    name=vlc-ps5-host-asan
    san=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
fi
mkdir -p "$obj" "$out"

bash "$repo/ps5/compile-shaders.sh"
TARGET=host bash "$repo/ps5/gen-static-modules.sh" "$static" >/dev/null

inc=(-I"$repo/src" -I"$repo/third_party/imgui" -I"$repo/third_party/volk" -I"$repo/third_party/stb"
    -I"$vk/.deps/native/radv-release/include" -I"$work/src/vlc-3.0.24/include" -I"$prefix/include")
defs=(-DVK_NO_PROTOTYPES -DIMGUI_IMPL_VULKAN_USE_VOLK -DVLCPS5_HOST)
cflags=(-O2 -g -Wall -Wno-unused-function "${san[@]}" "${defs[@]}" "${inc[@]}")
# the newest of our headers: a file is built again when one changed (a struct
# that grew, a default argument) not only when the file itself did
newest_h=$(ls -t "$repo"/src/*.h "$repo"/src/gen/*.h | head -1)

objs=()
compile() { # source compiler [flags...]
    local src=$1 cc=$2
    shift 2
    local o=$obj/$(basename "${src%.*}").o
    if [[ ! -f $o || $src -nt $o || $repo/src/gen/video_frag.h -nt $o || $newest_h -nt $o ]]; then
        "$cc" "${cflags[@]}" "$@" -c "$src" -o "$o"
    fi
    objs+=("$o")
}
for f in imgui imgui_draw imgui_tables imgui_widgets; do
    compile "$repo/third_party/imgui/$f.cpp" g++ -std=c++17
done
compile "$repo/third_party/imgui/backends/imgui_impl_vulkan.cpp" g++ -std=c++17
compile "$repo/third_party/volk/volk.c" gcc -std=c11
compile "$repo/src/platform_host.c" gcc -std=gnu11
for f in gfx image prefs player subconv library network osub lang web ui main; do
    compile "$repo/src/$f.cc" g++ -std=c++17 -Wno-missing-field-initializers
done
gcc -O2 -c "$static/static_modules.c" -o "$obj/static_modules.o"
objs+=("$obj/static_modules.o")
# VLCPS5_OWN_TSEARCH=1: the console's tree functions (ps5/compat/vlc_search.c)
# instead of glibc's, to run VLC on them here.
if [[ ${VLCPS5_OWN_TSEARCH:-0} == 1 ]]; then
    gcc -O2 -c "$repo/ps5/compat/vlc_search.c" -o "$obj/vlc_search.o"
    objs+=("$obj/vlc_search.o")
fi
# VLCPS5_OWN_IO=1: the console's readv/writev (ps5/compat/libc_missing.c), so
# VLC's file reading runs on them here.
io_flags=()
if [[ ${VLCPS5_OWN_IO:-0} == 1 ]]; then
    gcc -O2 -w -c "$repo/ps5/compat/libc_missing.c" -o "$obj/libc_missing.o"
    objs+=("$obj/libc_missing.o")
    io_flags=(-Wl,--defsym=readv=xemu_ps5_readv -Wl,--defsym=writev=xemu_ps5_writev)
fi
g++ "${san[@]}" -o "$out/$name" "${objs[@]}" @"$static/link.rsp" "$prefix/lib/libiconv.a" -lpthread -ldl "${io_flags[@]}"
echo "built $out/$name"
