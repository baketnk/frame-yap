#include "config.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace frameyap {
namespace {
struct Json {
    std::string value;
    std::map<std::string, Json> object;
    bool is_string = false, is_object = false, is_number = false;
};
struct Parser {
    std::string_view s;
    size_t pos = 0;
    void ws() { while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\r' || s[pos] == '\t')) ++pos; }
    bool eat(char c) { ws(); if (pos < s.size() && s[pos] == c) { ++pos; return true; } return false; }
    [[noreturn]] void fail() const { throw std::runtime_error("Invalid FrameYap JSON config at byte " + std::to_string(pos)); }
    std::string str() {
        if (!eat('"')) fail();
        std::string out;
        while (pos < s.size()) {
            unsigned char c = s[pos++];
            if (c == '"') return out;
            if (c < 0x20) fail();
            if (c != '\\') { out += char(c); continue; }
            if (pos == s.size()) fail();
            c = s[pos++];
            switch (c) {
            case '"': case '\\': case '/': out += char(c); break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break;
            case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                auto hex = [&]() {
                    unsigned n = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (pos == s.size()) fail();
                        char h = s[pos++];
                        if (!std::isxdigit(static_cast<unsigned char>(h))) fail();
                        n = n * 16 + (h <= '9' ? h - '0' : (h <= 'F' ? h - 'A' + 10 : h - 'a' + 10));
                    }
                    return n;
                };
                unsigned cp = hex();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (pos + 2 > s.size() || s.substr(pos, 2) != "\\u") fail();
                    pos += 2;
                    unsigned low = hex();
                    if (low < 0xdc00 || low > 0xdfff) fail();
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) fail();
                if (cp < 0x80) out += char(cp);
                else if (cp < 0x800) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
                else if (cp < 0x10000) { out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                else { out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
                break;
            }
            default: fail();
            }
        }
        fail();
    }
    Json parse(unsigned depth = 0) {
        if (depth > 8) fail();
        ws();
        if (pos == s.size()) fail();
        Json result;
        if (s[pos] == '"') { result.value = str(); result.is_string = true; return result; }
        if (eat('{')) {
            result.is_object = true;
            if (eat('}')) return result;
            do {
                ws(); if (pos == s.size() || s[pos] != '"') fail();
                auto key = str();
                if (!eat(':')) fail();
                auto [it, inserted] = result.object.emplace(std::move(key), parse(depth + 1));
                if (!inserted) fail();
                if (eat('}')) return result;
            } while (eat(','));
            fail();
        }
        if (eat('[')) {
            if (eat(']')) return result;
            do { parse(depth + 1); if (eat(']')) return result; } while (eat(','));
            fail();
        }
        size_t start = pos;
        if (s.substr(pos, 4) == "true" || s.substr(pos, 4) == "null") pos += 4;
        else if (s.substr(pos, 5) == "false") pos += 5;
        else {
            if (s[pos] == '-') ++pos;
            if (pos == s.size()) fail();
            if (s[pos] == '0') ++pos;
            else { if (s[pos] < '1' || s[pos] > '9') fail(); while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos; }
            if (pos < s.size() && s[pos] == '.') { ++pos; size_t at = pos; while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos; if (at == pos) fail(); }
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                ++pos; if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) ++pos;
                size_t at = pos; while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) ++pos;
                if (at == pos) fail();
            }
        }
        if (pos == start) fail();
        if (s[start] == '-' || (s[start] >= '0' && s[start] <= '9')) {
            result.is_number = true;
            result.value = s.substr(start, pos - start);
        }
        return result;
    }
};
Rgba color(const Json& json) {
    if (!json.is_string || json.value.size() != 7 || json.value[0] != '#')
        throw std::runtime_error("Theme colors must be #RRGGBB strings");
    Rgba c{0, 0, 0, 255};
    for (int i = 0; i < 3; ++i) {
        auto digit = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        int hi = digit(json.value[1 + 2*i]), lo = digit(json.value[2 + 2*i]);
        if (hi < 0 || lo < 0) throw std::runtime_error("Theme colors must be #RRGGBB strings");
        c[i] = static_cast<unsigned char>(hi * 16 + lo);
    }
    return c;
}
float bounded_number(const Json& json, std::string_view name, float lower, float upper) {
    if (!json.is_number) throw std::runtime_error("Config wrist " + std::string(name) + " must be a number");
    const float value = std::strtof(json.value.c_str(), nullptr);
    if (!std::isfinite(value) || value < lower || value > upper)
        throw std::runtime_error("Config wrist " + std::string(name) + " out of range");
    return value;
}
std::filesystem::path xdg_root(const char* variable, const char* fallback) {
    const char* env = std::getenv(variable);
    if (env && *env && std::filesystem::path(env).is_absolute()) return env;
    const char* home = std::getenv("HOME");
    if (home && *home && std::filesystem::path(home).is_absolute()) return std::filesystem::path(home) / fallback;
    return {};
}
std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + p.string());
    std::string data;
    char buf[4097];
    in.read(buf, sizeof(buf));
    if (in.gcount() == sizeof(buf)) throw std::runtime_error("FrameYap config/input file too large: " + p.string());
    data.assign(buf, size_t(in.gcount()));
    return data;
}
void write_file(const std::filesystem::path& path, const std::string& content) {
    auto temporary = path.string() + ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    try {
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << content;
            if (!out) throw std::runtime_error("Could not write generated OpenVR binding: " + path.string());
        }
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
} // namespace

