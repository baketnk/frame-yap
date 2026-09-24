#include "panel_drag.hpp"

#include <cassert>
#include <cmath>
#include <limits>

using namespace frameyap;
namespace {
constexpr Matrix34 identity{{{{1.f, 0.f, 0.f, 0.f}},
                              {{0.f, 1.f, 0.f, 0.f}},
                              {{0.f, 0.f, 1.f, 0.f}}}};
void near(float actual, float expected) { assert(std::abs(actual - expected) < 1e-4f); }
Matrix34 translated(Matrix34 pose, float x, float y, float z) {
    pose[0][3] += x; pose[1][3] += y; pose[2][3] += z;
    return pose;
}
Matrix34 controller_at(float x, float y, float z = 1.f) {
    return translated(identity, x, y, z);
}
void scale_case(float x, float y, float factor) {
    // 1 m square, top-left (-.5,+.5); shift controller and hence its
    // intersection by (factor - 1) times the original anchor-to-hit vector.
    const auto down = controller_at(x - .5f, .5f - y);
    PanelDrag drag;
    assert(drag.begin(PanelDragKind::Scale, identity, 1.f, 1.f, x, y, down));
    const auto same = drag.update(down);
    assert(same); near(same->factor, 1.f); near(same->dx, 0); near(same->dy, 0);
    const auto changed = drag.update(translated(down, (factor - 1.f) * x, (1.f - factor) * y, 0));
    assert(changed); near(changed->factor, factor);
    near(changed->dx, 0); near(changed->dy, 0);
}
// Yaw +90 degrees: panel right=-Z, up=+Y, normal=+X.
Matrix34 yawed_panel() {
    return Matrix34{{{{0.f, 0.f, 1.f, 2.f}},
                     {{0.f, 1.f, 0.f, 3.f}},
                     {{-1.f, 0.f, 0.f, 4.f}}}};
}
} // namespace

