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

# ----------------------------------------------------------------- config ---
REPO_DIR="$PWD"
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
[[ -d "vlc-${VLC_VERSION}" ]] || tar xf "vlc-${VLC_VERSION}.tar.xz"
popd >/dev/null

# ------------------------------------------------------------------ build ---
cd "$REPO_DIR"
export TARGET=ps5

log "Building dependencies (FFmpeg, GnuTLS, libass, ...)"
bash ps5/deps/build-deps.sh

log "Configuring and building libvlc ${VLC_VERSION}"
bash ps5/configure-vlc.sh

log "Packaging title (eboot.bin)"
bash ps5/build-title.sh

OUT="$REPO_DIR/dist/PPSA85300"
if [[ -d "$OUT" ]]; then
  log "Done. Output: $OUT"
  echo "Copy it to the console at /data/homebrew/PPSA85300/"
else
  warn "Build finished but $OUT was not found; check the output above."
fi
