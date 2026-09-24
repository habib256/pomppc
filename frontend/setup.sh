#!/usr/bin/env bash
# One-time: fetch Dear ImGui into ./imgui (CMake creates the build dir itself).
# Mirrors pom68k/setup_imgui.sh. ImGui is not vendored in git (see .gitignore).
#
# The frontend needs the **docking** branch (IMGUI_HAS_DOCK: DockSpace,
# DockBuilder) — the guest screen is a window docked in the middle of the
# POMPPC shell. Pinned tag: IMGUI_TAG below (latest docking tag, 24/09/2026).
# A sibling Pomme's checkout is reused (offline) only if it is a docking build
# too; an existing ./imgui without IMGUI_HAS_DOCK (older setup) is replaced.
set -e
IMGUI_TAG="${IMGUI_TAG:-v1.92.9b-docking}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

has_dock() { [ -f "$1/imgui.cpp" ] && grep -q '^#define IMGUI_HAS_DOCK' "$1/imgui.h"; }

if [ -f imgui/imgui.cpp ] && ! has_dock imgui; then
    echo "imgui/ is not the docking branch — replacing it."
    rm -rf imgui
fi
if [ ! -f imgui/imgui.cpp ]; then
    echo "Cloning Dear ImGui $IMGUI_TAG (docking)..."
    if ! git clone --depth 1 --branch "$IMGUI_TAG" https://github.com/ocornut/imgui.git imgui; then
        rm -rf imgui
        # Offline: reuse a sibling Pomme's docking checkout. The repo root is
        # taken from git so this also works from a worktree.
        MAIN="$(cd "$(git rev-parse --git-common-dir)/.." 2>/dev/null && pwd || echo "$DIR/..")"
        for src in "$MAIN/../pom68k/imgui" "$MAIN/../POM2/imgui" "$MAIN/../POMIIGS/imgui"; do
            if has_dock "$src"; then
                echo "Offline: copying Dear ImGui (docking) from $src ..."
                cp -r "$src" imgui
                break
            fi
        done
    fi
    has_dock imgui || { echo "Dear ImGui (docking) introuvable." >&2; exit 1; }
else
    echo "imgui/ already present (docking)."
fi
grep -m1 '^#define IMGUI_VERSION ' imgui/imgui.h
echo "Done. Next: cmake -S . -B build && cmake --build build -j"
