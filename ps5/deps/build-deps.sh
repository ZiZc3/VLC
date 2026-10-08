#!/usr/bin/env bash
# Build VLC-PS5's libraries from source as static archives into one prefix.
#
#   TARGET=host ps5/deps/build-deps.sh [recipe...]   (World 1: Linux proof)
#   TARGET=ps5  ps5/deps/build-deps.sh [recipe...]   (World 2: PS5 cross build)
#
# Same recipes for both targets, so what World 1 proves on Linux is what World 2
# cross-compiles. Versions follow VLC 3.0.24's own contrib/src/*/rules.mak.
# No recipe lists the system's libraries: the PS5 has none of them either.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
target=${TARGET:-host}
work=${VLC_PS5_WORK:-$HOME/ps5/vlc-ps5}
dl=$work/dl
src=$work/src
prefix=$work/prefix-$target
tools=$work/tools
jobs=$(nproc)
mkdir -p "$dl" "$src" "$prefix" "$tools" "$work/logs"
export PATH=$tools/bin:$PATH
export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig
export PKG_CONFIG_PATH=

case $target in
host)
    export CC=gcc CXX=g++ AR=ar RANLIB=ranlib
    host_triple=
    cflags="-O2 -fPIC"
    ;;
ps5)
    # PS5_Vulkan's pinned payload SDK, as XPSemu and Ruffle-Flash-PS5 use it.
    sdk=${PS5_PAYLOAD_SDK:-$HOME/ps5/PS5_Vulkan/.deps/native/ps5-payload-sdk}
    export PS5_PAYLOAD_SDK=$sdk
    export CC=$sdk/bin/prospero-clang CXX=$sdk/bin/prospero-clang++
    export AR=$sdk/bin/prospero-ar RANLIB=$sdk/bin/prospero-ranlib NM=$sdk/bin/prospero-nm
    export STRIP=$sdk/bin/prospero-strip CHOST=x86_64-pc-freebsd
    # Our pkg-config (the SDK's replaces PKG_CONFIG_LIBDIR with its own).
    export PKG_CONFIG="$(dirname "$here")/pkg-config" VLC_PS5_PREFIX=$prefix
    host_triple=x86_64-pc-freebsd
    cflags="-O2 -march=znver2 -fPIC"
    printf "[constants]\nsdk = '%s'\nps5 = '%s'\n" "$sdk" "$(dirname "$here")" > "$work/sdk.ini"
    meson_cross="--cross-file $work/sdk.ini --cross-file $(dirname "$here")/ps5-cross.ini"
    # The toolchain searches only its sysroot; add our prefix as a second root.
    cmake_cross="-DCMAKE_TOOLCHAIN_FILE=$sdk/toolchain/prospero.cmake -DCMAKE_FIND_ROOT_PATH=$prefix"
    ffmpeg_cross="--enable-cross-compile --target-os=freebsd --arch=x86_64 --cpu=znver2
        --cc=$CC --cxx=$CXX --ar=$AR --ranlib=$RANLIB --nm=$NM --strip=$STRIP
        --pkg-config=$PKG_CONFIG"
    ;;
*)
    echo "unknown TARGET=$target" >&2
    exit 2
    ;;
esac
export CFLAGS=$cflags CXXFLAGS=$cflags

fetch() { # url [file]
    local file=${2:-$(basename "$1")}
    [[ -f $dl/$file ]] && return
    curl -fsSL "$1" -o "$dl/$file.part" || { echo "download failed: $1" >&2; exit 1; }
    mv -f "$dl/$file.part" "$dl/$file"
}

unpack() { # file dir
    rm -rf "${src:?}/$2"
    tar xf "$dl/$1" -C "$src"
}

