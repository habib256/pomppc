/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
#pragma once
constexpr bool wantsAbsolutePointer(bool fullscreen, bool windowTablet) {
    return !fullscreen && windowTablet;
}
constexpr bool wantsPointerCapture(bool fullscreen, bool focused, bool running,
                                   bool paused, bool absolute) {
    return fullscreen && focused && running && !paused && !absolute;
}
// Curseur de l'hôte masqué au-dessus de l'image de l'invité quand la position
// lui est envoyée en absolu (01/11) : Tiger dessine son propre curseur dans le
// framebuffer, au même endroit, d'où un double curseur. En relatif (fenêtre
// sans tablette), les deux curseurs divergent et celui de l'hôte reste visible
// pour aller aux menus ; en capture, il est déjà GLFW_CURSOR_DISABLED.
constexpr bool wantsHiddenHostCursor(bool hovered, bool absolute, bool captured) {
    return hovered && absolute && !captured;
}
