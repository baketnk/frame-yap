#include "panel_surface.hpp"
#include <cassert>
#include <ctime>
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
    assert(!event.action && !event.mount && !event.lasers_anytime && !event.advanced_debug && !event.auto_insert &&
           !event.close_mic_when_idle && !event.lock_layout && !event.clock_24h && !event.date_format && !event.recenter && !event.open_bindings && !event.model_action);
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
    // Interaction tests use the opt-out path; animation has a deterministic
    // injected-clock suite below rather than wall-time-sensitive assertions.
    PanelSurface surface(argv[1], Mount::World, {}, {.enabled = false});
    Panel p{"Ready to record", "", "Focus your destination before Type. Enter is always separate.", false, false};
    assert(surface.render(p));
    assert(surface.pixels().size() == size_t(PanelSurface::width * PanelSurface::height * 4));
    assert(!surface.render(p));
    surface.set_clock_time(std::time_t{1704211440}); // local clock changes only when rendered minute changes
    assert(surface.render(p));
    assert(!surface.render(p));
    surface.set_clock_time(std::time_t{1704211500});
    assert(surface.render(p));
    surface.reset_pointers();
    assert(!surface.render(p)); // repeated inactive-input resets do not force uploads
    assert(surface.pixels()[3] == 0); // rounded outer corner is transparent
    assert(surface.pixels()[(40 * PanelSurface::width + 40) * 4 + 3] == 255);
    Theme custom;
    custom.background = {75, 30, 100, 255};
    custom.accent = {245, 110, 15, 255};
    PanelSurface themed(argv[1], Mount::World, custom, {.enabled = false});
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
    {
        // With nothing to review, the enabled Type button is an explicit Enter.
        PanelSurface idle(argv[1], Mount::World, {}, {.enabled = false});
        Panel ready{"Ready", "", "", true, false};
        idle.render(ready);
        assert(idle.available(UiAction::Insert));
        assert(click(idle, 480, 610).action == UiAction::Insert);
        ready.recording = true; idle.render(ready);
        assert(!idle.available(UiAction::Insert));
        // Indicators repaint only on change; absent values stay hidden.
        ready.recording = false; idle.render(ready);
        const auto plain = idle.pixels();
        StatusIndicators shown{true, BatteryLevel{82, false}, BatteryLevel{15, false}, BatteryLevel{100, true}};
        idle.set_indicators(shown);
        assert(idle.render(ready) && idle.pixels() != plain);
        idle.set_indicators(shown); assert(!idle.render(ready));
        if (argc >= 3) snapshot(idle, std::string(argv[2]) + "-indicators.ppm");
        shown.dashboard_open = false; idle.set_indicators(shown);
        assert(idle.render(ready));
        if (argc >= 3) snapshot(idle, std::string(argv[2]) + "-indicators-closed.ppm");
        idle.set_indicators({}); assert(idle.render(ready) && idle.pixels() == plain);
    }
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
    // Outside handles capture one cursor, invalidate all previous approvals,
    // and never activate Quit/Record on release. No repaint during manipulation.
    assert(surface.pointer_down(0, 1028, 712) == PanelDragKind::Scale);
    assert(surface.dragging(0) && !surface.dragging(1));
    assert(!surface.pointer_down(1, 500, 724));
    no_action(surface.pointer_up(1, 1028, 712));
    assert(surface.dragging(0));
    no_action(surface.pointer_up(0, 900, 610));
    assert(!surface.dragging(0));
    assert(!surface.render(p));
    surface.pointer_down(1, 100, 610);
    assert(surface.pointer_down(0, 500, 724) == PanelDragKind::Grab);
    no_action(surface.pointer_up(1, 100, 610));
    no_action(surface.pointer_up(0, 100, 610));
    no_action(surface.pointer_up(1, 100, 610));
    assert(surface.pointer_down(0, 1028, 712) == PanelDragKind::Scale);
    surface.reset_pointers();
    assert(!surface.dragging(0));
    no_action(surface.pointer_up(0, 1028, 712));
    no_action(click(surface, 951, 656)); // retired inset grip
    no_action(click(surface, 800, 724)); // empty transparent margin
    assert(!surface.pointer_down(0, 996, 680)); // clipped handle corner
    const auto alpha = [&](int x, int y) { return surface.pixels()[(y * PanelSurface::width + x) * 4 + 3]; };
    assert(alpha(500, 723) == 255 && alpha(1040, 705) == 255);
    assert(alpha(800, 724) == 0 && alpha(1050, 760) == 0);
    assert(alpha(500, 690) == 0 && alpha(1030, 704) == 0);
    // Antialiased handle edges carry real alpha, not RGB hidden at alpha zero.
    assert(alpha(401, 721) > 0 && alpha(401, 721) < 255);
    // Both external handles use the configured full frame gradient, not a
    // fixed blue or a tint that fills in their transparent surroundings.
    Theme gradient_theme;
    gradient_theme.frame_start = {255, 0, 0, 255};
    gradient_theme.frame_end = {0, 0, 255, 255};
    PanelSurface gradient_surface(argv[1], Mount::World, gradient_theme, {.enabled = false});
    gradient_surface.render(p);
    const auto channel = [&](int x, int y, int c) {
        return gradient_surface.pixels()[(y * PanelSurface::width + x) * 4 + c];
    };
    assert(channel(410, 723, 0) > 230 && channel(590, 723, 2) > 230);
    assert(channel(1012, 723, 0) > 220 && channel(1040, 705, 2) > 220);
    assert(channel(1030, 704, 3) == 0 && channel(800, 724, 3) == 0);
    assert(channel(401, 721, 3) == alpha(401, 721));
    {
        // Animated colors share one global field; timestamps never depend on the
        // civil clock, and full cycles return byte-identical pixels (including alpha).
        using namespace std::chrono_literals;
        const auto epoch = PanelSurface::Clock::time_point{};
        PanelSurface animated(argv[1], Mount::World, gradient_theme);
        animated.set_clock_time(std::time_t{1704211440});
        assert(animated.render(p, epoch));
        const auto first = animated.pixels();
        const auto rgb = [](const auto& pixels, int x, int y) {
            const auto i = (y * PanelSurface::width + x) * 4;
            return Rgba{pixels[i], pixels[i + 1], pixels[i + 2], 255};
        };
        assert(rgb(first, 100, 190) != rgb(first, 500, 190)); // spatial, not a flat color fade
        for (int x : {410, 500, 590})
            assert(rgb(first, x, 723) == rgb(first, x, 10)); // handle and perimeter same phase
        const auto bg = rgb(first, 500, 190), edge = rgb(first, 500, 10);
        for (int k = 0; k < 3; ++k)
            assert(bg[k] == static_cast<unsigned char>(gradient_theme.background[k] * .88f + edge[k] * .12f));
        assert(!animated.render(p, epoch + 99ms));
        assert(animated.render(p, epoch + 100ms));
        assert(!animated.render(p, epoch + 150ms));
        assert(animated.render(p, epoch + 7500ms));
        const auto quarter = animated.pixels();
        assert(rgb(quarter, 500, 190) != bg);
        assert(rgb(quarter, 500, 10) != edge);
        assert(rgb(quarter, 500, 723) != rgb(first, 500, 723));
        assert(rgb(quarter, 1040, 705) != rgb(first, 1040, 705));
        for (size_t i = 3; i < first.size(); i += 4) assert(first[i] == quarter[i]);
        assert(animated.render(p, epoch + 29900ms));
        const auto before_loop = animated.pixels();
        assert(animated.render(p, epoch + 30s));
        assert(animated.pixels() == first);
        assert(animated.render(p, epoch + 30100ms));
        for (auto point : {std::pair{500, 190}, std::pair{500, 10}, std::pair{500, 723}, std::pair{1040, 705}}) {
            const auto a = rgb(before_loop, point.first, point.second);
            const auto b = rgb(first, point.first, point.second);
            const auto c = rgb(animated.pixels(), point.first, point.second);
            for (int k = 0; k < 3; ++k) {
                assert(std::abs(int(a[k]) - b[k]) <= 3);
                assert(std::abs(int(c[k]) - b[k]) <= 3);
                assert(std::abs((int(b[k]) - a[k]) - (int(c[k]) - b[k])) <= 2);
            }
        }
        // Hidden panels skip animation; showing again catches up without replaying
        // missed frames. A frame never cancels an already approved pointer press.
        assert(!animated.render(p, epoch + 40s, false));
        assert(animated.render(p, epoch + 40s));
        animated.pointer_down(0, 100, 610, epoch + 40s);
        assert(animated.render(p, epoch + 41s));
        assert(animated.pointer_up(0, 100, 610, epoch + 41s).action == UiAction::BeginRecord);
        assert(!gradient_surface.render(p, epoch + 300s)); // disabled has no timed redraws
        PanelSurface faster(argv[1], Mount::World, gradient_theme, {.period_seconds = 10.f, .strength = 0.f});
        assert(faster.render(p, epoch));
        const auto fast_first = faster.pixels();
        assert(rgb(fast_first, 500, 190) == gradient_theme.background);
        assert(faster.render(p, epoch + 2500ms) && faster.pixels() != fast_first);
        assert(faster.render(p, epoch + 10s) && faster.pixels() == fast_first);
        if (argc >= 3) {
            PanelSurface preview(argv[1], Mount::World);
            preview.render(p, epoch);
            snapshot(preview, std::string(argv[2]) + "-gradient.ppm");
        }
    }
    auto regions = surface.input_regions();
    assert(regions.size() == 3);
    assert(regions[2].x == 996 && regions[2].y == 680); // masks use TOP-left, not GL mouse Y
    surface.pointer_down(0, 500, 724);
    surface.set_layout_locked(true);
    assert(!surface.dragging(0));
    assert(surface.render(p));
    assert(alpha(500, 723) == 0 && alpha(1040, 705) == 0);
    assert(surface.input_regions().size() == 1);
    assert(!surface.pointer_down(0, 500, 724));
    assert(!surface.pointer_down(0, 1028, 712));
    no_action(surface.pointer_up(0, 900, 610));
    assert(!surface.render(p));
    surface.set_layout_locked(false); assert(surface.render(p));
    assert(alpha(500, 723) == 255 && alpha(1040, 705) == 255);

    p.enabled = true;
    p.record_available = false;
    p.status = "Review your words";
    p.transcript = "A quieter way to type in VR.\nKeep the menu where it feels comfortable.\nNothing is sent until you choose Type.";
    assert(surface.render(p));
    surface.reset_pointers(); surface.render(p);
    if (argc >= 3) snapshot(surface, std::string(argv[2]) + "-review.ppm");
    assert(click(surface, 480, 610).action == UiAction::Insert);
    assert(click(surface, 680, 610).action == UiAction::Enter);
    p.quick_inputs = {"/new", "/questions", "/help"};
    p.quick_open = true;
    assert(surface.render(p));
    const auto first_quick = surface.pixels();
    no_action(click(surface, 900, 518)); // review pagination hidden behind picker
    p.quick_selected = 1;
    assert(surface.render(p) && surface.pixels() != first_quick);
    assert(click(surface, 680, 610).action == UiAction::Enter);
    assert(click(surface, 280, 610).action == UiAction::Cancel);
    no_action(click(surface, 480, 610));
    p.quick_open = false; p.quick_selected = 0;
    assert(surface.render(p));
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
    auto lock = click(surface, 790, 160);
    assert(lock.lock_layout == true && !lock.action);
    assert(!surface.render(p)); // caller applies/persists the requested lock
    surface.set_layout_locked(true); assert(surface.render(p));
    lock = click(surface, 790, 160);
    assert(lock.lock_layout == false); // unlock stays accessible in Settings
    surface.set_layout_locked(false); assert(surface.render(p));
    assert(click(surface, 200, 420).recenter);
    auto event = click(surface, 180, 332);
    assert(event.mount == Mount::LeftWrist && !event.action);
    no_action(click(surface, 200, 420)); // recenter is world-only
    assert(click(surface, 680, 332).mount == Mount::RightWrist);
    assert(click(surface, 680, 260).mount == Mount::Head);
    assert(click(surface, 180, 260).mount == Mount::World);
    auto laser = click(surface, 680, 400);
    assert(laser.lasers_anytime == true && !laser.action && !laser.mount);
    surface.set_lasers_anytime(true); assert(surface.render(p));
    laser = click(surface, 680, 400);
    assert(laser.lasers_anytime == false && !laser.action && !laser.mount);
    surface.set_lasers_anytime(false); assert(surface.render(p));
    auto automatic = click(surface, 200, 446);
    assert(automatic.auto_insert == true && !automatic.action);
    assert(!surface.render(p)); // request alone has no effect
    surface.set_auto_insert(true); assert(surface.render(p));
    automatic = click(surface, 200, 446);
    assert(automatic.auto_insert == false);
    surface.set_auto_insert(false); assert(surface.render(p));
    auto debug = click(surface, 680, 446);
    assert(debug.advanced_debug == true && !debug.action && !debug.mount && !debug.lasers_anytime);
    assert(!surface.render(p)); // an event is only a request; caller sets the accepted value
    surface.set_advanced_debug(true); assert(surface.render(p)); assert(!surface.render(p));
    surface.pointer_down(1, 680, 446);
    surface.set_advanced_debug(false); assert(surface.render(p));
    no_action(surface.pointer_up(1, 680, 446)); // stale press cannot toggle after state change
    debug = click(surface, 680, 446);
    assert(debug.advanced_debug == true);
    surface.set_advanced_debug(true); assert(surface.render(p));
    debug = click(surface, 680, 446);
    assert(debug.advanced_debug == false);
    surface.set_advanced_debug(false); assert(surface.render(p));
    auto clock = click(surface, 200, 492);
    assert(clock.clock_24h == true && !clock.action);
    assert(!surface.render(p)); // setting request waits for caller's accepted value
    surface.set_clock_24h(true); assert(surface.render(p));
    assert(click(surface, 200, 492).clock_24h == false);
    surface.set_clock_24h(false); assert(surface.render(p));
    for (DateFormat format : {DateFormat::DayMonthYear, DateFormat::Iso, DateFormat::Off, DateFormat::MonthDayYear}) {
        auto date = click(surface, 680, 492);
        assert(date.date_format == format && !date.action);
        surface.set_date_format(format);
        assert(surface.render(p));
    }
    auto mic = click(surface, 200, 532);
    assert(mic.close_mic_when_idle == true && !mic.action);
    assert(!surface.render(p)); // only the caller can accept a setting event
    surface.set_close_mic_when_idle(true); assert(surface.render(p));
    surface.pointer_down(1, 200, 532);
    surface.set_close_mic_when_idle(false); assert(surface.render(p));
    no_action(surface.pointer_up(1, 200, 532)); // stale press cannot change mic policy
    mic = click(surface, 200, 532);
    assert(mic.close_mic_when_idle == true);
    surface.set_close_mic_when_idle(true); assert(surface.render(p));
    assert(click(surface, 200, 532).close_mic_when_idle == false);
    surface.set_close_mic_when_idle(false); assert(surface.render(p));
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
    no_action(click(surface, 680, 400)); // laser toggle only exists on Settings
    no_action(click(surface, 680, 446)); // debug toggle only exists on Settings
    no_action(click(surface, 200, 446)); // auto insert only exists on Settings
    no_action(click(surface, 790, 160)); // layout lock only exists on Settings
    no_action(click(surface, 200, 492)); // clock/date controls only exist on Settings
    no_action(click(surface, 680, 492));
    no_action(click(surface, 200, 532)); // mic preference only exists on Settings

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
    no_action(click(surface, 900, 518));
    surface.reset_pointers(); assert(surface.render(p));
    assert(surface.pixels() != first);
    no_action(click(surface, 90, 518));
    surface.reset_pointers(); surface.render(p);
    assert(surface.pixels() == first);
    for (int i = 0; i < 200; ++i) { no_action(click(surface, 900, 518)); surface.render(p); }
    for (int i = 0; i < 200; ++i) { no_action(click(surface, 90, 518)); surface.render(p); }
    surface.reset_pointers(); surface.render(p);
    assert(surface.pixels() == first);
    p.transcript = "New result";
    assert(surface.render(p));
    surface.reset_pointers(); surface.render(p);
    auto replaced = surface.pixels();
    no_action(click(surface, 900, 518));
    surface.reset_pointers(); surface.render(p);
    assert(surface.pixels() == replaced);
    p.status = std::string(4096, 's'); p.detail = std::string(4096, 'd');
    assert(surface.render(p)); assert(!surface.render(p));
    // A press does not re-upload pixels. A completed action still reaches the
    // caller; meaningful panel changes redraw.
    assert(!surface.render(p));
    const auto at = PanelSurface::Clock::now();
    surface.pointer_down(0, 900, 610, at);
    no_action(surface.pointer_up(0, 900, 610, at + PanelSurface::quit_hold - std::chrono::milliseconds(1)));
    surface.pointer_down(0, 900, 610, at);
    no_action(surface.pointer_up(0, 280, 610, at + PanelSurface::quit_hold));
    surface.pointer_down(0, 900, 610, at);
    surface.reset_pointers();
    no_action(surface.pointer_up(0, 900, 610, at + PanelSurface::quit_hold));
    surface.pointer_down(0, 900, 610, at);
    assert(surface.pointer_up(0, 900, 610, at + PanelSurface::quit_hold).action == UiAction::Quit);
    assert(surface.render(p));
    assert(!surface.render(p));
    // Model page: selection is separate from installation; the first Install
    // click reveals the source/size/license and cannot launch a child.
    PanelSurface chooser(argv[1], Mount::World, {}, {.enabled = false});
    Panel model_panel{"Ready", "", "", true, false};
    model_panel.selected_backend = "redux";
    model_panel.models = {{"redux", "Parakeet Redux", "not_installed", "https://example.org/model",
                           "CC-BY-4.0", "Pinned license text", "Attribution", 177774490, false, std::string(64, 'a')},
                          {"fake", "Fixture", "installed_verified", "https://example.org/fake",
                           "MIT", "Fixture license", "Fixture", 7, true, std::string(64, 'b')}};
    chooser.render(model_panel);
    no_action(click(chooser, 290, 160)); // Settings
    chooser.render(model_panel);
    no_action(click(chooser, 680, 530)); // Models
    chooser.render(model_panel);
    auto select = click(chooser, 100, 310);
    assert(select.model_action && select.model_action->id == "fake" && !select.model_action->install);
    model_panel.selected_backend = "fake";
    chooser.render(model_panel);
    no_action(click(chooser, 700, 546)); // verified model cannot be installed
    select = click(chooser, 100, 260);
    assert(select.model_action && select.model_action->id == "redux" && !select.model_action->install);
    model_panel.selected_backend = "redux";
    chooser.render(model_panel);
    no_action(click(chooser, 700, 546)); // consent preview only
    chooser.render(model_panel);
    no_action(click(chooser, 700, 546)); // first page cannot authorize installation
    while (true) {
        auto rows = chooser.visible_model_review_lines();
        assert(!rows.empty());
        auto before = chooser.pixels();
        no_action(click(chooser, 370, 546));
        if (!chooser.render(model_panel)) break; // final page has disabled Next
        assert(chooser.pixels() != before);
    }
    auto install = click(chooser, 700, 546);
    assert(install.model_action && install.model_action->id == "redux" && install.model_action->install &&
           install.model_action->manifest_sha256 == std::string(64, 'a'));
    // Same ID with a new source or fingerprint invalidates displayed approval.
    no_action(click(chooser, 700, 546));
    chooser.render(model_panel);
    model_panel.models[0].source = "https://example.org/changed";
    model_panel.models[0].manifest_sha256 = std::string(64, 'c');
    chooser.render(model_panel);
    no_action(click(chooser, 700, 546)); // now only a new preview, no install
    chooser.render(model_panel);
    while (true) {
        auto before = chooser.pixels();
        no_action(click(chooser, 370, 546));
        if (!chooser.render(model_panel)) break;
        assert(chooser.pixels() != before);
    }
    install = click(chooser, 700, 546);
    assert(install.model_action && install.model_action->manifest_sha256 == std::string(64, 'c'));
    // Max-length metadata must have every byte represented on navigable pages,
    // including the source tail, long license text, attribution and exact hash.
    PanelSurface long_review(argv[1], Mount::World, {}, {.enabled = false});
    Panel long_panel{"Ready", "", "", true, false};
    long_panel.selected_backend = "redux";
    auto long_model = model_panel.models[0];
    long_model.source = "https://" + std::string(1016, 's');
    long_model.license = std::string(1024, 'L');
    long_model.license_text = std::string(1024, 'T');
    long_model.attribution = std::string(1024, 'A');
    long_panel.models = {long_model};
    long_review.render(long_panel);
    no_action(click(long_review, 290, 160)); long_review.render(long_panel);
    no_action(click(long_review, 680, 530)); long_review.render(long_panel);
    no_action(click(long_review, 700, 546));
    assert(long_review.visible_model_review_lines().empty()); // preview not yet drawn
    no_action(click(long_review, 370, 546)); // cannot skip unpainted consent page
    long_review.render(long_panel);
    std::string reviewed;
    int pages = 0;
    for (;;) {
        auto rows = long_review.visible_model_review_lines();
        assert(!rows.empty());
        for (const auto& row : rows) reviewed += row;
        ++pages;
        if (pages == 1) no_action(click(long_review, 700, 546));
        auto before = long_review.pixels();
        no_action(click(long_review, 370, 546));
        if (long_review.visible_model_review_lines().empty()) {
            no_action(click(long_review, 700, 546)); // next page not painted yet
            no_action(click(long_review, 370, 546)); // cannot skip next unpainted page
        }
        if (!long_review.render(long_panel)) break;
        assert(long_review.pixels() != before);
    }
    assert(pages > 8);
    for (const auto& value : {long_model.source, long_model.license, long_model.license_text,
                              long_model.attribution, long_model.manifest_sha256})
        assert(reviewed.find(value) != std::string::npos);
    auto long_install = click(long_review, 700, 546);
    assert(long_install.model_action && long_install.model_action->install &&
           long_install.model_action->manifest_sha256 == long_model.manifest_sha256);
    long_panel.model_note = "Downloading: weights.dat";
    long_review.render(long_panel);
    auto without_error = long_review.pixels();
    long_panel.model_note = "Install failed: manifest_mismatch: pinned metadata changed; local model status checked offline.";
    assert(long_review.render(long_panel));
    bool footer_note_changed = false;
    for (int y = 644; y < 680; ++y) for (int x = 32; x < 968; ++x) {
        auto pixel = (size_t(y) * PanelSurface::width + x) * 4;
        footer_note_changed |= long_review.pixels()[pixel] != without_error[pixel];
    }
    assert(footer_note_changed); // feedback below buttons, never underneath them
    chooser.render(model_panel);
    chooser.pointer_down(0, 700, 546);
    model_panel.model_busy = true;
    chooser.render(model_panel);
    no_action(chooser.pointer_up(0, 700, 546)); // change invalidates approval
    std::cout << "panel checks passed (no OpenVR, microphone or input injection)\n";
}
