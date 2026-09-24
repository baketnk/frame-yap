#include "panel_surface.hpp"
#include "panel_clock.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace frameyap {
namespace {
constexpr int W = PanelSurface::width, H = PanelSurface::height;
using Color = Rgba;
constexpr int CW = PanelSurface::body.w, CH = PanelSurface::body.h;
float rounded_distance(float px, float py, int x, int y, int w, int h, float radius) {
    const float dx = std::abs(px - (x + w / 2.f)) - (w / 2.f - radius);
    const float dy = std::abs(py - (y + h / 2.f)) - (h / 2.f - radius);
    return std::hypot(std::max(dx, 0.f), std::max(dy, 0.f)) +
           std::min(std::max(dx, dy), 0.f) - radius;
}
struct Rect {
    int x, y, w, h;
    bool contains(float px, float py) const {
        return std::isfinite(px) && std::isfinite(py) && px >= x && px < x + w && py >= y && py < y + h &&
               rounded_distance(px, py, x, y, w, h, std::min(16, h / 3)) <= 0.f;
    }
};
enum class Control { Review, Settings, Bindings, Prev, Next, Record, Cancel, Insert, Enter, Quit,
                     World, Left, Right, Head, Recenter, LasersAnytime, AdvancedDebug, AutoInsert,
                     Clock24h, Date, LockLayout };
enum class Tab { Review, Settings };
struct Button { Rect r; Control id; const char* label; };
constexpr std::array<Button, 21> buttons{{
    {{32, 138, 180, 46}, Control::Review, "Review"},
    {{226, 138, 180, 46}, Control::Settings, "Settings"},
    {{420, 138, 180, 46}, Control::Bindings, "Bindings"},
    {{620, 138, 348, 46}, Control::LockLayout, "Lock grab/scale"},
    {{32, 406, 154, 44}, Control::Prev, "Previous"},
    {{838, 406, 130, 44}, Control::Next, "Next"},
    {{32, 574, 176, 68}, Control::Record, "Record"},
    {{222, 574, 176, 68}, Control::Cancel, "Cancel"},
    {{412, 574, 176, 68}, Control::Insert, "Insert"},
    {{602, 574, 176, 68}, Control::Enter, "Submit"},
    {{792, 574, 176, 68}, Control::Quit, "Hold Quit"},
    {{32, 234, 454, 58}, Control::World, "World space"},
    {{514, 234, 454, 58}, Control::Head, "Head"},
    {{32, 308, 454, 58}, Control::Left, "Left wrist"},
    {{514, 308, 454, 58}, Control::Right, "Right wrist"},
    {{32, 394, 300, 50}, Control::Recenter, "Recenter in front"},
    {{514, 394, 454, 50}, Control::LasersAnytime, "Lasers anytime"},
    {{32, 452, 454, 48}, Control::AutoInsert, "Auto insert"},
    {{514, 452, 454, 48}, Control::AdvancedDebug, "Advanced debug"},
    {{32, 506, 454, 42}, Control::Clock24h, "Clock"},
    {{514, 506, 454, 42}, Control::Date, "Date"},
}};
std::optional<UiAction> action(Control c) {
    switch (c) {
    case Control::Record: return UiAction::Record;
    case Control::Cancel: return UiAction::Cancel;
    case Control::Insert: return UiAction::Insert;
    case Control::Enter: return UiAction::Enter;
    case Control::Quit: return UiAction::Quit;
    default: return {};
    }
}
std::optional<Mount> mounting(Control c) {
    switch (c) {
    case Control::World: return Mount::World;
    case Control::Left: return Mount::LeftWrist;
    case Control::Right: return Mount::RightWrist;
    case Control::Head: return Mount::Head;
    default: return {};
    }
}
// Invalid bytes become visible replacement glyphs, never control commands.
uint32_t next_codepoint(std::string_view s, size_t& i) {
    const auto a = static_cast<unsigned char>(s[i++]);
    if (a < 0x80) return a;
    int count = a >= 0xc2 && a <= 0xdf ? 1 : a >= 0xe0 && a <= 0xef ? 2 : a >= 0xf0 && a <= 0xf4 ? 3 : 0;
    if (!count || i + count > s.size()) return 0xfffd;
    uint32_t value = a & (count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
    for (int k = 0; k < count; ++k) {
        auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xc0) != 0x80) return 0xfffd;
        value = (value << 6) | (b & 0x3f);
    }
    if ((count == 1 && value < 0x80) || (count == 2 && value < 0x800) ||
        (count == 3 && value < 0x10000) || (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff)
        return 0xfffd;
    i += count;
    return value;
}
} // namespace

