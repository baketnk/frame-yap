#include "panel_surface.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

using namespace frameyap;
namespace {
SurfaceEvent click(PanelSurface& surface, float x, float y, unsigned cursor = 0) {
    surface.pointer_down(cursor, x, y);
    return surface.pointer_up(cursor, x, y);
}
void no_action(const SurfaceEvent& event) {
    assert(!event.action && !event.mount && !event.lasers_anytime && !event.advanced_debug &&
           !event.recenter && !event.open_bindings);
}
void snapshot(PanelSurface& surface, const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    assert(out);
    out << "P6\n" << PanelSurface::width << ' ' << PanelSurface::height << "\n255\n";
    const auto& pixels = surface.pixels();
    for (size_t i = 0; i < pixels.size(); i += 4) out.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
    assert(out);
}
}
int main(int argc, char** argv) {
    assert(argc >= 2);
    PanelSurface surface(argv[1], Mount::World);
    Panel p{"Ready to record", "", "Focus your destination before Insert. Enter is always separate.", false, false};
    assert(surface.render(p));
    assert(surface.pixels().size() == size_t(PanelSurface::width * PanelSurface::height * 4));
    assert(!surface.render(p));
    surface.reset_pointers();
    assert(!surface.render(p)); // repeated inactive-input resets do not force uploads
    assert(surface.pixels()[3] == 0); // rounded outer corner is transparent
    assert(surface.pixels()[(40 * PanelSurface::width + 40) * 4 + 3] == 255);
    Theme custom;
    custom.background = {75, 30, 100, 255};
    custom.accent = {245, 110, 15, 255};
    PanelSurface themed(argv[1], Mount::World, custom);
    assert(themed.render(p));
    const auto pixel = size_t((190 * PanelSurface::width + 500) * 4);
    assert(themed.pixels()[pixel] == 75 && themed.pixels()[pixel + 1] == 30 && themed.pixels()[pixel + 2] == 100);
    assert(surface.pixels()[pixel] != themed.pixels()[pixel]);
    // Disabled controls never emit insertion/submission. Retry/Cancel remain available.
    no_action(click(surface, 480, 610)); no_action(click(surface, 680, 610));
    no_action(click(surface, 32, 574)); // clipped visual corners are not invisible hit targets
    no_action(click(surface, 207, 575));
    assert(click(surface, 100, 610).action == UiAction::BeginRecord);
    assert(!surface.render(p)); // a diagnostic click does not change the canvas
    assert(click(surface, 280, 610).action == UiAction::Cancel);
    assert(!surface.render(p));
    no_action(surface.pointer_up(0, 100, 610));
    surface.pointer_down(0, 100, 610);
    no_action(surface.pointer_up(0, 280, 610));
    surface.pointer_down(0, 100, 610);
    surface.reset_pointers();
    no_action(surface.pointer_up(0, 100, 610));
    no_action(click(surface, 100, 610, 2));
    no_action(click(surface, std::numeric_limits<float>::quiet_NaN(), 610));
    no_action(click(surface, -1, 610));
    no_action(click(surface, 1000, 680));

    p.enabled = true;
    p.record_available = false;
    p.status = "Review your words";
    p.transcript = "A quieter way to type in VR.\nKeep the menu where it feels comfortable.\nNothing is sent until you choose Insert.";
    assert(surface.render(p));
    surface.reset_pointers(); surface.render(p);
    if (argc >= 3) snapshot(surface, std::string(argv[2]) + "-review.ppm");
    assert(click(surface, 480, 610).action == UiAction::Insert);
    assert(click(surface, 680, 610).action == UiAction::Enter);
    // Down/up must use the same controller, and release rechecks availability.
    surface.pointer_down(0, 480, 610);
    no_action(surface.pointer_up(1, 480, 610));
    p.recording = true; p.status = "Recording - 00:04";
    surface.render(p);
    no_action(surface.pointer_up(0, 480, 610));
    no_action(click(surface, 680, 610));
    assert(click(surface, 100, 610).action == UiAction::EndRecord); // explicit Stop, never a stale toggle
    surface.reset_pointers(); surface.render(p);
    if (argc >= 3) snapshot(surface, std::string(argv[2]) + "-recording.ppm");
    surface.pointer_down(0, 100, 610); // an old Stop release must not begin recording again
    p.recording = false; p.status = "Review your words";
    surface.render(p);
    no_action(surface.pointer_up(0, 100, 610));
    p.record_available = false; p.enabled = false; // warming/transcribing
    surface.render(p);
    no_action(click(surface, 100, 610)); no_action(click(surface, 680, 610));
    assert(click(surface, 280, 610).action == UiAction::Cancel);
    p.enabled = true; // review: Insert/Enter available, Record waits for discard/insertion
    surface.render(p);
    no_action(click(surface, 100, 610));
    assert(click(surface, 480, 610).action == UiAction::Insert);
    surface.render(p);

    // Settings replace preview in the same RGBA surface; footer remains available.
    no_action(click(surface, 290, 160));
    surface.render(p);
    surface.reset_pointers(); surface.render(p);
    if (argc >= 3) snapshot(surface, std::string(argv[2]) + "-settings.ppm");
    assert(click(surface, 200, 420).recenter);
    auto event = click(surface, 180, 332);
    assert(event.mount == Mount::LeftWrist && !event.action);
    no_action(click(surface, 200, 420)); // recenter is world-only
    assert(click(surface, 680, 332).mount == Mount::RightWrist);
    assert(click(surface, 680, 260).mount == Mount::Head);
    assert(click(surface, 180, 260).mount == Mount::World);
    auto laser = click(surface, 680, 420);
    assert(laser.lasers_anytime == true && !laser.action && !laser.mount);
    surface.set_lasers_anytime(true); assert(surface.render(p));
    laser = click(surface, 680, 420);
    assert(laser.lasers_anytime == false && !laser.action && !laser.mount);
    surface.set_lasers_anytime(false); assert(surface.render(p));
    auto debug = click(surface, 680, 474);
    assert(debug.advanced_debug == true && !debug.action && !debug.mount && !debug.lasers_anytime);
    assert(!surface.render(p)); // an event is only a request; caller sets the accepted value
    surface.set_advanced_debug(true); assert(surface.render(p)); assert(!surface.render(p));
    surface.pointer_down(1, 680, 474);
    surface.set_advanced_debug(false); assert(surface.render(p));
    no_action(surface.pointer_up(1, 680, 474)); // stale press cannot toggle after state change
    debug = click(surface, 680, 474);
    assert(debug.advanced_debug == true);
    surface.set_advanced_debug(true); assert(surface.render(p));
    debug = click(surface, 680, 474);
    assert(debug.advanced_debug == false);
    surface.set_advanced_debug(false); assert(surface.render(p));
    assert(click(surface, 280, 610).action == UiAction::Cancel);
    surface.set_placement_note("Wrist not tracked - using world space until it returns.");
    assert(surface.render(p)); assert(!surface.render(p));
    surface.set_placement_note("Wrist not tracked - using world space until it returns.");
    assert(!surface.render(p));
    // Switching tabs invalidates the other pointer's old press.
    surface.pointer_down(1, 680, 260);
    no_action(click(surface, 100, 160));
    no_action(surface.pointer_up(1, 680, 260));
    no_action(click(surface, 680, 260)); // mount controls not active on Review
    no_action(click(surface, 680, 420)); // laser toggle only exists on Settings
    no_action(click(surface, 680, 474)); // debug toggle only exists on Settings

    // Bindings opens SteamVR directly, from either tab, without replacing review.
    surface.pointer_down(1, 480, 610);
    auto editor = click(surface, 500, 160);
    assert(editor.open_bindings && !editor.action && !editor.mount && !editor.recenter && !editor.lasers_anytime);
    no_action(surface.pointer_up(1, 480, 610)); // opening editor invalidates old approval
    no_action(surface.pointer_up(0, 500, 160)); // one launch per deliberate click
    surface.set_binding_note("SteamVR could not open bindings. Try its controller settings.");
    assert(surface.render(p)); assert(!surface.render(p));
    assert(click(surface, 480, 610).action == UiAction::Insert);
    assert(click(surface, 680, 610).action == UiAction::Enter);
    assert(click(surface, 280, 610).action == UiAction::Cancel);
    no_action(click(surface, 100, 160));
    editor = click(surface, 500, 160);
    assert(editor.open_bindings && !editor.action);
    no_action(click(surface, 200, 434)); // old secondary editor button is gone
    no_action(click(surface, 290, 160)); // switch to Settings
    editor = click(surface, 500, 160);
    assert(editor.open_bindings && !editor.action && !editor.mount);
    assert(click(surface, 680, 260).mount == Mount::Head); // still in Settings
    no_action(click(surface, 100, 160));

    // Long UTF-8, newlines and malformed bytes are bounded, paginated and navigable.
    p.transcript.clear();
    for (int i = 0; i < 60; ++i) p.transcript += "Line " + std::to_string(i) + ": café / 日本語 / naïve / Ω\n";
    p.transcript += std::string("invalid: \xff\xc0\xe0\x80", 13);
    surface.render(p);
    surface.reset_pointers(); surface.render(p);
    const auto first = surface.pixels();
    no_action(click(surface, 900, 430));
    surface.reset_pointers(); assert(surface.render(p));
    assert(surface.pixels() != first);
    no_action(click(surface, 90, 430));
    surface.reset_pointers(); surface.render(p);
    assert(surface.pixels() == first);
    for (int i = 0; i < 200; ++i) { no_action(click(surface, 900, 430)); surface.render(p); }
    for (int i = 0; i < 200; ++i) { no_action(click(surface, 90, 430)); surface.render(p); }
    surface.reset_pointers(); surface.render(p);
    assert(surface.pixels() == first);
    p.transcript = "New result";
    assert(surface.render(p));
    surface.reset_pointers(); surface.render(p);
    auto replaced = surface.pixels();
    no_action(click(surface, 900, 430));
    surface.reset_pointers(); surface.render(p);
    assert(surface.pixels() == replaced);
    p.status = std::string(4096, 's'); p.detail = std::string(4096, 'd');
    assert(surface.render(p)); assert(!surface.render(p));
    // Laser motion and a press do not re-upload raw pixels. A completed
    // action still reaches the caller; meaningful panel changes redraw.
    surface.pointer_move(0, 900, 610); assert(!surface.render(p));
    surface.pointer_move(0, 900, 610); assert(!surface.render(p));
    surface.pointer_down(0, 900, 610); assert(!surface.render(p));
    assert(surface.pointer_up(0, 900, 610).action == UiAction::Quit);
    assert(!surface.render(p));
    std::cout << "panel checks passed (no OpenVR, microphone or input injection)\n";
}
