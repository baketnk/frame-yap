#pragma once
#include <utility>

namespace frameyap {
// Follow docs/rendering-performance.md: retain initialized images throughout
// manipulation, leaving content invalidation pending until the next refresh.
template<class Render, class Upload>
bool refresh_texture(bool manipulating, bool initialized, Render&& render, Upload&& upload) {
    if (manipulating && initialized) return false;
    if (!std::forward<Render>(render)()) return false;
    std::forward<Upload>(upload)();
    return true;
}
}
