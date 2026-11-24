#!/usr/bin/env bash
# Build "VLC for PS5" (https://github.com/ZiZc3/VLC) on Arch Linux.
#
# Run this from the root of the cloned repo:
#   git clone https://github.com/ZiZc3/VLC && cd VLC
#   ./build-vlc-ps5-arch.sh
#
# Everything (PS5_Vulkan, PS5_Mesa, SDKs, VLC tarball, build output) stays
# inside the repo folder under ./.build/. The only thing touched outside it is
# the system package install via pacman (skip with SKIP_PACKAGES=1).
#
# The READMEs expect ~/ps5/PS5_Vulkan, ~/ps5/PS5_Mesa (sibling checkout) and
# ~/ps5/vlc-ps5/src. To keep that working without leaving the repo, HOME is
# redirected to ./.build/home after the package install, so ~/ps5/... resolves
# inside the repo folder.
#
# Options (env vars): SKIP_PACKAGES=1  VLC_VERSION=3.0.24  JOBS=N
#                     SKIP_VULKAN=1   (PS5_Vulkan already built, skip that step)

set -Eeuo pipefail

# Everything below is wrapped in { ...; exit; } so bash parses the whole script
# before running any of it. Without this, editing or replacing this file while
# a (long) build is running makes bash read from the wrong offset and die with
# bogus "syntax error" messages.
{

SCRIPT_VERSION="11 (full isolation, env.sh, leak audit, libc.prx step)"
printf '\033[1;34m==> build-vlc-ps5-arch.sh version %s\033[0m\n' "$SCRIPT_VERSION"

# ----------------------------------------------------------------- config ---
REPO_DIR="$PWD"
REAL_HOME="$HOME"
VLC_VERSION="${VLC_VERSION:-3.0.24}"
JOBS="${JOBS:-$(nproc)}"

BUILD_ROOT="$REPO_DIR/.build"
FAKE_HOME="$BUILD_ROOT/home"
PS5_DIR="$FAKE_HOME/ps5"                       # what the READMEs call ~/ps5
VULKAN_DIR="$PS5_DIR/PS5_Vulkan"
MESA_DIR="$PS5_DIR/PS5_Mesa"                   # must be a sibling of PS5_Vulkan
SDKFORK_DIR="$PS5_DIR/PS5_PayloadSDK"          # likewise (payload SDK fork)
VLC_SRC_DIR="$PS5_DIR/vlc-ps5/src"
export MAKEFLAGS="-j${JOBS}"

# ---------------------------------------------------------------- helpers ---
log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
trap 'die "failed at line $LINENO"' ERR

# Run `make deps`; if it fails because the PS5_PayloadSDK fork lacks the pinned
# commit, fetch that commit from GitHub and retry (up to 3 times).
make_deps() {
  local log_file="$BUILD_ROOT/make-deps.log" sha try
  for try in 1 2 3; do
    if make deps 2>&1 | tee "$log_file"; then return 0; fi
    sha="$(grep -oE 'does not have [0-9a-f]{40}' "$log_file" | tail -n1 | awk '{print $4}' || true)"
    [[ -n "$sha" ]] || die "make deps failed (see output above)."
    warn "PS5_PayloadSDK is missing pinned commit $sha; fetching it"
    git -C "$SDKFORK_DIR" fetch origin "$sha" \
      || git -C "$SDKFORK_DIR" fetch --all --tags --prune \
      || true
    git -C "$SDKFORK_DIR" cat-file -e "$sha^{commit}" 2>/dev/null \
      || die "Commit $sha is not available from $(git -C "$SDKFORK_DIR" remote get-url origin). The pin in PS5_Vulkan may be ahead of what is pushed; try again later or check out a matching PS5_Vulkan revision."
  done
  die "make deps still failing after retries."
}

clone_or_update() {  # url dir  (full clone: PS5_Vulkan pins Mesa by revision)
  if [[ -d "$2/.git" ]]; then git -C "$2" pull --ff-only || warn "could not update $2; using existing checkout"
  else git clone "$1" "$2"; fi
}

[[ $EUID -ne 0 ]] || die "Do not run as root; sudo is only used for pacman."
command -v pacman >/dev/null || die "This script is for Arch Linux (pacman not found)."
[[ -f "$REPO_DIR/ps5/deps/build-deps.sh" && -f "$REPO_DIR/ps5/build-title.sh" ]] \
  || die "Run this from the root of the cloned ZiZc3/VLC repo (ps5/ folder not found in $REPO_DIR)."

# --------------------------------------------------------------- packages ---
if [[ "${SKIP_PACKAGES:-0}" != "1" ]]; then
  log "Installing build dependencies (pacman)"
  sudo pacman -Syu --needed --noconfirm \
    base-devel git curl wget unzip p7zip xz tar \
    clang llvm lld cmake ninja meson make ccache \
    autoconf automake libtool pkgconf gettext \
    nasm yasm bison flex gperf \
    python python-pip python-mako python-markupsafe python-yaml \
    glslang shaderc spirv-tools vulkan-headers lua
fi

mkdir -p "$PS5_DIR" "$VLC_SRC_DIR"
export HOME="$FAKE_HOME"          # from here on, ~/ps5/... lives in the repo

# Keep EVERYTHING inside the repo folder: not just HOME, but every place a tool
# or one of the repo's scripts might look. The repo scripts honour these
# variables (build-deps.sh / build-title.sh read them before falling back to
# $HOME/ps5/...), so they no longer depend on HOME alone.
export VLC_PS5_WORK="$PS5_DIR/vlc-ps5"
export PS5_VULKAN="$VULKAN_DIR"
export XDG_CACHE_HOME="$FAKE_HOME/.cache" XDG_CONFIG_HOME="$FAKE_HOME/.config" \
       XDG_DATA_HOME="$FAKE_HOME/.local/share" XDG_STATE_HOME="$FAKE_HOME/.local/state"
export TMPDIR="$BUILD_ROOT/tmp"
export CCACHE_DIR="$FAKE_HOME/.ccache" PIP_CACHE_DIR="$FAKE_HOME/.cache/pip" \
       CARGO_HOME="$FAKE_HOME/.cargo" GIT_CONFIG_GLOBAL="$FAKE_HOME/.gitconfig"
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME"

# Source this to run any repo script by hand with the same isolation:
#   source .build/env.sh && TARGET=ps5 bash ps5/build-title.sh
cat > "$BUILD_ROOT/env.sh" <<ENVEOF
export HOME="$FAKE_HOME" VLC_PS5_WORK="$VLC_PS5_WORK" PS5_VULKAN="$PS5_VULKAN"
export XDG_CACHE_HOME="$XDG_CACHE_HOME" XDG_CONFIG_HOME="$XDG_CONFIG_HOME"
export XDG_DATA_HOME="$XDG_DATA_HOME" XDG_STATE_HOME="$XDG_STATE_HOME" TMPDIR="$TMPDIR"
export CCACHE_DIR="$CCACHE_DIR" PIP_CACHE_DIR="$PIP_CACHE_DIR" CARGO_HOME="$CARGO_HOME"
export GIT_CONFIG_GLOBAL="$GIT_CONFIG_GLOBAL" PATH="$BUILD_ROOT/bin:\$PATH"
[ -n "\${PS5_PAYLOAD_SDK:-}" ] || export PS5_PAYLOAD_SDK="$VULKAN_DIR/.deps/native/ps5-payload-sdk"
ENVEOF

# Leak audit: list files created/changed under your REAL home (outside the
# repo folder) during this run. Runs on exit, success or failure.
touch "$BUILD_ROOT/.run-start"
audit_leaks() {
  local out
  out="$(find "$REAL_HOME" -xdev \( -path "$REPO_DIR" -o -path "$REAL_HOME/.local/share/Trash" \) -prune \
          -o -type f -newer "$BUILD_ROOT/.run-start" -print 2>/dev/null | head -n 40 || true)"
  if [[ -n "$out" ]]; then
    warn "files touched under $REAL_HOME OUTSIDE $REPO_DIR during this run:"
    printf '  %s\n' $out >&2
  else
    printf '\033[1;34m==> audit: nothing written under %s outside the repo folder\033[0m\n' "$REAL_HOME"
  fi
}
trap audit_leaks EXIT

# Arch ships unversioned LLVM tools (llvm-objcopy, clang, ...) but the repo's
# scripts call the Debian/Ubuntu names (llvm-objcopy-18, clang-18, ...).
# Provide version-suffixed symlinks in ./.build/bin (no system changes).
LLVM_SUFFIX="${LLVM_SUFFIX:-18}"
mkdir -p "$BUILD_ROOT/bin"
for tool in /usr/bin/llvm-* /usr/bin/clang /usr/bin/clang++ /usr/bin/clang-cpp \
            /usr/bin/ld.lld /usr/bin/lld /usr/bin/lld-link /usr/bin/wasm-ld; do
  [[ -x "$tool" ]] || continue
  name="$(basename "$tool")"
  [[ "$name" =~ -[0-9]+$ ]] && continue            # already versioned
  [[ -e "/usr/bin/${name}-${LLVM_SUFFIX}" ]] && continue
  ln -sf "$tool" "$BUILD_ROOT/bin/${name}-${LLVM_SUFFIX}"
done
export PATH="$BUILD_ROOT/bin:$PATH"

# ------------------------------------------------- PS5_Vulkan (RADV) ---------
# Plain `make` in PS5_Vulkan builds probe titles and is NOT what VLC needs.
# The README's RADV recipe is: make deps; setup-native-dependencies; build-radv.
build_vulkan() {
  log "Cloning PS5_Vulkan, PS5_Mesa and PS5_PayloadSDK into $PS5_DIR"
  clone_or_update https://github.com/mihawk-99/PS5_Vulkan.git "$VULKAN_DIR"
  clone_or_update https://github.com/mihawk-99/PS5_Mesa.git   "$MESA_DIR"
  clone_or_update https://github.com/mihawk-99/PS5_PayloadSDK.git "$SDKFORK_DIR"
  # a shallow clone (e.g. from an earlier run) lacks the pinned commit
  if [[ "$(git -C "$SDKFORK_DIR" rev-parse --is-shallow-repository)" == "true" ]]; then
    git -C "$SDKFORK_DIR" fetch --unshallow --tags
  fi
  mkdir -p "$BUILD_ROOT"

  cd "$VULKAN_DIR"
  log "PS5_Vulkan: make doctor"
  make doctor || warn "make doctor reported problems (see above); continuing"

  log "PS5_Vulkan: make deps (payload SDK + test framework into .deps/)"
  make_deps

  log "PS5_Vulkan: tools/setup-native-dependencies.sh (payload SDK fork)"
  bash tools/setup-native-dependencies.sh

  log "PS5_Vulkan: tools/build-radv.sh release (what titles ship)"
  bash tools/build-radv.sh release
  cd "$REPO_DIR"
}

if [[ "${SKIP_VULKAN:-0}" == "1" ]]; then
  log "Skipping PS5_Vulkan (SKIP_VULKAN=1)"
else
  build_vulkan
fi

# ------------------------------------------- PS5_Vulkan host packaging tool ---
# ps5/build-title.sh needs PS5_Vulkan/build/host/ps5-native-tool (ELF converter
# / FSELF writer). PS5_Vulkan builds it inside tools/build.sh with a C++ compile
# using -Werror, which fails on Arch's newer clang, so build it here with the
# same sources but without -Werror (warnings are fine).
ensure_native_tool() {
  local root="$VULKAN_DIR" native="$VULKAN_DIR/tooling/native"
  local tool="$root/build/host/ps5-native-tool"
  [[ -x "$tool" ]] && { log "ps5-native-tool already built"; return; }
  log "Building PS5_Vulkan host tool: ps5-native-tool"

  local cxx="${CXX:-$(command -v clang++ || command -v clang++-18 || true)}"
  [[ -n "$cxx" ]] || die "clang++ not found"
  [[ -f "$native/native_app_builder.cpp" ]] || die "$native/native_app_builder.cpp not found (PS5_Vulkan layout changed?)"

  # zlib: PS5_Vulkan's pinned static copy if `make deps` fetched it, else system zlib
  local zlib_root="$root/.deps/native/zlib/root" zlib_args zlib_inc=()
  local zlib_a; zlib_a="$(find "$zlib_root" -type f -name libz.a -print -quit 2>/dev/null || true)"
  if [[ -n "$zlib_a" ]]; then zlib_args=("$zlib_a"); zlib_inc=(-I "$zlib_root/usr/include")
  else zlib_args=(-lz); warn "pinned zlib not found; using system zlib"; fi

  mkdir -p "$root/build/host"
  "$cxx" -std=c++20 -O2 -Wall -Wextra "${zlib_inc[@]}" \
    "$native/native_app_builder.cpp" "$native/self_container.cpp" \
    "$native/elf_object.cpp" "$native/sce_module_writer.cpp" \
    "${zlib_args[@]}" -o "$tool" \
    || die "Failed to compile ps5-native-tool (see compiler errors above)."
  [[ -x "$tool" ]] || die "ps5-native-tool was not produced."
}
ensure_native_tool

# ------------------------------------------- PS5_Vulkan runtime/libc.prx ------
# build-title.sh copies PS5_Vulkan/runtime/libc.prx into the title. PS5_Vulkan's
# README calls it the "generated clean-room libc.prx"; only libc.prx.sha256 is
# tracked, so it has to be generated. Try the Makefile rule for it; if there is
# none, say where PS5_Vulkan mentions it instead of guessing further.
ensure_libc_prx() {
  local prx="$VULKAN_DIR/runtime/libc.prx"
  if [[ -f "$prx" ]]; then
    ( cd "$VULKAN_DIR/runtime" && sha256sum --check --strict --quiet libc.prx.sha256 ) \
      && { log "runtime/libc.prx present and checksum OK"; return; }
    warn "runtime/libc.prx exists but fails its checksum; regenerating"
    rm -f "$prx"
  fi
  log "Generating PS5_Vulkan runtime/libc.prx"
  ( cd "$VULKAN_DIR"
    for t in runtime/libc.prx libc.prx runtime; do
      if make -n "$t" >/dev/null 2>&1; then
        log "make $t"; make "$t" && [[ -f runtime/libc.prx ]] && break
      fi
    done )
  if [[ ! -f "$prx" ]]; then
    warn "Could not generate $prx automatically. PS5_Vulkan mentions it here:"
    ( cd "$VULKAN_DIR" && grep -rn "libc.prx" Makefile tools tooling 2>/dev/null \
        | grep -v "sce_module/libc.prx" | head -n 20 ) >&2 || true
    die "Create runtime/libc.prx (see lines above), or copy it from another PS5_Vulkan checkout, then re-run."
  fi
  ( cd "$VULKAN_DIR/runtime" && sha256sum --check --strict --quiet libc.prx.sha256 ) \
    || die "Generated libc.prx fails its checksum (runtime/libc.prx.sha256)."
}
ensure_libc_prx

# ------------------------------------------------------------- PS5 SDK ------
# Reuse the SDK that PS5_Vulkan fetched (kept inside the repo) if the caller
# did not point us at one.
if [[ -z "${PS5_PAYLOAD_SDK:-}" ]]; then
  cc="$(find "$VULKAN_DIR/.deps" "$FAKE_HOME/ps5-payload-sdk" -maxdepth 6 \
          -name 'prospero-clang*' -path '*/bin/*' 2>/dev/null | head -n1 || true)"
  if [[ -n "$cc" ]]; then
    export PS5_PAYLOAD_SDK="$(dirname "$(dirname "$cc")")"
    log "Using PS5 payload SDK at $PS5_PAYLOAD_SDK"
  else
    warn "No PS5 payload SDK found; set PS5_PAYLOAD_SDK if the VLC build asks for it."
  fi
fi

# -------------------------------------------------------------- VLC source --
log "Fetching vlc-${VLC_VERSION} tarball"
pushd "$VLC_SRC_DIR" >/dev/null
[[ -f "vlc-${VLC_VERSION}.tar.xz" ]] || curl -fLO "https://download.videolan.org/pub/videolan/vlc/${VLC_VERSION}/vlc-${VLC_VERSION}.tar.xz"
# configure-vlc.sh patches the VLC tree in place and is NOT safe to run twice
# (a second run re-applies the patches: "redefinition of 'colour'" in vmem.c).
# So: no stamp = the tree may be half/double patched -> start from fresh source.
CONF_STAMP="$BUILD_ROOT/.libvlc-${VLC_VERSION}-configured"
[[ "${FORCE_CONFIGURE:-0}" == "1" ]] && rm -f "$CONF_STAMP"
if [[ ! -f "$CONF_STAMP" ]]; then
  rm -rf "vlc-${VLC_VERSION}" "$PS5_DIR/vlc-ps5/build-vlc-ps5"
  tar xf "vlc-${VLC_VERSION}.tar.xz"
fi
popd >/dev/null

# ------------------------------------------- tool names in the repo scripts ---
# Rewrites Debian-style versioned tool names (llvm-objcopy-18, clang-18, ...)
# to Arch's unversioned ones in the repo's own build scripts. Idempotent.
# Undo with: git checkout -- ps5/        Disable with: PATCH_SCRIPTS=0
if [[ "${PATCH_SCRIPTS:-1}" == "1" ]]; then
  log "Patching versioned LLVM tool names in ps5/ scripts"
  re='\b(llvm-[a-z0-9]+(-[a-z0-9]+)*|clang\+\+|clang|ld\.lld|lld)-'"${LLVM_SUFFIX}"'\b'
  mapfile -t files < <(grep -rlE "$re" ps5 --include='*.sh' --include='Makefile*' \
                         --include='*.mk' --include='*.cmake' --include='*.txt' 2>/dev/null || true)
  for f in "${files[@]}"; do
    sed -i -E "s/${re}/\\1/g" "$f"
    echo "  patched $f"
  done
  [[ ${#files[@]} -gt 0 ]] || echo "  nothing to patch"
  leftover="$(grep -rnE -- "-${LLVM_SUFFIX}\b" ps5 --include='*.sh' 2>/dev/null | grep -v 'prospero' || true)"
  [[ -z "$leftover" ]] || { warn "remaining '-${LLVM_SUFFIX}' references (check these if the build fails):"; echo "$leftover" >&2; }
fi

# ------------------------------------------------------------------ build ---
cd "$REPO_DIR"
export TARGET=ps5

log "Building dependencies (FFmpeg, GnuTLS, libass, ...)"
bash ps5/deps/build-deps.sh

log "Configuring and building libvlc ${VLC_VERSION}"
if [[ -f "$CONF_STAMP" ]]; then
  log "libvlc already configured and built (FORCE_CONFIGURE=1 to redo); skipping"
else
  bash ps5/configure-vlc.sh
  touch "$CONF_STAMP"
fi

log "Packaging title (eboot.bin)"
bash ps5/build-title.sh

OUT="$REPO_DIR/dist/PPSA85300"
if [[ -d "$OUT" ]]; then
  log "Done. Output: $OUT"
  echo "Copy it to the console at /data/homebrew/PPSA85300/"
else
  warn "Build finished but $OUT was not found; check the output above."
fi

exit 0
}