std::filesystem::path default_config_path() {
    auto root = xdg_root("XDG_CONFIG_HOME", ".config");
    return root.empty() ? root : root / "frameyap/config.json";
}
Config load_config(const std::filesystem::path& path) {
    Config config;
    if (path.empty() || !std::filesystem::exists(path)) return config;
    auto bytes = read_file(path);
    Parser parser{bytes};
    auto root = parser.parse(); parser.ws();
    if (parser.pos != bytes.size() || !root.is_object) throw std::runtime_error("Invalid FrameYap config object: " + path.string());
    for (const auto& [key, value] : root.object) {
        if (key == "font") {
            if (!value.is_string) throw std::runtime_error("Config font must be a path string");
            config.font = value.value;
        } else if (key == "input_priority") {
            if (!value.is_string || (value.value != "normal" && value.value != "experimental"))
                throw std::runtime_error("Config input_priority must be normal or experimental");
            config.experimental_input_priority = value.value == "experimental";
        } else if (key == "wrist") {
            if (!value.is_object) throw std::runtime_error("Config wrist must be an object");
            for (const auto& [name, v] : value.object) {
                if (name == "x") config.wrist.x = bounded_number(v, name, -.3f, .3f);
                else if (name == "y") config.wrist.y = bounded_number(v, name, -.3f, .3f);
                else if (name == "z") config.wrist.z = bounded_number(v, name, -.3f, .3f);
                else if (name == "width") config.wrist.width = bounded_number(v, name, .15f, .6f);
                else if (name == "roll_degrees") config.wrist.roll_degrees = bounded_number(v, name, -180.f, 180.f);
                else throw std::runtime_error("Unknown wrist placement key: " + name);
            }
        } else if (key == "theme") {
            if (!value.is_object) throw std::runtime_error("Config theme must be an object");
            for (const auto& [name, v] : value.object) {
                Rgba* target = nullptr;
                if (name == "background") target = &config.theme.background;
                else if (name == "card") target = &config.theme.card;
                else if (name == "ink") target = &config.theme.ink;
                else if (name == "muted") target = &config.theme.muted;
                else if (name == "accent") target = &config.theme.accent;
                else if (name == "warning") target = &config.theme.warning;
                else if (name == "frame_start") target = &config.theme.frame_start;
                else if (name == "frame_end") target = &config.theme.frame_end;
                else throw std::runtime_error("Unknown theme color: " + name);
                *target = color(v);
            }
        } else if (key == "buttons") {
            if (!value.is_object) throw std::runtime_error("Config buttons must be an object");
            for (const auto& [name, v] : value.object) {
                if (name != "left_grip" && name != "right_grip" && name != "ptt" && name != "cancel" && name != "insert" && name != "enter")
                    throw std::runtime_error("Unknown OpenVR button action: " + name);
                if (!v.is_string) throw std::runtime_error("Button path must be a string");
                auto path = v.value;
                if (!path.empty()) {
                    auto valid = [](std::string_view prefix, const std::string& path) {
                        return path.starts_with(prefix) && path.size() > prefix.size() &&
                            std::all_of(path.begin() + prefix.size(), path.end(), [](unsigned char c) {
                                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                       (c >= '0' && c <= '9') || c == '_';
                            });
                    };
                    if (!valid("/user/hand/left/input/", path) && !valid("/user/hand/right/input/", path))
                        throw std::runtime_error("Invalid Frame controller button path: " + path);
                }
                config.buttons[name] = path;
            }
        } else throw std::runtime_error("Unknown FrameYap config key: " + key);
    }
    return config;
}
std::string resolve_font(const std::string& assets, const std::string& requested) {
    const auto bundled = std::filesystem::path(assets) / "fonts/Inconsolata-Regular.ttf";
    const std::array<std::filesystem::path, 4> candidates{requested, bundled,
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"};
    for (const auto& candidate : candidates)
        if (!candidate.empty() && std::filesystem::is_regular_file(candidate)) return candidate.string();
    throw std::runtime_error("No panel font found (set font in config.json or install bundled font)");
}
std::filesystem::path action_manifest(const std::string& assets, const Config& config) {
    auto source = std::filesystem::absolute(std::filesystem::path(assets) / "actions.json");
    if (config.buttons.empty()) return source;
    auto root = xdg_root("XDG_CACHE_HOME", ".cache");
    if (root.empty()) throw std::runtime_error("No absolute XDG_CACHE_HOME or HOME for generated bindings");
    auto dir = root / "frameyap/bindings";
    std::filesystem::create_directories(dir);
    std::map<std::string, std::string> buttons{{"left_grip", "/user/hand/left/input/grip"},
        {"right_grip", "/user/hand/right/input/grip"}, {"ptt", "/user/hand/right/input/x"},
        {"cancel", "/user/hand/right/input/b"}, {"insert", "/user/hand/right/input/a"},
        {"enter", "/user/hand/right/input/y"}};
    for (const auto& [key, path] : config.buttons) buttons[key] = path;
    std::string binding = "{\"controller_type\":\"frame_controller\",\"name\":\"FrameYap configured controls\",\"bindings\":{\"/actions/frameyap\":{\"sources\":[";
    std::map<std::string, std::string> used;
    for (const auto& [action, path] : buttons) {
        if (path.empty()) continue;
        if (!used.emplace(path, action).second) throw std::runtime_error("Two actions share Frame button: " + path);
        if (binding.back() != '[') binding += ',';
        binding += "{\"path\":\"" + path + "\",\"mode\":\"button\",\"inputs\":{\"click\":{\"output\":\"/actions/frameyap/in/" + action + "\"}}}";
    }
    binding += "]}}}";
    auto manifest = read_file(source);
    // The OpenVR SDK resolves relative binding URLs beside its manifest.
    write_file(dir / "bindings_frame_controller.json", binding);
    write_file(dir / "bindings_knuckles.json", read_file(std::filesystem::path(assets) / "bindings_knuckles.json"));
    write_file(dir / "actions.json", manifest);
    return dir / "actions.json";
}
} // namespace frameyap