struct PanelSurface::Impl {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    std::vector<unsigned char> pixels = std::vector<unsigned char>(W * H * 4);
    Panel panel;
    Mount mount;
    Theme theme;
    Color background, card, ink, muted, cyan, pink;
    Tab tab = Tab::Review;
    bool dirty = true, lasers_anytime = false, advanced_debug = false, auto_insert = false;
    bool clock_24h = false, layout_locked = false;
    DateFormat date_format = DateFormat::MonthDayYear;
    std::time_t clock_time = std::time(nullptr);
    ClockLabel displayed_clock;
    std::string placement_note, binding_note;
    std::array<int, 2> pressed{{-1, -1}};
    std::array<PanelSurface::Clock::time_point, 2> press_time{};
    int hold_progress = 0;
    int drag_cursor = -1;
    std::vector<std::string> lines;
    size_t page = 0;
    static constexpr size_t lines_per_page = 4;

    Impl(const std::string& font, Mount m, Theme t)
        : mount(m), theme(t), background(t.background), card(t.card), ink(t.ink), muted(t.muted),
          cyan(t.accent), pink(t.warning) {
        if (FT_Init_FreeType(&library)) throw std::runtime_error("FreeType initialization failed");
        if (FT_New_Face(library, font.c_str(), 0, &face)) {
            FT_Done_FreeType(library);
            throw std::runtime_error("Could not load specified font: " + font);
        }
    }
    ~Impl() { FT_Done_Face(face); FT_Done_FreeType(library); }
    void size(unsigned px) {
        if (FT_Set_Pixel_Sizes(face, 0, px)) throw std::runtime_error("Could not size panel font");
    }
    Color mix(Color a, Color b, float t) const {
        Color result{};
        for (int k = 0; k < 3; ++k)
            result[k] = static_cast<unsigned char>(a[k] * (1.f - t) + b[k] * t);
        result[3] = 255;
        return result;
    }
    void rect(Rect r, Color c) {
        for (int y = std::max(r.y, 0); y < std::min(H, r.y + r.h); ++y)
            for (int x = std::max(r.x, 0); x < std::min(W, r.x + r.w); ++x)
                std::copy(c.begin(), c.end(), pixels.begin() + (size_t(y) * W + x) * 4);
    }
    void blend(int x, int y, Color color, float amount) {
        auto* dst = pixels.data() + (size_t(y) * W + x) * 4;
        // Straight-alpha source-over, including strokes in the transparent margin.
        const float alpha = amount * color[3] / 255.f;
        const float old_alpha = dst[3] / 255.f;
        const float out_alpha = alpha + old_alpha * (1.f - alpha);
        if (out_alpha <= 0.f) return;
        for (int k = 0; k < 3; ++k)
            dst[k] = static_cast<unsigned char>((dst[k] * old_alpha * (1.f - alpha) + color[k] * alpha) / out_alpha);
        dst[3] = static_cast<unsigned char>(255.f * out_alpha);
    }
    void rounded(Rect r, int radius, Color fill, Color edge, float glow = 0.f, int stroke = 1) {
        // Distance-field antialiasing and restrained baked glow. No second texture or GPU pass.
        constexpr int halo = 10;
        for (int y = std::max(0, r.y - halo); y < std::min(H, r.y + r.h + halo); ++y)
            for (int x = std::max(0, r.x - halo); x < std::min(W, r.x + r.w + halo); ++x) {
                const float d = rounded_distance(x + .5f, y + .5f, r.x, r.y, r.w, r.h, radius);
                if (glow > 0.f && d > 0.f && d < halo) {
                    const float falloff = 1.f - d / halo;
                    blend(x, y, edge, glow * falloff * falloff);
                }
                if (d < .5f) blend(x, y, fill, std::clamp(.5f - d, 0.f, 1.f));
                if (d > -stroke - .5f && d < .5f)
                    blend(x, y, edge, std::clamp(d + stroke + .5f, 0.f, 1.f) *
                                       std::clamp(.5f - d, 0.f, 1.f));
            }
    }
    void frame() {
        // Independently rasterized version of kouseki's HUD visual language:
        // rounded mint-to-blue perimeter and a second shallow curved accent.
        for (int y = 0; y < CH; ++y) for (int x = 0; x < CW; ++x) {
            if (x > 34 && x < CW - 34 && y > 34 && y < CH - 34) continue;
            const float distance = rounded_distance(x, y, 10, 10, CW - 20, CH - 20, 18.f);
            const float edge = std::abs(distance);
            const float strength = edge <= 1.f ? 1.f : .30f * std::max(0.f, 1.f - (edge - 1.f) / 6.f);
            const float t = float(x) / CW;
            Color gradient{};
            for (int k = 0; k < 3; ++k)
                gradient[k] = static_cast<unsigned char>(theme.frame_start[k] * (1.f - t) + theme.frame_end[k] * t);
            gradient[3] = 255;
            auto* dst = pixels.data() + (size_t(y) * W + x) * 4;
            for (int k = 0; k < 3; ++k) dst[k] = static_cast<unsigned char>(background[k] + (gradient[k] - background[k]) * strength);
            dst[3] = distance <= 1.f ? 255 : static_cast<unsigned char>(255 * std::max(0.f, 1.f - (distance - 1.f) / 6.f));
        }
        for (int x = 32; x < CW - 32; ++x) {
            const float t = float(x - 32) / (CW - 64);
            const int y = CH - 21 - int(5 * std::sin(t * 3.14159265f));
            Color gradient{};
            for (int k = 0; k < 3; ++k)
                gradient[k] = static_cast<unsigned char>(theme.frame_start[k] * (1.f - t) + theme.frame_end[k] * t);
            gradient[3] = 255;
            rect({x, y, 1, 1}, gradient);
        }
    }
    int advance(uint32_t cp) {
        if (cp == '\r' || cp == '\t') return 0;
        if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT)) return 0;
        return int(face->glyph->advance.x >> 6);
    }
    std::vector<std::string> wrap(std::string_view s, unsigned px, int width) {
        size(px);
        std::vector<std::string> result;
        size_t start = 0, i = 0;
        int x = 0;
        while (i < s.size()) {
            size_t before = i;
            auto cp = next_codepoint(s, i);
            if (cp == '\n') {
                result.emplace_back(s.substr(start, before - start)); start = i; x = 0; continue;
            }
            int a = advance(cp);
            if (x + a > width && x > 0) {
                result.emplace_back(s.substr(start, before - start)); start = before; x = 0;
            }
            x += a;
        }
        result.emplace_back(s.substr(start));
        return result;
    }
    void text(std::string_view s, int x, int baseline, unsigned px, Color color, int right) {
        size(px);
        size_t i = 0;
        while (i < s.size()) {
            auto cp = next_codepoint(s, i);
            if (cp == '\n' || cp == '\r' || cp == '\t') continue;
            if (FT_Load_Char(face, cp, FT_LOAD_RENDER)) continue;
            auto glyph = face->glyph;
            int a = int(glyph->advance.x >> 6);
            if (x + a > right) break;
            const auto& b = glyph->bitmap;
            for (unsigned row = 0; row < b.rows; ++row)
                for (unsigned col = 0; col < b.width; ++col) {
                    int xx = x + glyph->bitmap_left + int(col), yy = baseline - glyph->bitmap_top + int(row);
                    if (xx < 0 || xx >= right || xx >= W || yy < 0 || yy >= H) continue;
                    unsigned char alpha = b.buffer[int(row) * b.pitch + int(col)];
                    auto* dst = pixels.data() + (size_t(yy) * W + xx) * 4;
                    for (int k = 0; k < 3; ++k)
                        dst[k] = static_cast<unsigned char>((unsigned(dst[k]) * (255 - alpha) + unsigned(color[k]) * alpha) / 255);
                }
            x += a;
        }
    }
    size_t page_count() const { return std::max(size_t(1), (lines.size() + lines_per_page - 1) / lines_per_page); }
    bool available(UiAction a) const {
        if (a == UiAction::Record) return !panel.quick_open && (panel.recording || panel.record_available);
        if (a == UiAction::Insert) return !panel.quick_open && panel.enabled && !panel.recording && !panel.transcript.empty();
        if (a == UiAction::Enter) return panel.enabled && !panel.recording;
        if (a == UiAction::QuickChat) return panel.enabled && !panel.recording && !panel.quick_inputs.empty();
        if (a == UiAction::Toggle) return false;
        return true;
    }
    bool visible(Control c) const {
        if (panel.quick_open && c != Control::Cancel && c != Control::Enter && c != Control::Quit) return false;
        if (c == Control::Prev || c == Control::Next) return tab == Tab::Review && !panel.quick_open;
        if (mounting(c) || c == Control::Recenter || c == Control::LasersAnytime ||
            c == Control::AdvancedDebug || c == Control::AutoInsert ||
            c == Control::Clock24h || c == Control::Date || c == Control::LockLayout) return tab == Tab::Settings;
        return true;
    }
    bool enabled(Control c) const {
        if (auto a = action(c)) return available(*a);
        if (c == Control::Prev) return page > 0;
        if (c == Control::Next) return page + 1 < page_count();
        if (c == Control::Recenter) return mount == Mount::World;
        return true;
    }
    int hit(float x, float y) const {
        for (size_t i = 0; i < buttons.size(); ++i)
            if (visible(buttons[i].id) && enabled(buttons[i].id) && buttons[i].r.contains(x, y)) return int(i);
        return -1;
    }
    void reset() {
        pressed.fill(-1);
        drag_cursor = -1;
    }
    bool render(const Panel& p) {
        if (p.recording != panel.recording || p.enabled != panel.enabled ||
            p.record_available != panel.record_available || p.transcript != panel.transcript) {
            reset(); // an old press cannot authorize a changed action or replacement transcript
            dirty = true;
        }
        if (p.transcript != panel.transcript || lines.empty()) {
            lines = wrap(p.transcript.empty() ? "Your words will appear here.\nReview them, then choose Insert." : p.transcript, 32, 904);
            page = 0; dirty = true;
        }
        if (p.status != panel.status || p.enabled != panel.enabled || p.recording != panel.recording ||
            p.quick_open != panel.quick_open || p.quick_selected != panel.quick_selected || p.quick_inputs != panel.quick_inputs)
            dirty = true;
        if (p.quick_open != panel.quick_open) reset();
        panel = p;
        if (panel.quick_open && tab != Tab::Review) { tab = Tab::Review; dirty = true; }
        int progress = 0;
        for (size_t cursor = 0; cursor < pressed.size(); ++cursor)
            if (pressed[cursor] == 10)
                progress = std::max(progress, std::clamp(int(std::chrono::duration_cast<std::chrono::milliseconds>(
                    PanelSurface::Clock::now() - press_time[cursor]).count() / 100) + 1, 1, 10));
        if (progress != hold_progress) { hold_progress = progress; dirty = true; }
        auto label = panel_clock(clock_time, clock_24h, date_format);
        if (label.time != displayed_clock.time || label.date != displayed_clock.date) dirty = true;
        if (!dirty) return false;
        displayed_clock = std::move(label);
        std::fill(pixels.begin(), pixels.end(), 0);
        rect({0, 0, CW, CH}, background);
        frame();
        text("FrameYap", 32, 61, 40, ink, 300);
        text(displayed_clock.time, 475, 58, 28, ink, 735);
        if (!displayed_clock.date.empty()) text(displayed_clock.date, 756, 58, 24, cyan, 968);
        rounded({32, 78, 936, 48}, 13, mix(background, card, .7f),
                panel.recording ? mix(card, pink, .36f) : mix(card, cyan, .22f),
                panel.recording ? .16f : 0.f);
        rounded({46, 95, 13, 13}, 6, panel.recording ? pink : cyan,
                panel.recording ? pink : cyan, panel.recording ? .40f : .20f);
        auto status = wrap(panel.status, 27, 790);
        text(status.front(), 70, 109, 27, ink, 844);
        if (status.size() > 1) text("[...]", 864, 109, 23, muted, 968);
        if (tab == Tab::Review && panel.quick_open) {
            rounded({32, 212, 936, 324}, 16, mix(card, muted, .08f), mix(card, cyan, .25f), .18f);
            text("QUICK CHAT    Y: NEXT   SUBMIT: SEND   CANCEL: CLOSE", 48, 243, 22, cyan, 952);
            for (size_t i = 0; i < panel.quick_inputs.size() && i < 6; ++i) {
                const Rect row{48, 254 + int(i) * 45, 904, 40};
                const bool selected = i == panel.quick_selected;
                rounded(row, 9, selected ? mix(card, cyan, .22f) : card,
                        selected ? cyan : mix(card, muted, .4f), selected ? .2f : 0.f, selected ? 2 : 1);
                text(panel.quick_inputs[i], 66, row.y + 28, 26, selected ? ink : muted, 930);
            }
        } else if (tab == Tab::Review) {
            rounded({32, 212, 936, 178}, 16, mix(card, muted, .08f), mix(card, cyan, .25f), .18f);
            for (size_t i = 0; i < lines_per_page && page * lines_per_page + i < lines.size(); ++i)
                text(lines[page * lines_per_page + i], 48, 246 + int(i) * 40, 32,
                     panel.transcript.empty() ? muted : ink, 952);
            text("PAGE " + std::to_string(page + 1) + " / " + std::to_string(page_count()), 416, 436, 23, muted, 790);
            if (!binding_note.empty()) text(binding_note, 32, 203, 20, muted, 968);
        } else {
            text("Auto insert needs stable X focus; debug logs may contain speech.", 32, 204, 18, pink, 968);
            text(placement_note.empty() ? "No automatic Enter; changing debug restarts the worker." : placement_note,
                 32, 225, 18, muted, 968);
        }
        rect({32, 550, 936, 1}, mix(card, cyan, .17f));
        for (size_t i = 0; i < buttons.size(); ++i) {
            const auto& b = buttons[i];
            if (!visible(b.id)) continue;
            bool on = enabled(b.id);
            bool selected = (b.id == Control::Review && tab == Tab::Review) ||
                            (b.id == Control::Settings && tab == Tab::Settings) ||
                            (mounting(b.id) && *mounting(b.id) == mount) ||
                            (b.id == Control::LasersAnytime && lasers_anytime) ||
                            (b.id == Control::AdvancedDebug && advanced_debug) ||
                            (b.id == Control::AutoInsert && auto_insert) ||
                            (b.id == Control::LockLayout && layout_locked);
            const Color fill = !on ? mix(background, card, .40f) :
                               selected ? mix(card, cyan, .14f) : card;
            const Color accent = b.id == Control::Record && panel.recording ? pink : cyan;
            const bool highlighted = on && (selected || b.id == Control::Record ||
                                             (b.id == Control::Insert && panel.transcript.size()));
            rounded(b.r, std::min(16, b.r.h / 3), fill,
                    !on ? mix(card, muted, .13f) : highlighted ? accent : mix(card, muted, .38f),
                    highlighted ? .23f : 0.f, highlighted ? 2 : 1);
            if (b.id == Control::Quit && (pressed[0] == int(i) || pressed[1] == int(i))) {
                const int cursor = pressed[0] == int(i) ? 0 : 1;
                const float fraction = std::clamp(float(std::chrono::duration_cast<std::chrono::milliseconds>(
                    PanelSurface::Clock::now() - press_time[cursor]).count()) / PanelSurface::quit_hold.count(), 0.f, 1.f);
                rect({b.r.x + 7, b.r.y + b.r.h - 9, int((b.r.w - 14) * fraction), 3}, pink);
            }
            const std::string label = b.id == Control::Record && panel.recording ? "Stop" :
                b.id == Control::Clock24h ? (clock_24h ? "Clock: 24 hour" : "Clock: 12 hour") :
                b.id == Control::Date ? (date_format == DateFormat::Off ? "Date: Off" :
                    date_format == DateFormat::MonthDayYear ? "Date: MM/DD/YYYY" :
                    date_format == DateFormat::DayMonthYear ? "Date: DD/MM/YYYY" : "Date: YYYY-MM-DD") : b.label;
            text(label, b.r.x + 16, b.r.y + b.r.h / 2 + 9, b.id == Control::Date ? 23 : 27,
                 on ? ink : mix(background, muted, .48f), b.r.x + b.r.w - 8);
            if (mounting(b.id) && selected) text("ON", b.r.x + b.r.w - 56, b.r.y + 38, 23, cyan, b.r.x + b.r.w - 12);
            if (b.id == Control::LasersAnytime || b.id == Control::AdvancedDebug || b.id == Control::AutoInsert ||
                b.id == Control::LockLayout) {
                bool active = b.id == Control::LasersAnytime ? lasers_anytime :
                              b.id == Control::AutoInsert ? auto_insert :
                              b.id == Control::LockLayout ? layout_locked : advanced_debug;
                text(active ? "ON" : "OFF", b.r.x + b.r.w - 66, b.r.y + b.r.h / 2 + 9, 22,
                     active ? cyan : muted, b.r.x + b.r.w - 12);
            }
        }
        // Screenshot-inspired grab underline and outside corner bracket. No
        // opaque toolbar backing; broad hit targets surround the slender strokes.
        if (!layout_locked) {
            const Color handle = theme.frame_end;
            rounded({400, 721, 200, 6}, 3, handle, handle);
            rounded({1008, 721, 36, 6}, 3, handle, handle);
            rounded({1038, 691, 6, 36}, 3, handle, handle);
            // The frame gradient, without changing antialiased alpha coverage.
            for (Rect bounds : {Rect{400, 721, 200, 6}, Rect{1008, 691, 36, 36}})
                for (int y = bounds.y; y < bounds.y + bounds.h; ++y)
                    for (int x = bounds.x; x < bounds.x + bounds.w; ++x) {
                        auto* dst = pixels.data() + (size_t(y) * W + x) * 4;
                        if (!dst[3]) continue;
                        const auto color = mix(theme.frame_start, theme.frame_end,
                                               float(x - bounds.x) / (bounds.w - 1));
                        std::copy_n(color.begin(), 3, dst);
                    }
        }
        dirty = false;
        return true;
    }
};
PanelSurface::PanelSurface(const std::string& font, Mount mount, Theme theme)
    : impl_(std::make_unique<Impl>(font, mount, theme)) {}
