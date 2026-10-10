#!/usr/bin/env bash
# Configure and build libvlc 3.0.24 with every plugin linked in statically.
#
#   TARGET=host ps5/configure-vlc.sh   (World 1)
#   TARGET=ps5  ps5/configure-vlc.sh   (World 3)
#
# Only the libraries from ps5/deps/build-deps.sh are visible (PKG_CONFIG_LIBDIR),
# and everything else is switched off by name, so the host build has the same
# plugin set the PS5 build will have.
set -euo pipefail

target=${TARGET:-host}
work=${VLC_PS5_WORK:-$HOME/ps5/vlc-ps5}
vlc=$work/src/vlc-3.0.24
prefix=$work/prefix-$target
build=$work/build-vlc-$target
export PATH=$work/tools/bin:$PATH
export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig
export PKG_CONFIG_PATH=

case $target in
host)
    export CC=gcc CXX=g++
    cross=()
    cflags="-O2 -fPIC"
    ;;
ps5)
    sdk=${PS5_PAYLOAD_SDK:-$HOME/ps5/PS5_Vulkan/.deps/native/ps5-payload-sdk}
    export PS5_PAYLOAD_SDK=$sdk
    export CC=$sdk/bin/prospero-clang CXX=$sdk/bin/prospero-clang++ OBJC=$sdk/bin/prospero-clang
    export AR=$sdk/bin/prospero-ar RANLIB=$sdk/bin/prospero-ranlib NM=$sdk/bin/prospero-nm
    export STRIP=$sdk/bin/prospero-strip
    export PKG_CONFIG="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/pkg-config" VLC_PS5_PREFIX=$prefix
    export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig:$prefix/libdata/pkgconfig
    cross=(--host=x86_64-pc-freebsd --build=x86_64-pc-linux-gnu
        # Declared by the SDK's headers but missing from its libc, so configure's link
        # test fails and VLC's fallback macros clash with the declarations. The title
        # link binds it to the SDK platform layer's ps5_if_nameindex (radv-link.sh).
        ac_cv_func_if_nameindex=yes
        # No stack protector: VLC appends -fstack-protector-strong after our flags,
        # and no title proven on the console imports __stack_chk_guard yet. FFmpeg
        # and the other libraries don't use it either.
        ax_cv_check_cflags___fstack_protector_strong=no
        ax_cv_check_cxxflags___fstack_protector_strong=no)
    cflags="-O2 -march=znver2 -fPIC"
    ;;
esac

# VLC-PS5's own changes to VLC (patches/), applied once.
for p in "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"/patches/*.patch; do
    if patch -d "$vlc" -p1 -s -N --dry-run < "$p" >/dev/null 2>&1; then
        patch -d "$vlc" -p1 -s -N < "$p"
        echo "applied $(basename "$p")"
    fi
done

off=(
    # front ends, scripting, services
    nls dbus qt skins2 ncurses lirc lua vlm addonmanagermodules sout vlc
    update-check notify secret kwallet avahi udev mtp microdns
    # access / network
    live555 smbclient dsm sftp nfs v4l2 vcd
    libcddb screen vnc freerdp srt librist decklink linsys dc1394 dv1394 libgcrypt
    # demux / codecs covered by FFmpeg, or not wanted yet
    gme sid shout mod mpc mad mpg123 faad aom vpx a52 dca flac libmpeg2
    vorbis tremor speex theora oggspots kate tiger png jpeg bpg x262 x264 x26410b
    x265 fluidsynth fluidlite zvbi aribsub aribb25 spatialaudio postproc libva crystalhd
    shine twolame fdkaac taglib chromaprint chromecast
    # video / audio outputs and visualisations (the app draws and plays itself)
    xcb xvideo vdpau wayland gles2 sdl-image svg svgdec caca aa evas mmal
    pulse alsa oss sndio jack samplerate soxr goom projectm vsxu libplacebo
    # text: FreeType with fribidi + harfbuzz (every script), libass for styled ASS; no fontconfig
    fontconfig
)
args=()
for f in "${off[@]}"; do args+=("--disable-$f"); done

mkdir -p "$build"
cd "$build"
"$vlc/configure" "${cross[@]}" --prefix="$prefix" \
    --enable-static --disable-shared --disable-rpath \
    --enable-avcodec --enable-avformat --enable-swscale --enable-dav1d \
    --enable-matroska --enable-gnutls --enable-smb2 --enable-upnp --enable-libxml2 --enable-libass --enable-dvdread --enable-dvdnav --enable-bluray --enable-archive --enable-freetype --enable-fribidi --enable-harfbuzz --enable-dvbpsi --enable-opus --enable-ogg \
    CFLAGS="$cflags -I$prefix/include" CXXFLAGS="$cflags -I$prefix/include" \
    LDFLAGS="-L$prefix/lib" \
    "${args[@]}"
make -j"$(nproc)"