# nasm: a build tool (FFmpeg's and dav1d's x86 assembly), always for the build machine.
build_nasm() {
    local v=2.16.03
    [[ -x $tools/bin/nasm ]] && return
    fetch "https://www.nasm.us/pub/nasm/releasebuilds/$v/nasm-$v.tar.xz"
    unpack "nasm-$v.tar.xz" "nasm-$v"
    (cd "$src/nasm-$v" && CC=gcc CFLAGS=-O2 ./configure --prefix="$tools" &&
        make -j"$jobs" && make install)
}

# libc pieces the PS5 lacks (from XPSemu's ps5/compat), as libps5compat.a and a
# stub libresolv.a. Linked into the title in World 3; only on the PS5.
build_compat() {
    [[ $target == ps5 ]] || return 0
    local o=$src/build-compat c=$(dirname "$here")/compat
    mkdir -p "$o" "$prefix/lib"
    for f in resolv_stub libc_stubs libc_missing mmap vlc_missing vlc_search vlc_resolver; do
        "$CC" $CFLAGS -c "$c/$f.c" -o "$o/$f.o"
    done
    rm -f "$prefix/lib/libresolv.a" "$prefix/lib/libps5compat.a"
    "$AR" rcs "$prefix/lib/libresolv.a" "$o/resolv_stub.o"
    "$AR" rcs "$prefix/lib/libps5compat.a" "$o/libc_stubs.o" "$o/libc_missing.o" "$o/mmap.o" \
        "$o/vlc_missing.o" "$o/vlc_search.o" "$o/vlc_resolver.o"
    # The maths functions live in the SDK's libc, and there is no libm: an empty one
    # keeps configure's "-lm" link checks (VLC's sincos, lrint, ...) from failing.
    echo "" > "$o/empty.c"
    "$CC" $CFLAGS -c "$o/empty.c" -o "$o/empty.o"
    rm -f "$prefix/lib/libm.a"
    "$AR" rcs "$prefix/lib/libm.a" "$o/empty.o"
}

build_zlib() {
    local v=1.3.1
    fetch "https://zlib.net/fossils/zlib-$v.tar.gz"
    unpack "zlib-$v.tar.gz" "zlib-$v"
    (cd "$src/zlib-$v" && ./configure --static --prefix="$prefix" &&
        make -j"$jobs" libz.a && make install)
}

