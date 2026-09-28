/* GPL3 - Copyleft VERHILLE Arnaud */
#pragma once
constexpr bool wantsAbsolutePointer(bool fullscreen, bool windowTablet) {
    return !fullscreen && windowTablet;
}
constexpr bool wantsPointerCapture(bool fullscreen, bool focused, bool running,
                                   bool paused, bool absolute) {
    return fullscreen && focused && running && !paused && !absolute;
}
