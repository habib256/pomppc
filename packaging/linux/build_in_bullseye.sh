#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# build_in_bullseye.sh — COMPILE et empaquette, dans l'image figée (Dockerfile.bullseye), dépôt
# monté sur /work. Ne touche pas à apt : la chaîne est cuite dans l'image, épinglée par digest.
#
# Pourquoi `docker run` et pas `container:` dans le workflow : les actions node24 ont besoin de la
# glibc et du Node du runner, et un `container:` les ferait tourner ici. Elles restent sur l'hôte,
# seul ce script tourne dans le conteneur (même décision que POM2, release.yml:209-212).
#
# Env : POMPPC_VERSION (défaut : frontend/CMakeLists.txt), GUEST_ISO (exigé), JOBS.
set -euxo pipefail

git config --global --add safe.directory '*'
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
: "${GUEST_ISO:?GUEST_ISO attendu : packaging/linux/fetch_guest_iso.sh le télécharge}"
export JOBS="${JOBS:-$(nproc)}"

# --- QEMU : profil paquet, -O2 générique, jamais -march=native ---
# Hors du dépôt : l'arbre QEMU ne doit pas entrer dans `git status`, ni le paquet dans son arbre.
export QEMU_SRC="${QEMU_SRC:-/tmp/qemu-src}"
QEMU_PROFIL=paquet-linux scripts/build_qemu_qfb.sh

# --- Frontal ---
# -static-libstdc++ et -static-libgcc : aucune exigence GLIBCXX sur l'hôte, seule la glibc reste.
# LEGACY : libGL.so.1, présente partout où il y a un pilote. GLVND lierait libOpenGL.so.0, un
# paquet à part sur Debian et Ubuntu (c'est ce que lie le frontal du PC).
frontend/setup.sh
cmake -S frontend -B frontend/build-appimage -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DOpenGL_GL_PREFERENCE=LEGACY \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build frontend/build-appimage --parallel "$JOBS"

# --- AppImage ---
FRONTEND="$ROOT/frontend/build-appimage/pomppc" packaging/linux/build_appimage.sh