build_ffmpeg() {
    local v=8.1.2
    fetch "https://ffmpeg.org/releases/ffmpeg-$v.tar.xz"
    unpack "ffmpeg-$v.tar.xz" "ffmpeg-$v"
    # VLC 3.0.24's own FFmpeg patch: keep H.264's aspect ratio from the first frame.
    patch -d "$src/ffmpeg-$v" -p1 -s < "$src/vlc-3.0.24/contrib/src/ffmpeg/h264_early_SAR.patch" || true
    # VLC-PS5's own (patches/ffmpeg/): the PS5 recordings' Opus header, ...
    for p in "$(dirname "$(dirname "$here")")"/patches/ffmpeg/*.patch; do
        patch -d "$src/ffmpeg-$v" -p1 -s < "$p"
    done
    # Decoders, demuxers, parsers and swscale only. No autodetect: nothing from the system.
    (cd "$src/ffmpeg-$v" && ./configure --prefix="$prefix" \
        --enable-static --disable-shared --enable-pic --disable-autodetect \
        --disable-programs --disable-doc --disable-debug \
        --disable-avdevice --disable-devices --disable-avfilter --disable-filters \
        --disable-network --disable-iconv --disable-bzlib --disable-lzma \
        --disable-encoders --disable-muxers --disable-hwaccels \
        --enable-zlib \
        --extra-cflags="-I$prefix/include" --extra-ldflags="-L$prefix/lib" \
        ${ffmpeg_cross:-} &&
        make -j"$jobs" && make install)
}

build_dav1d() {
    local v=1.5.4
    fetch "https://download.videolan.org/pub/videolan/dav1d/$v/dav1d-$v.tar.xz"
    unpack "dav1d-$v.tar.xz" "dav1d-$v"
    meson setup "$src/build-dav1d-$target" "$src/dav1d-$v" --wipe ${meson_cross:-} \
        --prefix "$prefix" --libdir lib --default-library static --buildtype release \
        -Denable_tools=false -Denable_tests=false
    ninja -C "$src/build-dav1d-$target" install
}

build_ebml() {
    local v=1.4.6
    fetch "https://dl.matroska.org/downloads/libebml/libebml-$v.tar.xz"
    unpack "libebml-$v.tar.xz" "libebml-$v"
    cmake -S "$src/libebml-$v" -B "$src/build-ebml-$target" -G Ninja ${cmake_cross:-} \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    cmake --build "$src/build-ebml-$target" && cmake --install "$src/build-ebml-$target"
}

build_matroska() {
    local v=1.7.2
    fetch "https://dl.matroska.org/downloads/libmatroska/libmatroska-$v.tar.xz"
    unpack "libmatroska-$v.tar.xz" "libmatroska-$v"
    cmake -S "$src/libmatroska-$v" -B "$src/build-matroska-$target" -G Ninja ${cmake_cross:-} \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$prefix" \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    cmake --build "$src/build-matroska-$target" && cmake --install "$src/build-matroska-$target"
}

# MPEG-TS tables, for VLC's own ts demux (.ts, .m2ts, Blu-ray rips).
build_dvbpsi() {
    local v=1.3.3
    fetch "https://download.videolan.org/pub/libdvbpsi/$v/libdvbpsi-$v.tar.bz2"
    unpack "libdvbpsi-$v.tar.bz2" "libdvbpsi-$v"
    (cd "$src/libdvbpsi-$v" && ./configure ${host_triple:+--host=$host_triple} --prefix="$prefix"         --enable-static --disable-shared --with-pic &&
        make -j"$jobs" -C src && make -C src install && make install-pkgconfigDATA)
}

# Text for the OSD and subtitles (VLC's freetype module). Plain FreeType for now;
# fribidi/harfbuzz/libass join in World 7.
build_freetype() {
    local v=2.13.3
    fetch "https://download.savannah.gnu.org/releases/freetype/freetype-$v.tar.xz"
    unpack "freetype-$v.tar.xz" "freetype-$v"
    cmake -S "$src/freetype-$v" -B "$src/build-freetype-$target" -G Ninja ${cmake_cross:-} \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$prefix" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DFT_REQUIRE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    cmake --build "$src/build-freetype-$target" && cmake --install "$src/build-freetype-$target"
}

# Right-to-left text (Arabic, Hebrew subtitles) for VLC's freetype module.
build_fribidi() {
    local v=1.0.16
    fetch "https://github.com/fribidi/fribidi/releases/download/v$v/fribidi-$v.tar.xz"
    unpack "fribidi-$v.tar.xz" "fribidi-$v"
    meson setup "$src/build-fribidi-$target" "$src/fribidi-$v" --wipe ${meson_cross:-} \
        --prefix "$prefix" --libdir lib --default-library static --buildtype release \
        -Ddocs=false -Dbin=false -Dtests=false
    ninja -C "$src/build-fribidi-$target" install
}

# Shaping (Arabic letters joining, Indic and Thai clusters) through FreeType.
build_harfbuzz() {
    local v=10.4.0
    fetch "https://github.com/harfbuzz/harfbuzz/releases/download/$v/harfbuzz-$v.tar.xz"
    unpack "harfbuzz-$v.tar.xz" "harfbuzz-$v"
    meson setup "$src/build-harfbuzz-$target" "$src/harfbuzz-$v" --wipe ${meson_cross:-} \
        --prefix "$prefix" --libdir lib --default-library static --buildtype release \
        -Dfreetype=enabled -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dchafa=disabled \
        -Dicu=disabled -Dgraphite2=disabled -Dtests=disabled -Ddocs=disabled \
        -Dutilities=disabled -Dintrospection=disabled -Dbenchmark=disabled
    ninja -C "$src/build-harfbuzz-$target" install
}

# HTTPS (Batch 6): GnuTLS on nettle on GMP, as VLC 3.0.24's contrib builds them.
# On the PS5 all three in plain C: their x86 assembly keeps constant tables in
# the code section and reads them, and code is execute-only on the console
# (run 14: SYSTEM_XO_VIOLATION, the app killed with no report).
# Plain C GMP (no assembly): it only does the handshake's big-number maths.
# Its temporaries on the heap, not the stack (VLC's threads aren't big).
build_gmp() {
    local v=6.3.0
    fetch "https://ftp.gnu.org/gnu/gmp/gmp-$v.tar.xz"
    unpack "gmp-$v.tar.xz" "gmp-$v"
    (cd "$src/gmp-$v" && CFLAGS="$cflags -std=gnu99" ./configure --prefix="$prefix" \
        ${host_triple:+--host=$host_triple --build=x86_64-pc-linux-gnu} \
        --enable-static --disable-shared --disable-assembly --with-pic         --enable-alloca=malloc-reentrant &&
        make -j"$jobs" && make install)
}

build_nettle() {
    local v=3.10.2
    fetch "https://ftp.gnu.org/gnu/nettle/nettle-$v.tar.gz"
    unpack "nettle-$v.tar.gz" "nettle-$v"
    (cd "$src/nettle-$v" && ./configure --prefix="$prefix" --libdir="$prefix/lib" \
        ${host_triple:+--host=$host_triple --build=x86_64-pc-linux-gnu} \
        --enable-static --disable-shared --disable-documentation --disable-openssl \
        $( [[ $target == ps5 ]] && echo --disable-assembler --disable-fat ) \
        --enable-pic CPPFLAGS="-I$prefix/include" LDFLAGS="-L$prefix/lib" &&
        make -j"$jobs" SUBDIRS= && make SUBDIRS= install)
}

# Certificates are checked against Mozilla's list, shipped with the app
# (certs/cacert.pem in the app folder, /app0 on the console).
build_gnutls() {
    local v=3.8.13 store=/etc/ssl/certs/ca-certificates.crt
    [[ $target == ps5 ]] && store=/app0/certs/cacert.pem
    fetch "https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-$v.tar.xz"
    unpack "gnutls-$v.tar.xz" "gnutls-$v"
    (cd "$src/gnutls-$v" && ./configure --prefix="$prefix" --libdir="$prefix/lib" \
        ${host_triple:+--host=$host_triple --build=x86_64-pc-linux-gnu} \
        --enable-static --disable-shared --with-pic \
        --with-default-trust-store-file="$store" --with-default-trust-store-dir=no \
        --without-p11-kit --disable-cxx --disable-srp-authentication \
        --disable-anon-authentication --disable-openssl-compatibility --disable-guile \
        --disable-nls --without-libintl-prefix --disable-doc --disable-tools \
        --disable-tests --disable-manpages --with-included-libtasn1 \
        --with-included-unistring --without-idn --without-zlib --without-brotli \
        --without-zstd --without-tpm --without-tpm2 --disable-libdane \
        --without-leancrypto --disable-ktls \
        $( [[ $target == ps5 ]] && echo --disable-hardware-acceleration ) \
        CPPFLAGS="-I$prefix/include" LDFLAGS="-L$prefix/lib" &&
        make -j"$jobs" -C gl && make -j"$jobs" -C lib &&
        make -C gl install && make -C lib install)
}

# SMB shares (Batch 6): libsmb2 for VLC's smb2 module, no Kerberos.
build_smb2() {
    local v=6.1 d=libsmb2-libsmb2-6.1
    fetch "https://github.com/sahlberg/libsmb2/archive/refs/tags/libsmb2-$v.tar.gz" "libsmb2-$v.tar.gz"
    unpack "libsmb2-$v.tar.gz" "$d"
    patch -d "$src/$d" -p1 -s < "$src/vlc-3.0.24/contrib/src/smb2/0001-cmake-add-ENABLE_LIBKRB5-and-ENABLE_GSSAPI-options.patch"
    cmake -S "$src/$d" -B "$src/build-smb2-$target" -G Ninja ${cmake_cross:-} \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DENABLE_EXAMPLES=OFF
    cmake --build "$src/build-smb2-$target" && cmake --install "$src/build-smb2-$target"
}

# DLNA / UPnP media servers (Batch 6): pupnp for VLC's upnp module.
build_upnp() {
    local v=1.14.31 d=pupnp-release-1.14.31 ipv6=ON
    # The console refused UPnP's sockets (UPNP_E_SOCKET_ERROR, run 12): IPv4 only there.
    [[ $target == ps5 ]] && ipv6=OFF
    fetch "https://github.com/pupnp/pupnp/archive/refs/tags/release-$v.tar.gz" "pupnp-release-$v.tar.gz"
    unpack "pupnp-release-$v.tar.gz" "$d"
    for p in libtool-nostdlib-workaround miniserver; do
        patch -d "$src/$d" -p1 -s < "$src/vlc-3.0.24/contrib/src/upnp/$p.patch"
    done
    cmake -S "$src/$d" -B "$src/build-upnp-$target" -G Ninja ${cmake_cross:-} \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DUPNP_BUILD_SHARED=OFF -DUPNP_BUILD_STATIC=ON \
        -DBUILD_TESTING=OFF -DUPNP_BUILD_SAMPLES=OFF -DUPNP_ENABLE_IPV6=$ipv6
    cmake --build "$src/build-upnp-$target" && cmake --install "$src/build-upnp-$target"
}

# XML (Batch 6): VLC's xml reader for DASH streams, podcast feeds and .xspf
# playlists. Only the parser; no iconv (VLC hands it UTF-8).
build_libxml2() {
    local v=2.15.3
    fetch "https://download.gnome.org/sources/libxml2/2.15/libxml2-$v.tar.xz"
    unpack "libxml2-$v.tar.xz" "libxml2-$v"
    cmake -S "$src/libxml2-$v" -B "$src/build-libxml2-$target" -G Ninja ${cmake_cross:-}         -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release         -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON         -DLIBXML2_WITH_C14N=OFF -DLIBXML2_WITH_ISO8859X=OFF -DLIBXML2_WITH_SCHEMAS=OFF         -DLIBXML2_WITH_SCHEMATRON=OFF -DLIBXML2_WITH_VALID=OFF -DLIBXML2_WITH_WRITER=OFF         -DLIBXML2_WITH_XINCLUDE=OFF -DLIBXML2_WITH_XPATH=OFF -DLIBXML2_WITH_XPTR=OFF         -DLIBXML2_WITH_MODULES=OFF -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_ICONV=OFF         -DLIBXML2_WITH_REGEXPS=OFF -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_PROGRAMS=OFF         -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_HTTP=OFF         -DLIBXML2_WITH_THREADS=ON -DLIBXML2_WITH_DEBUG=OFF
    cmake --build "$src/build-libxml2-$target" && cmake --install "$src/build-libxml2-$target"
}

# Styled subtitles (extras): libass for VLC's libass module, on FreeType,
# FriBidi and HarfBuzz. No fontconfig on the console: the fonts are the file's
# own (MKV attachments), our subtitle font and the Noto fallbacks.
build_ass() {
    local v=0.17.5
    fetch "https://github.com/libass/libass/releases/download/$v/libass-$v.tar.xz"
    unpack "libass-$v.tar.xz" "libass-$v"
    (cd "$src/libass-$v" && ./configure --prefix="$prefix" --libdir="$prefix/lib" \
        ${host_triple:+--host=$host_triple --build=x86_64-pc-linux-gnu} \
        --enable-static --disable-shared --with-pic --disable-test --disable-fontconfig \
        --disable-require-system-font-provider --disable-libunibreak \
        CPPFLAGS="-I$prefix/include" LDFLAGS="-L$prefix/lib" &&
        make -j"$jobs" && make install)
}

# Discs (extras): DVD (libdvdread + libdvdnav, with menus) and Blu-ray
# (libbluray, no BD-J), as ISO files or folders. No decryption libraries:
# discs without copy protection.
build_dvdread() {
    local v=6.1.3
    fetch "https://download.videolan.org/pub/videolan/libdvdread/$v/libdvdread-$v.tar.bz2"
    unpack "libdvdread-$v.tar.bz2" "libdvdread-$v"
    (cd "$src/libdvdread-$v" && ./configure --prefix="$prefix" --libdir="$prefix/lib" \
        ${host_triple:+--host=$host_triple --build=x86_64-pc-linux-gnu} \
        --enable-static --disable-shared --with-pic --disable-apidoc &&
        make -j"$jobs" && make install)
}

build_dvdnav() {
    local v=6.1.1
    fetch "https://download.videolan.org/pub/videolan/libdvdnav/$v/libdvdnav-$v.tar.bz2"
    unpack "libdvdnav-$v.tar.bz2" "libdvdnav-$v"
    (cd "$src/libdvdnav-$v" && ./configure --prefix="$prefix" --libdir="$prefix/lib" \
        ${host_triple:+--host=$host_triple --build=x86_64-pc-linux-gnu} \
        --enable-static --disable-shared --with-pic &&
        make -j"$jobs" && make install)
}

build_bluray() {
    local v=1.4.1
    fetch "https://download.videolan.org/pub/videolan/libbluray/$v/libbluray-$v.tar.xz"
    unpack "libbluray-$v.tar.xz" "libbluray-$v"
    meson setup "$src/build-bluray-$target" "$src/libbluray-$v" --wipe ${meson_cross:-} \
        --prefix "$prefix" --libdir lib --default-library static --buildtype release \
        -Dfreetype=enabled -Dlibxml2=enabled -Dfontconfig=disabled -Denable_tools=false \
        -Dbdj_jar=disabled -Denable_docs=false
    ninja -C "$src/build-bluray-$target" install
    # Its gc_free (graphics controller) clashes with Mesa's (RADV) in the title.
    llvm-objcopy-18 --redefine-sym gc_free=bluray_gc_free "$prefix/lib/libbluray.a"
}

# ZIP / RAR / 7z (extras): libarchive for VLC's archive module, reading only.
build_libarchive() {
    local v=3.8.9
    fetch "https://github.com/libarchive/libarchive/releases/download/v$v/libarchive-$v.tar.xz"
    unpack "libarchive-$v.tar.xz" "libarchive-$v"
    cmake -S "$src/libarchive-$v" -B "$src/build-libarchive-$target" -G Ninja ${cmake_cross:-} \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$prefix" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DENABLE_CPIO=OFF -DENABLE_TAR=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_TEST=OFF \
        -DENABLE_WERROR=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_LZMA=OFF \
        -DENABLE_ICONV=OFF -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF \
        -DENABLE_CNG=OFF -DENABLE_BZip2=OFF -DENABLE_ZSTD=OFF -DENABLE_LZ4=OFF -DENABLE_LIBB2=OFF \
        -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_ZLIB=ON
    cmake --build "$src/build-libarchive-$target" && cmake --install "$src/build-libarchive-$target"
}

all=(nasm compat zlib ffmpeg dav1d ebml matroska dvbpsi freetype fribidi harfbuzz gmp nettle gnutls smb2 upnp libxml2 ass dvdread dvdnav bluray libarchive)
recipes=("$@")
[[ ${#recipes[@]} -gt 0 ]] || recipes=("${all[@]}")
for r in "${recipes[@]}"; do
    start=$SECONDS
    printf '[%s] %-9s ... ' "$target" "$r"
    log=$work/logs/$target-$r.log
    if ! ( "build_$r" ) >"$log" 2>&1; then
        echo "FAILED, last lines of $log:"
        tail -25 "$log"
        exit 1
    fi
    echo "ok ($((SECONDS - start)) s)"
done