PanelSurface::~PanelSurface() = default;
bool PanelSurface::render(const Panel& p) { return impl_->render(p); }
const std::vector<unsigned char>& PanelSurface::pixels() const { return impl_->pixels; }
bool PanelSurface::available(UiAction a) const { return impl_->available(a); }
bool PanelSurface::dragging(unsigned cursor) const {
    return cursor < impl_->pressed.size() && int(cursor) == impl_->drag_cursor;
}
std::optional<PanelDragKind> PanelSurface::pointer_down(unsigned cursor, float x, float y, Clock::time_point now) {
    if (cursor >= impl_->pressed.size() || impl_->drag_cursor >= 0) return {};
    const auto contains = [&](Bounds b) { return Rect{b.x, b.y, b.w, b.h}.contains(x, y); };
    if (!impl_->layout_locked && (contains(grab) || contains(scale))) {
        impl_->reset(); // other cursor's prior approval cannot survive relocation
        impl_->drag_cursor = int(cursor);
        return contains(grab) ? PanelDragKind::Grab : PanelDragKind::Scale;
    }
    impl_->pressed[cursor] = impl_->hit(x, y);
    impl_->press_time[cursor] = now;
    if (impl_->pressed[cursor] == 10) impl_->dirty = true;
    return {};
}
SurfaceEvent PanelSurface::pointer_up(unsigned cursor, float x, float y, Clock::time_point now) {
    SurfaceEvent result;
    if (cursor >= impl_->pressed.size()) return result;
    if (impl_->drag_cursor >= 0) {
        if (impl_->drag_cursor == int(cursor)) impl_->reset();
        return result;
    }
    int index = std::exchange(impl_->pressed[cursor], -1);
    if (index == 10) impl_->dirty = true;
    if (index < 0 || impl_->hit(x, y) != index) return result;
    auto c = buttons[index].id;
    if (c == Control::Quit && now - impl_->press_time[cursor] < quit_hold) return result;
    if (c == Control::Record) result.action = impl_->panel.recording ? UiAction::EndRecord : UiAction::BeginRecord;
    else if (auto a = action(c)) result.action = a;
    else if (auto m = mounting(c)) { impl_->mount = *m; result.mount = *m; impl_->reset(); impl_->dirty = true; }
    else if (c == Control::Recenter) { result.recenter = true; impl_->reset(); }
    else if (c == Control::LasersAnytime) result.lasers_anytime = !impl_->lasers_anytime;
    else if (c == Control::AdvancedDebug) result.advanced_debug = !impl_->advanced_debug;
    else if (c == Control::AutoInsert) result.auto_insert = !impl_->auto_insert;
    else if (c == Control::LockLayout) result.lock_layout = !impl_->layout_locked;
    else if (c == Control::Clock24h) result.clock_24h = !impl_->clock_24h;
    else if (c == Control::Date) result.date_format = static_cast<DateFormat>((static_cast<int>(impl_->date_format) + 1) % 4);
    else if (c == Control::Bindings) { result.open_bindings = true; impl_->reset(); }
    else if (c == Control::Review || c == Control::Settings) {
        impl_->tab = c == Control::Review ? Tab::Review : Tab::Settings;
        impl_->reset(); impl_->dirty = true;
    }
    else if (c == Control::Prev) { --impl_->page; impl_->dirty = true; }
    else if (c == Control::Next) { ++impl_->page; impl_->dirty = true; }
    return result;
}
void PanelSurface::set_binding_note(std::string note) {
    if (impl_->binding_note != note) { impl_->binding_note = std::move(note); impl_->dirty = true; }
}
void PanelSurface::reset_pointers() { impl_->reset(); }
std::vector<PanelSurface::Bounds> PanelSurface::input_regions() const {
    std::vector<Bounds> result{{10, 10, CW - 20, CH - 20}};
    if (!impl_->layout_locked) { result.push_back(grab); result.push_back(scale); }
    return result;
}
void PanelSurface::set_layout_locked(bool locked) {
    if (impl_->layout_locked != locked) {
        impl_->layout_locked = locked; impl_->reset(); impl_->dirty = true;
    }
}
void PanelSurface::set_placement_note(std::string note) {
    if (impl_->placement_note != note) { impl_->placement_note = std::move(note); impl_->dirty = true; }
}
void PanelSurface::set_lasers_anytime(bool enabled) {
    if (impl_->lasers_anytime != enabled) {
        impl_->lasers_anytime = enabled;
        impl_->reset();
        impl_->dirty = true;
    }
}
void PanelSurface::set_advanced_debug(bool enabled) {
    if (impl_->advanced_debug != enabled) {
        impl_->advanced_debug = enabled;
        impl_->reset();
        impl_->dirty = true;
    }
}
void PanelSurface::set_auto_insert(bool enabled) {
    if (impl_->auto_insert != enabled) {
        impl_->auto_insert = enabled;
        impl_->reset();
        impl_->dirty = true;
    }
}
void PanelSurface::set_clock_24h(bool enabled) {
    if (impl_->clock_24h != enabled) {
        impl_->clock_24h = enabled; impl_->reset(); impl_->dirty = true;
    }
}
void PanelSurface::set_date_format(DateFormat format) {
    if (impl_->date_format != format) {
        impl_->date_format = format; impl_->reset(); impl_->dirty = true;
    }
}
void PanelSurface::set_clock_time(std::time_t now) {
    impl_->clock_time = now;
}
} // namespace frameyap
