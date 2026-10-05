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
void near_pose(const Matrix34& actual, const Matrix34& expected) {
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col)
            near(actual[row][col], expected[row][col]);
}
Matrix34 translated(Matrix34 pose, float x, float y, float z) {
    pose[0][3] += x; pose[1][3] += y; pose[2][3] += z;
    return pose;
}
Matrix34 controller_at(float x, float y, float z = 1.f) {
    return translated(identity, x, y, z);
}
// Right-handed +90 degree rotations around each world axis.
Matrix34 pitch() {
    return Matrix34{{{{1.f, 0.f, 0.f, 0.f}},
                     {{0.f, 0.f, -1.f, 0.f}},
                     {{0.f, 1.f, 0.f, 0.f}}}};
}
Matrix34 yaw() {
    return Matrix34{{{{0.f, 0.f, 1.f, 0.f}},
                     {{0.f, 1.f, 0.f, 0.f}},
                     {{-1.f, 0.f, 0.f, 0.f}}}};
}
Matrix34 roll() {
    return Matrix34{{{{0.f, -1.f, 0.f, 0.f}},
                     {{1.f, 0.f, 0.f, 0.f}},
                     {{0.f, 0.f, 1.f, 0.f}}}};
}
void scale_case(float x, float y, float factor) {
    // 1 m square, top-left (-.5,+.5); shift controller and hence its
    // intersection by (factor - 1) times the original anchor-to-hit vector.
    const auto down = controller_at(x - .5f, .5f - y);
    PanelDrag drag;
    assert(drag.begin(PanelDragKind::Scale, identity, 1.f, 1.f, x, y, down));
    const auto same = drag.update(down);
    assert(same); near(same->factor, 1.f); near_pose(same->pose, identity);
    const auto changed = drag.update(translated(down, (factor - 1.f) * x, (1.f - factor) * y, 0));
    assert(changed); near(changed->factor, factor); near_pose(changed->pose, identity);
}
void grab_rotation_case(const Matrix34& rotation, const Matrix34& expected) {
    PanelDrag drag;
    // Nonzero lever arm: rotation must turn the panel position as well as its axes.
    const auto down = controller_at(0, 0);
    const auto panel = translated(identity, .25f, .4f, -.3f);
    assert(drag.begin(PanelDragKind::Grab, panel, 1, 1, .7f, .3f, down));
    auto turned = rotation;
    turned[2][3] = 1.f;
    const auto update = drag.update(turned);
    assert(update); near(update->factor, 1.f); near_pose(update->pose, expected);
    // No accumulation: moving back to down restores the exact initial pose.
    drag.reset();
    assert(!drag.update(turned));
    // Release while turned, then pick that rotated panel up again elsewhere.
    const auto second_down = translated(turned, .12f, -.08f, .2f);
    assert(drag.begin(PanelDragKind::Grab, update->pose, 1, 1, .4f, .6f, second_down));
    const auto restored = drag.update(second_down);
    assert(restored); near_pose(restored->pose, update->pose);
    const auto shifted = drag.update(translated(second_down, 0.f, 0.f, -.4f));
    assert(shifted); near_pose(shifted->pose, translated(update->pose, 0.f, 0.f, -.4f));
}
} // namespace

