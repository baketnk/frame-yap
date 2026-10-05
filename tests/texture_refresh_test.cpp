#include "texture_refresh.hpp"
#include <cassert>

int main() {
    bool initialized = false, dirty = true;
    int renders = 0, uploads = 0, model = 0, visible = -1;
    const auto refresh = [&](bool moving) {
        return frameyap::refresh_texture(moving, initialized, [&] {
            ++renders;
            if (!dirty) return false;
            visible = model; dirty = false;
            return true;
        }, [&] { ++uploads; initialized = true; });
    };
    assert(refresh(true)); // first image is allowed during manipulation
    assert(renders == 1 && uploads == 1 && visible == 0);
    for (int i = 1; i <= 200; ++i) {
        model = i; dirty = true; // model and cosmetic changes stay live
        assert(!refresh(true));
    }
    assert(renders == 1 && uploads == 1 && visible == 0 && dirty);
    assert(refresh(false)); // display the latest state once, after release
    assert(renders == 2 && uploads == 2 && visible == 200 && !dirty);
    assert(!refresh(false));
    assert(renders == 3 && uploads == 2);
}
