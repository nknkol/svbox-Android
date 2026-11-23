#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetch pinned upstream sources for Step 4 GPU hardware acceleration (gfxstream).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

clone_pinned() {
    local name="$1"
    local url="$2"
    local ref="$3"
    local commit="$4"
    if [ -d "$TP/$name/.git" ]; then
        echo "[fetch] $name already present"
        return
    fi
    echo "[fetch] cloning $name ($ref @ $commit)..."
    git clone --depth 1 -b "$ref" "$url" "$TP/$name"
}

clone_pinned aemu \
    https://android.googlesource.com/platform/hardware/google/aemu \
    main \
    3c1ced8a369417db591eb7cd083af5bb2c317975

clone_pinned gfxstream \
    https://android.googlesource.com/platform/hardware/google/gfxstream \
    main \
    d047a57228332d995d36600792fa9ccc26cf8ae6

clone_pinned goldfish-opengl-r \
    https://android.googlesource.com/device/generic/goldfish-opengl \
    android11-gsi \
    482e759862dc1aa0dd8167bd681e9476d9432e75

echo "[fetch] done"
