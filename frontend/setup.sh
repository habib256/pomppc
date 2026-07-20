#!/usr/bin/env bash
# One-time: fetch Dear ImGui into ./imgui and create ./build.
# Mirrors pom68k/setup_imgui.sh. ImGui is not vendored in git (see .gitignore).
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"
if [ ! -f imgui/imgui.cpp ]; then
    # Reuse a sibling Pomme's checkout if present (identical upstream).
    for src in ../../pom68k/imgui ../../POMIIGS/imgui ../../POM2/imgui; do
        if [ -f "$src/imgui.cpp" ]; then
            echo "Copying Dear ImGui from $src ..."
            cp -r "$src" imgui
            break
        fi
    done
    if [ ! -f imgui/imgui.cpp ]; then
        echo "Cloning Dear ImGui..."
        git clone --depth 1 https://github.com/ocornut/imgui.git imgui
    fi
else
    echo "imgui/ already present."
fi
mkdir -p build
echo "Done. Next: cd build && cmake .. && make -j"