int main() {
    PanelDrag drag;
    assert(!drag.active()); assert(!drag.update(controller_at(0, 0)));
    const auto down = controller_at(0, 0);
    assert(drag.begin(PanelDragKind::Grab, identity, 1.f, 1.f, .5f, .5f, down));
    assert(drag.active());
    const auto still = drag.update(down);
    assert(still); near(still->dx, 0); near(still->dy, 0); near(still->factor, 1);
    auto moved = drag.update(controller_at(.23f, -.16f));
    assert(moved); near(moved->dx, .23f); near(moved->dy, -.16f); near(moved->factor, 1);
    // The original plane is fixed: the ray may hit outside the initial canvas.
    moved = drag.update(controller_at(2.f, 1.f));
    assert(moved); near(moved->dx, 2.f); near(moved->dy, 1.f);
    drag.reset(); assert(!drag.active()); assert(!drag.update(down));

    // Independent horizontal, vertical, and diagonal anchor-based scaling;
    // both contraction and growth, including intersections outside the panel.
    for (float factor : {.4f, 1.f, 1.6f, 3.f}) {
        scale_case(.5f, 0.f, factor);
        scale_case(0.f, .5f, factor);
        scale_case(.5f, .5f, factor);
    }
    assert(drag.begin(PanelDragKind::Scale, identity, 1, 1, .5f, .5f,
                      controller_at(0, 0)));
    // A perpendicular offset does not alter projected scale.
    moved = drag.update(controller_at(.25f, .25f));
    assert(moved); near(moved->factor, 1);
    moved = drag.update(controller_at(.5f, -.5f));
    assert(moved); near(moved->factor, 2);
    moved = drag.update(controller_at(0, 0)); // no feedback from the previous size
    assert(moved); near(moved->factor, 1);
    drag.reset();
    assert(!drag.begin(PanelDragKind::Scale, identity, 1, 1, 0, 0,
                       controller_at(-.5f, .5f))); // zero anchor baseline
    assert(drag.begin(PanelDragKind::Grab, identity, 1, 1, 0, 0,
                      controller_at(-.5f, .5f)));

    // Rotating the controller changes the calibrated ray even without moving
    // its origin. The initial ray is (.25,0,-1), rotated 90 degrees about Z.
    auto angled_down = controller_at(0, 0);
    assert(drag.begin(PanelDragKind::Grab, identity, 1, 1, .75f, .5f, angled_down));
    auto turned = angled_down;
    turned[0][0] = 0; turned[0][1] = -1;
    turned[1][0] = 1; turned[1][1] = 0;
    moved = drag.update(turned);
    assert(moved); near(moved->dx, -.25f); near(moved->dy, .25f);

    const auto panel = yawed_panel();
    // Use the same rotated basis for both controller and panel; a 0.2 m shift
    // along panel right (-Z) yields panel-right grab displacement.
    auto rotated_down = panel;
    rotated_down[0][3] += 1.f;
    assert(drag.begin(PanelDragKind::Grab, panel, 1, 1, .5f, .5f, rotated_down));
    moved = drag.update(translated(rotated_down, 0, -.15f, -.2f));
    assert(moved); near(moved->dx, .2f); near(moved->dy, -.15f);
    auto corner_ray = rotated_down;
    corner_ray[1][3] += .25f;
    corner_ray[2][3] += .25f; // initial hit at x=.25, y=.25
    assert(drag.begin(PanelDragKind::Scale, panel, 1, 1, .25f, .25f, corner_ray));
    moved = drag.update(translated(corner_ray, 0, -.25f, -.25f));
    assert(moved); near(moved->factor, 2.f);

    // Relative poses are valid without converting them to world space: both
    // inputs here share a rotated/translated parent coordinate system.
    auto relative_panel = identity;
    relative_panel[0][3] = -.3f; relative_panel[1][3] = .1f;
    auto relative_controller = translated(relative_panel, 0, 0, .8f);
    assert(drag.begin(PanelDragKind::Grab, relative_panel, .4f, .2f,
                      .5f, .5f, relative_controller));
    moved = drag.update(translated(relative_controller, .08f, -.04f, 0));
    assert(moved); near(moved->dx, .08f); near(moved->dy, -.04f);
    // Express the same poses in world coordinates under a yawed parent:
    // (x, y, z) -> (2+z, 3+y, 4-x). The update is invariant.
    auto world_panel = yawed_panel();
    world_panel[1][3] += .1f; world_panel[2][3] += .3f;
    auto world_controller = translated(world_panel, .8f, 0, 0);
    assert(drag.begin(PanelDragKind::Grab, world_panel, .4f, .2f,
                      .5f, .5f, world_controller));
    moved = drag.update(translated(world_controller, 0, -.04f, -.08f));
    assert(moved); near(moved->dx, .08f); near(moved->dy, -.04f);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    auto bad = identity; bad[2][3] = nan;
    assert(!drag.begin(PanelDragKind::Grab, bad, 1, 1, .5f, .5f, down));
    assert(!drag.active()); assert(!drag.update(down));
    bad = identity; bad[0][0] = 2; // not rigid
    assert(!drag.begin(PanelDragKind::Grab, bad, 1, 1, .5f, .5f, down));
    bad = down; bad[0][3] = inf;
    assert(!drag.begin(PanelDragKind::Grab, identity, 1, 1, .5f, .5f, bad));
    bad = down; bad[0][0] = -1; // reflection, not a proper rotation
    assert(!drag.begin(PanelDragKind::Grab, identity, 1, 1, .5f, .5f, bad));
    assert(!drag.begin(PanelDragKind::Grab, identity, 0, 1, .5f, .5f, down));
    assert(!drag.begin(PanelDragKind::Grab, identity, inf, 1, .5f, .5f, down));
    assert(!drag.begin(PanelDragKind::Grab, identity, 1, 1, nan, .5f, down));
    assert(!drag.begin(PanelDragKind::Grab, identity, 1, 1, -1, .5f, down));
    assert(!drag.begin(PanelDragKind::Grab, identity, 1, 1, .5f, .5f, identity)); // zero ray
    assert(!drag.begin(PanelDragKind::Grab, identity, 1, 1, .5f, .5f,
                       controller_at(1.f, 0.f, 1e-7f))); // near parallel at down
    assert(!drag.begin(PanelDragKind::Grab, identity, 1, 1, .5f, .5f,
                       controller_at(0, 0, 11.f))); // beyond 10 m
    assert(drag.begin(PanelDragKind::Grab, identity, 1, 1, .5f, .5f, down));
    bad = down; bad[1][2] = inf;
    assert(!drag.update(bad));
    bad = down; bad[0][0] = 0;
    assert(!drag.update(bad));
    assert(!drag.update(controller_at(0, 0, -1.f))); // behind ray source
    assert(!drag.update(controller_at(0, 0, 11.f))); // beyond 10 m
    // Rotate -Z to -X: ray parallel to initial panel plane.
    bad = down;
    bad[0][0] = 0; bad[0][2] = 1;
    bad[2][0] = -1; bad[2][2] = 0;
    assert(!drag.update(bad));
    assert(drag.active()); // invalid update leaves original calibration intact
    assert(drag.update(down));
    drag.reset(); assert(!drag.active());
}
