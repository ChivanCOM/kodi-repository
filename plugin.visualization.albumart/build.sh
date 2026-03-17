#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Build script for plugin.visualization.albumart (macOS)
#
# Usage:  ./build.sh          — first-time setup + build
#         ./build.sh rebuild  — clean rebuild
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KODI_SRC="/tmp/xbmc-omega"
KODI_INCLUDE="${KODI_SRC}/xbmc/addons/kodi-dev-kit/include"
STB_HEADER="${SCRIPT_DIR}/src/stb_image.h"
BUILD_DIR="${SCRIPT_DIR}/build"

# ── 1. Check build tools ──────────────────────────────────────────────────────
echo "→ Checking build tools..."
if ! command -v cmake &>/dev/null; then
  echo "  cmake not found — install with: brew install cmake"
  exit 1
fi
if ! command -v git &>/dev/null; then
  echo "  git not found — install Xcode command line tools: xcode-select --install"
  exit 1
fi
echo "  cmake $(cmake --version | head -1 | awk '{print $3}') ✓"

# ── 2. Download stb_image.h ───────────────────────────────────────────────────
if [[ ! -f "$STB_HEADER" ]]; then
  echo "→ Downloading stb_image.h..."
  curl -sL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" \
       -o "$STB_HEADER"
  echo "  stb_image.h ✓"
else
  echo "→ stb_image.h already present ✓"
fi

# ── 3. Download stb_truetype.h ────────────────────────────────────────────────
STB_TRUETYPE="${SCRIPT_DIR}/src/stb_truetype.h"
if [[ ! -f "$STB_TRUETYPE" ]]; then
  echo "→ Downloading stb_truetype.h..."
  curl -sL "https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h" \
       -o "$STB_TRUETYPE"
  echo "  stb_truetype.h ✓"
else
  echo "→ stb_truetype.h already present ✓"
fi

# ── 4. Fetch Kodi dev-kit headers (shallow, headers only) ────────────────────
if [[ ! -d "$KODI_INCLUDE" ]]; then
  echo "→ Fetching Kodi Omega headers (shallow clone, this may take a minute)..."
  git clone --depth=1 --filter=blob:none --sparse \
      -b Omega https://github.com/xbmc/xbmc.git "$KODI_SRC"
  pushd "$KODI_SRC" >/dev/null
  git sparse-checkout set xbmc/addons/kodi-dev-kit/include
  popd >/dev/null
  echo "  Kodi headers ✓"
else
  echo "→ Kodi headers already present ✓"
fi

# ── 4. Configure ─────────────────────────────────────────────────────────────
if [[ "${1:-}" == "rebuild" ]]; then
  echo "→ Cleaning build dir..."
  rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
echo "→ Configuring..."
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Release \
      -DKODI_INCLUDE_DIR="$KODI_INCLUDE"

# ── 5. Build ──────────────────────────────────────────────────────────────────
echo "→ Building..."
cmake --build "$BUILD_DIR" --config Release

# ── 6. Copy .dylib next to addon.xml ─────────────────────────────────────────
LIB_NAME="plugin.visualization.albumart.dylib"
SRC_LIB="${BUILD_DIR}/${LIB_NAME}"
DST_LIB="${SCRIPT_DIR}/${LIB_NAME}"

if [[ -f "$SRC_LIB" ]]; then
  cp "$SRC_LIB" "$DST_LIB"
  echo ""
  echo "✓ Build succeeded: ${DST_LIB}"
  echo ""
  echo "Next steps:"
  echo "  1. Zip the addon folder:"
  echo "     cd \"$(dirname "$SCRIPT_DIR")\""
  echo "     zip -r plugin.visualization.albumart-1.0.0.zip plugin.visualization.albumart"
  echo "  2. Install in Kodi: Settings → Add-ons → Install from zip file"
  echo "  3. Select it: Settings → Music → Visualisation → iBroadcast Album Art"
else
  echo "✗ Build failed — library not found at ${SRC_LIB}"
  exit 1
fi