int main() {
    PanelDrag drag;
    assert(!drag.active()); assert(!drag.update(controller_at(0, 0)));
    const auto down = controller_at(0, 0);
    auto panel = translated(identity, .25f, .4f, -.3f);
    assert(drag.begin(PanelDragKind::Grab, panel, 1.f, 1.f, .5f, .5f, down));
    assert(drag.active());
    for (int i = 0; i < 10; ++i) {
        const auto still = drag.update(down);
        assert(still); near_pose(still->pose, panel); near(still->factor, 1.f);
    }
    auto moved = drag.update(controller_at(.23f, -.16f, 1.45f));
    assert(moved); near_pose(moved->pose, translated(panel, .23f, -.16f, .45f));
    const auto released_pose = moved->pose;
    drag.reset(); assert(!drag.active()); assert(!drag.update(down));
    // A subsequent grab begins at the dropped position and orientation, not the
    // old calibration; releasing leaves the caller's last pose unchanged.
    const auto rotated_pose = compose_pose(translated(yaw(), 0.f, .1f, 0.f), released_pose);
    const auto second_down = controller_at(-.2f, .1f, .9f);
    assert(drag.begin(PanelDragKind::Grab, rotated_pose, 1, 1, .5f, .5f, second_down));
    moved = drag.update(second_down);
    assert(moved); near_pose(moved->pose, rotated_pose);
    moved = drag.update(translated(second_down, -.1f, .2f, -.3f));
    assert(moved); near_pose(moved->pose, translated(rotated_pose, -.1f, .2f, -.3f));
    drag.reset(); assert(!drag.update(second_down));

    // Stick depth is independent of controller translation, preserves pose
    // rotation, and follows the captured panel normal as the controller turns.
    assert(drag.begin(PanelDragKind::Grab, panel, 1, 1, .5f, .5f, down));
    drag.step_depth(.1f, 1.); near(float(drag.depth()), 0);
    drag.step_depth(1.f, .05); near(float(drag.depth()), .03f);
    auto depth_pose = drag.update(down);
    assert(depth_pose); near_pose(depth_pose->pose, translated(panel, 0, 0, -.03f));
    depth_pose = drag.update(translated(down, .1f, .2f, 0));
    assert(depth_pose); near_pose(depth_pose->pose, translated(panel, .1f, .2f, -.03f));
    drag.step_depth(0.f, .05); near(float(drag.depth()), .03f);
    drag.step_depth(-1.f, .05); near(float(drag.depth()), 0);
    drag.step_depth(1.f, 10.); near(float(drag.depth()), .03f); // bounded time step
    drag.step_depth(std::numeric_limits<float>::quiet_NaN(), .05);
    drag.step_depth(1.f, std::numeric_limits<double>::infinity());
    near(float(drag.depth()), .03f);
    const auto turned_controller = compose_pose(down, yaw());
    depth_pose = drag.update(turned_controller);
    assert(depth_pose);
    near_pose(depth_pose->pose, translated(compose_pose(turned_controller, relative_pose(down, panel)), -.03f, 0, 0));
    for (int i = 0; i < 100; ++i) drag.step_depth(1.f, .05);
    near(float(drag.depth()), 1.5f);
    for (int i = 0; i < 100; ++i) drag.step_depth(-1.f, .05);
    near(float(drag.depth()), -1.5f);
    drag.reset(); near(float(drag.depth()), 0);
    assert(drag.begin(PanelDragKind::Scale, identity, 1, 1, .5f, .5f, down));
    drag.step_depth(1.f, .05); near(float(drag.depth()), 0);
    drag.reset();

    // A controller pointing obliquely at a rotated panel must move depth
    // perpendicular to the panel rather than along its own pointing axis.
    const auto angled_panel = translated(yaw(), .25f, .4f, -.3f);
    assert(drag.begin(PanelDragKind::Grab, angled_panel, 1, 1, .5f, .5f, down));
    drag.step_depth(1.f, .05);
    depth_pose = drag.update(down);
    assert(depth_pose); near_pose(depth_pose->pose, translated(angled_panel, -.03f, 0, 0));
    const auto angled_moved = translated(compose_pose(down, pitch()), .1f, .2f, -.1f);
    depth_pose = drag.update(angled_moved);
    auto expected_relative = relative_pose(down, angled_panel);
    expected_relative[0][3] -= .03f;
    assert(depth_pose); near_pose(depth_pose->pose, compose_pose(angled_moved, expected_relative));
    // The same elapsed movement has the same result at 50 and 100 Hz.
    for (const double dt : {.01, .02}) {
        assert(drag.begin(PanelDragKind::Grab, angled_panel, 1, 1, .5f, .5f, down));
        for (int i = 0; i < int(1. / dt); ++i) drag.step_depth(1.f, dt);
        near(float(drag.depth()), .6f);
    }
    drag.reset();

    // Compositor events are source-tagged and already time-integrated.
    assert(drag.begin(PanelDragKind::Grab, angled_panel, 1, 1, .5f, .5f, down));
    assert(!drag.queue_scroll_depth(2, 1, 1.f)); // the other hand
    assert(!drag.queue_scroll_depth(std::numeric_limits<uint32_t>::max(), 1, 1.f));
    assert(!drag.queue_scroll_depth(1, 1, std::numeric_limits<float>::quiet_NaN()));
    assert(drag.queue_scroll_depth(1, 1, .5f));
    drag.update_depth({}, .001); near(float(drag.depth()), .02f);
    drag.update_depth({}, .05); near(float(drag.depth()), .02f); // no replay
    depth_pose = drag.update(down);
    assert(depth_pose); near_pose(depth_pose->pose, translated(angled_panel, -.02f, 0, 0));
    assert(drag.queue_scroll_depth(1, 1, 100.f));
    drag.update_depth({}, 100.); near(float(drag.depth()), .05f); // capped, no dt multiplication
    assert(drag.queue_scroll_depth(1, 1, -.5f));
    drag.update_depth({}, 0); near(float(drag.depth()), .03f); // opposite direction
    assert(drag.queue_scroll_depth(1, 1, 1.f));
    drag.update_depth(0.f, .05); near(float(drag.depth()), .03f); // active centered axis wins
    assert(drag.queue_scroll_depth(1, 1, 1.f));
    drag.update_depth(1.f, .05); near(float(drag.depth()), .06f); // no double application
    for (int i = 0; i < 100; ++i) assert(drag.queue_scroll_depth(1, 1, 1.f));
    drag.update_depth({}, .01); near(float(drag.depth()), 1.5f);
    assert(drag.queue_scroll_depth(1, 1, -.5f));
    drag.reset(); // queued events cannot leak into a later grab
    assert(drag.begin(PanelDragKind::Grab, angled_panel, 1, 1, .5f, .5f, down));
    drag.update_depth({}, .05); near(float(drag.depth()), 0);
    assert(drag.begin(PanelDragKind::Scale, identity, 1, 1, .5f, .5f, down));
    assert(!drag.queue_scroll_depth(1, 1, 1.f));
    drag.update_depth({}, .05); near(float(drag.depth()), 0);
    drag.reset();
    assert(!drag.queue_scroll_depth(1, 1, 1.f));

    grab_rotation_case(pitch(), Matrix34{{{{1.f, 0.f, 0.f, .25f}},
                                                {{0.f, 0.f, -1.f, 1.3f}},
                                                {{0.f, 1.f, 0.f, 1.4f}}}});
    grab_rotation_case(yaw(), Matrix34{{{{0.f, 0.f, 1.f, -1.3f}},
                                              {{0.f, 1.f, 0.f, .4f}},
                                              {{-1.f, 0.f, 0.f, .75f}}}});
    grab_rotation_case(roll(), Matrix34{{{{0.f, -1.f, 0.f, -.4f}},
                                               {{1.f, 0.f, 0.f, .25f}},
                                               {{0.f, 0.f, 1.f, -.3f}}}});

    // Independent horizontal, vertical, and diagonal anchor-based scaling;
    // both contraction and growth, including intersections outside the panel.
    for (float factor : {.4f, 1.f, 1.6f, 3.f}) {
        scale_case(.5f, 0.f, factor);
        scale_case(0.f, .5f, factor);
        scale_case(.5f, .5f, factor);
    }
    assert(drag.begin(PanelDragKind::Scale, identity, 1, 1, .5f, .5f, down));
    moved = drag.update(controller_at(.25f, .25f)); // perpendicular to anchor vector
    assert(moved); near(moved->factor, 1);
    moved = drag.update(controller_at(.5f, -.5f));
    assert(moved); near(moved->factor, 2);
    moved = drag.update(down); // no feedback from previous size
    assert(moved); near(moved->factor, 1);
    drag.reset();
    assert(!drag.begin(PanelDragKind::Scale, identity, 1, 1, 0, 0,
                       controller_at(-.5f, .5f))); // zero anchor baseline
    assert(drag.begin(PanelDragKind::Grab, identity, 1, 1, 0, 0,
                      controller_at(-.5f, .5f)));

    // Scaling still uses the captured controller-local ray, not the grab pose.
    auto angled_down = controller_at(0, 0);
    assert(drag.begin(PanelDragKind::Scale, identity, 1, 1, .75f, .5f, angled_down));
    auto turned = angled_down;
    turned[0][0] = 0; turned[0][1] = -1;
    turned[1][0] = 1; turned[1][1] = 0;
    moved = drag.update(turned);
    assert(moved); near(moved->factor, .5f / .8125f); near_pose(moved->pose, identity);

    const auto yawed_panel = translated(yaw(), 2.f, 3.f, 4.f);
    auto corner_ray = translated(yawed_panel, 1.f, .25f, .25f);
    assert(drag.begin(PanelDragKind::Scale, yawed_panel, 1, 1, .25f, .25f, corner_ray));
    moved = drag.update(translated(corner_ray, 0, -.25f, -.25f));
    assert(moved); near(moved->factor, 2.f); near_pose(moved->pose, yawed_panel);

    // The identical grab in a device-relative frame and under an arbitrary
    // rigid parent in world space must produce equivalent world panel poses.
    const auto parent = translated(compose_pose(yaw(), pitch()), 2.f, 3.f, 4.f);
    const auto relative_panel = translated(roll(), -.3f, .1f, -.1f);
    const auto relative_controller = controller_at(.1f, -.2f, .8f);
    const auto relative_next = compose_pose(translated(yaw(), .08f, -.04f, .2f), relative_controller);
    assert(drag.begin(PanelDragKind::Grab, relative_panel, .4f, .2f,
                      .5f, .5f, relative_controller));
    moved = drag.update(relative_next);
    assert(moved);
    const auto relative_result = moved->pose;
    const auto world_panel = compose_pose(parent, relative_panel);
    const auto world_controller = compose_pose(parent, relative_controller);
    assert(drag.begin(PanelDragKind::Grab, world_panel, .4f, .2f,
                      .5f, .5f, world_controller));
    moved = drag.update(compose_pose(parent, relative_next));
    assert(moved); near_pose(moved->pose, compose_pose(parent, relative_result));
    near_pose(relative_pose(parent, moved->pose), relative_result);
    near_pose(compose_pose(world_controller, relative_pose(world_controller, world_panel)), world_panel);

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
    // Both inputs are finite but their relative translation cannot fit in Matrix34.
    const float max = std::numeric_limits<float>::max();
    assert(!drag.begin(PanelDragKind::Grab, translated(identity, -max, 0, 0),
                       1, 1, .5f, .5f, translated(identity, max, 0, 0)));
    assert(!drag.active()); assert(!drag.update(down));
    assert(drag.begin(PanelDragKind::Grab, identity, 1, 1, .5f, .5f, identity));
    assert(drag.update(identity)); // grab does not require a ray to the panel
    drag.reset();
    assert(!drag.begin(PanelDragKind::Scale, identity, 1, 1, .5f, .5f, identity)); // zero ray
    assert(!drag.begin(PanelDragKind::Scale, identity, 1, 1, .5f, .5f,
                       controller_at(1.f, 0.f, 1e-7f))); // near parallel at down
    assert(!drag.begin(PanelDragKind::Scale, identity, 1, 1, .5f, .5f,
                       controller_at(0, 0, 11.f))); // beyond 10 m
    assert(drag.begin(PanelDragKind::Scale, identity, 1, 1, .5f, .5f, down));
    bad = down; bad[1][2] = inf;
    assert(!drag.update(bad));
    bad = down; bad[0][0] = 0;
    assert(!drag.update(bad));
    assert(!drag.update(controller_at(0, 0, -1.f))); // behind ray source
    assert(!drag.update(controller_at(0, 0, 11.f))); // beyond 10 m
    bad = down;
    bad[0][0] = 0; bad[0][2] = 1;
    bad[2][0] = -1; bad[2][2] = 0;
    assert(!drag.update(bad)); // ray parallel to initial panel plane
    assert(drag.active()); // invalid update leaves original calibration intact
    assert(drag.update(down));
    drag.reset(); assert(!drag.active()); assert(!drag.update(down));
    assert(drag.begin(PanelDragKind::Grab, identity, 1, 1, .5f, .5f, down));
    bad = down; bad[2][3] = nan;
    assert(!drag.update(bad));
    assert(drag.update(down));
    drag.reset(); assert(!drag.update(down));
}
