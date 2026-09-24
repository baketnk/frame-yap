#include "panel_surface.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace frameyap {
namespace {
constexpr int W = PanelSurface::width, H = PanelSurface::height;
using Color = Rgba;
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
enum class Control { Review, Settings, Prev, Next, Record, Cancel, Insert, Enter, Quit,
                     World, Left, Right, Head, Recenter };
struct Button { Rect r; Control id; const char* label; };
constexpr std::array<Button, 14> buttons{{
    {{32, 138, 180, 46}, Control::Review, "Review"},
    {{226, 138, 180, 46}, Control::Settings, "Settings"},
    {{32, 406, 154, 44}, Control::Prev, "Previous"},
    {{838, 406, 130, 44}, Control::Next, "Next"},
    {{32, 574, 176, 68}, Control::Record, "Record"},
    {{222, 574, 176, 68}, Control::Cancel, "Cancel"},
    {{412, 574, 176, 68}, Control::Insert, "Insert"},
    {{602, 574, 176, 68}, Control::Enter, "Enter"},
    {{792, 574, 176, 68}, Control::Quit, "Quit"},
    {{32, 234, 454, 58}, Control::World, "World space"},
    {{514, 234, 454, 58}, Control::Head, "Head"},
    {{32, 308, 454, 58}, Control::Left, "Left wrist"},
    {{514, 308, 454, 58}, Control::Right, "Right wrist"},
    {{32, 394, 300, 50}, Control::Recenter, "Recenter in front"},
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
std::string_view mount_label(Mount m) {
    switch (m) {
    case Mount::World: return "World space";
    case Mount::LeftWrist: return "Left wrist";
    case Mount::RightWrist: return "Right wrist";
    case Mount::Head: return "Head";
    }
    return "World space";
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
    bool settings = false, dirty = true;
    std::string placement_note;
    std::array<int, 2> pressed{{-1, -1}};
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
        for (int k = 0; k < 3; ++k)
            dst[k] = static_cast<unsigned char>(dst[k] * (1.f - amount) + color[k] * amount);
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
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            if (x > 34 && x < W - 34 && y > 34 && y < H - 34) continue;
            const float distance = rounded_distance(x, y, 10, 10, W - 20, H - 20, 18.f);
            const float edge = std::abs(distance);
            const float strength = edge <= 1.f ? 1.f : .30f * std::max(0.f, 1.f - (edge - 1.f) / 6.f);
            const float t = float(x) / W;
            Color gradient{};
            for (int k = 0; k < 3; ++k)
                gradient[k] = static_cast<unsigned char>(theme.frame_start[k] * (1.f - t) + theme.frame_end[k] * t);
            gradient[3] = 255;
            auto* dst = pixels.data() + (size_t(y) * W + x) * 4;
            for (int k = 0; k < 3; ++k) dst[k] = static_cast<unsigned char>(background[k] + (gradient[k] - background[k]) * strength);
            dst[3] = distance <= 1.f ? 255 : static_cast<unsigned char>(255 * std::max(0.f, 1.f - (distance - 1.f) / 6.f));
        }
        for (int x = 32; x < W - 32; ++x) {
            const float t = float(x - 32) / (W - 64);
            const int y = H - 21 - int(5 * std::sin(t * 3.14159265f));
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
        if (a == UiAction::Record) return panel.recording || panel.record_available;
        if (a == UiAction::Insert) return panel.enabled && !panel.recording && !panel.transcript.empty();
        if (a == UiAction::Enter) return panel.enabled && !panel.recording;
        if (a == UiAction::Toggle) return false;
        return true;
    }
    bool visible(Control c) const {
        if (c == Control::Prev || c == Control::Next) return !settings;
        if (mounting(c) || c == Control::Recenter) return settings;
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
        if (p.status != panel.status || p.detail != panel.detail || p.enabled != panel.enabled || p.recording != panel.recording)
            dirty = true;
        panel = p;
        if (!dirty) return false;
        rect({0, 0, W, H}, background);
        frame();
        text("FrameYap", 32, 61, 40, ink, 300);
        text("ON-DEVICE / REVIEW FIRST", 280, 58, 22, muted, 720);
        text(mount_label(mount), 756, 58, 24, cyan, 968);
        rounded({32, 78, 936, 48}, 13, mix(background, card, .7f),
                panel.recording ? mix(card, pink, .36f) : mix(card, cyan, .22f),
                panel.recording ? .16f : 0.f);
        rounded({46, 95, 13, 13}, 6, panel.recording ? pink : cyan,
                panel.recording ? pink : cyan, panel.recording ? .40f : .20f);
        auto status = wrap(panel.status, 27, 790);
        text(status.front(), 70, 109, 27, ink, 844);
        if (status.size() > 1) text("[...]", 864, 109, 23, muted, 968);
        if (!settings) {
            rounded({32, 212, 936, 178}, 16, mix(card, muted, .08f), mix(card, cyan, .25f), .18f);
            for (size_t i = 0; i < lines_per_page && page * lines_per_page + i < lines.size(); ++i)
                text(lines[page * lines_per_page + i], 48, 246 + int(i) * 40, 32,
                     panel.transcript.empty() ? muted : ink, 952);
            text("PAGE " + std::to_string(page + 1) + " / " + std::to_string(page_count()), 416, 436, 23, muted, 790);
            auto detail = wrap(panel.detail, 24, 904);
            for (size_t i = 0; i < std::min(size_t(2), detail.size()); ++i)
                text(detail[i], 32, 486 + int(i) * 30, 24, muted, 936);
            if (detail.size() > 2) text("[detail truncated]", 730, 546, 22, pink, 968);
        } else {
            text("MOUNT THE MENU", 32, 224, 22, muted, 968);
            text("World stays put. Recenter places it in front of you.", 354, 426, 22, muted, 968);
            text(placement_note.empty() ? "Choose a mount. Preference is saved on this device." : placement_note,
                 32, 486, 23, muted, 968);
            text("Tracking lost? Wrist placement falls back to world space.", 32, 518, 23, muted, 968);
        }
        rect({32, 550, 936, 1}, mix(card, cyan, .17f));
        for (size_t i = 0; i < buttons.size(); ++i) {
            const auto& b = buttons[i];
            if (!visible(b.id)) continue;
            bool on = enabled(b.id);
            bool selected = (b.id == Control::Review && !settings) || (b.id == Control::Settings && settings) ||
                            (mounting(b.id) && *mounting(b.id) == mount);
            const Color fill = !on ? mix(background, card, .40f) :
                               selected ? mix(card, cyan, .14f) : card;
            const Color accent = b.id == Control::Record && panel.recording ? pink : cyan;
            const bool highlighted = on && (selected || b.id == Control::Record ||
                                             (b.id == Control::Insert && panel.transcript.size()));
            rounded(b.r, std::min(16, b.r.h / 3), fill,
                    !on ? mix(card, muted, .13f) : highlighted ? accent : mix(card, muted, .38f),
                    highlighted ? .23f : 0.f, highlighted ? 2 : 1);
            const auto label = b.id == Control::Record && panel.recording ? "Stop" : b.label;
            text(label, b.r.x + 16, b.r.y + b.r.h / 2 + 9, 27, on ? ink : mix(background, muted, .48f), b.r.x + b.r.w - 8);
            if (mounting(b.id) && selected) text("ON", b.r.x + b.r.w - 56, b.r.y + 38, 23, cyan, b.r.x + b.r.w - 12);
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
void PanelSurface::pointer_move(unsigned, float, float) {
    // Hit-test on down/up only. SetOverlayRaw can flicker in SteamVR when each
    // laser hover frame causes another full RGBA upload.
}
void PanelSurface::pointer_down(unsigned cursor, float x, float y) {
    if (cursor >= impl_->pressed.size()) return;
    impl_->pressed[cursor] = impl_->hit(x, y);
}
SurfaceEvent PanelSurface::pointer_up(unsigned cursor, float x, float y) {
    SurfaceEvent result;
    if (cursor >= impl_->pressed.size()) return result;
    int index = std::exchange(impl_->pressed[cursor], -1);
    if (index < 0 || impl_->hit(x, y) != index) return result;
    auto c = buttons[index].id;
    if (c == Control::Record) result.action = impl_->panel.recording ? UiAction::EndRecord : UiAction::BeginRecord;
    else if (auto a = action(c)) result.action = a;
    else if (auto m = mounting(c)) { impl_->mount = *m; result.mount = *m; impl_->reset(); impl_->dirty = true; }
    else if (c == Control::Recenter) { result.recenter = true; impl_->reset(); }
    else if (c == Control::Review || c == Control::Settings) { impl_->settings = c == Control::Settings; impl_->reset(); impl_->dirty = true; }
    else if (c == Control::Prev) { --impl_->page; impl_->dirty = true; }
    else if (c == Control::Next) { ++impl_->page; impl_->dirty = true; }
    return result;
}
void PanelSurface::reset_pointers() { impl_->reset(); }
void PanelSurface::set_placement_note(std::string note) {
    if (impl_->placement_note != note) { impl_->placement_note = std::move(note); impl_->dirty = true; }
}
} // namespace frameyap
