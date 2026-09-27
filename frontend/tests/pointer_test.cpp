#include "PointerPolicy.h"
#include <cassert>
int main() {
    assert(wantsAbsolutePointer(false, true));
    assert(!wantsAbsolutePointer(true, true));
    assert(!wantsAbsolutePointer(true, false));
    assert(!wantsAbsolutePointer(false, false));
    assert(wantsPointerCapture(true, true, true, false, false));
    assert(!wantsPointerCapture(false, true, true, false, false));
    assert(!wantsPointerCapture(true, false, true, false, false));
    assert(!wantsPointerCapture(true, true, false, false, false));
    assert(!wantsPointerCapture(true, true, true, true, false));
    assert(!wantsPointerCapture(true, true, true, false, true));
}
