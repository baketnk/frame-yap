#include "config.hpp"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
using namespace frameyap;
namespace {
void put(const std::filesystem::path& p, const std::string& s) { std::ofstream out(p); out << s; assert(out); }
std::string get(const std::filesystem::path& p) { std::ifstream in(p); return {std::istreambuf_iterator<char>(in), {}}; }
template<class Fn> void fails(Fn fn) { bool raised = false; try { fn(); } catch (const std::exception&) { raised = true; } assert(raised); }
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const auto dir = std::filesystem::temp_directory_path() / ("frameyap-config-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    ::setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    auto path = default_config_path();
    assert(path == dir / "frameyap/config.json");
    ::setenv("XDG_CONFIG_HOME", "relative", 1);
    ::setenv("HOME", dir.c_str(), 1);
    assert(default_config_path() == dir / ".config/frameyap/config.json");
    ::unsetenv("HOME");
    assert(default_config_path().empty());
    ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    assert(load_config(path).buttons.empty());
    assert(!load_config(path).experimental_input_priority);
    assert(!load_config(path).advanced_debug);
    assert(load_config(path).wrist.width == .30f);
    auto example = load_config(std::filesystem::path(argv[1]) / "config.example.json");
    assert(example.buttons.at("ptt") == "/user/hand/right/input/x");
    assert(example.font.empty());
    assert(!example.experimental_input_priority);
    assert(!example.advanced_debug);
    assert(example.wrist.y == .18f && example.wrist.z == .089f);
    std::filesystem::create_directories(path.parent_path());
    assert(save_advanced_debug(path, true));
    assert(load_config(path).advanced_debug);
    assert(get(path).find("\"advanced_debug\":true") != std::string::npos);
    assert(save_advanced_debug(path, false));
    assert(!load_config(path).advanced_debug);
    for (const auto* invalid : {R"({"advanced_debug":"true"})", R"({"advanced_debug":0})",
                               R"({"advanced_debug":null})", R"({"advanced_debug":[]})"}) {
        put(path, invalid);
        fails([&] { load_config(path); });
        assert(!save_advanced_debug(path, true));
        assert(get(path) == invalid);
    }
    put(path, R"({"font":"escaped \u0061 and \"quotes\"","input_priority":"experimental","wrist":{"y":0.21},"theme":{"ink":"#123ABC"},"buttons":{"enter":""}})");
    const auto customized = get(path);
    assert(save_advanced_debug(path, true));
    assert(load_config(path).advanced_debug);
    assert(get(path).find(customized.substr(1, customized.size() - 2)) != std::string::npos);
    assert(save_advanced_debug(path, false));
    assert(!load_config(path).advanced_debug);
    auto before = get(path);
    assert(save_advanced_debug(path, false) && get(path) == before);
    put(path, R"({"advanced_debug":false, "font":"kept"})");
    assert(save_advanced_debug(path, true));
    assert(get(path) == R"({"advanced_debug":true, "font":"kept"})");
    before = get(path);
    put(path, R"({"font":"bad","font":"duplicate"})");
    assert(!save_advanced_debug(path, true));
    assert(get(path) == R"({"font":"bad","font":"duplicate"})");
    put(path, before);
    auto link = dir / "linked-config";
    std::filesystem::create_symlink(path, link);
    assert(!save_advanced_debug(link, false));
    assert(get(path) == before);
    std::filesystem::remove(link);
    put(path, R"({"font":")" + std::string(4090, 'x') + R"("})");
    before = get(path);
    assert(!save_advanced_debug(path, true) && get(path) == before);
    put(path, R"({"input_priority":"experimental","advanced_debug":true})");
    auto experimental = load_config(path);
    assert(experimental.experimental_input_priority && experimental.advanced_debug);
    // A priority request must preserve the user's existing action manifest/bindings.
    assert(action_manifest(argv[1], experimental) == std::filesystem::absolute(std::filesystem::path(argv[1]) / "actions.json"));
    put(path, R"({"input_priority":"normal"})");
    assert(!load_config(path).experimental_input_priority);
    for (const auto* invalid : {R"({"input_priority":true})", R"({"input_priority":16777216})",
                               R"({"input_priority":"highest"})", R"({"input_priority":null})"}) {
        put(path, invalid);
        fails([&] { load_config(path); });
    }
    put(path, R"({"wrist":{"x":-0.02,"y":0.15,"z":0.1,"width":0.35,"roll_degrees":-35}})");
    auto tuned = load_config(path).wrist;
    assert(tuned.x == -.02f && tuned.y == .15f && tuned.z == .1f);
    assert(tuned.width == .35f && tuned.roll_degrees == -35.f);
    for (const auto* invalid : {R"({"wrist":{"width":0}})", R"({"wrist":{"x":true}})",
                               R"({"wrist":{"z":"0.1"}})", R"({"wrist":{"roll_degrees":181}})",
                               R"({"wrist":{"extra":1}})", R"({"wrist":null})",
                               R"({"wrist":{"x":1e999}})"}) {
        put(path, invalid); fails([&] { load_config(path); });
    }
    put(path, R"({"theme":{"background":"#123ABC","accent":"#abcdef","frame_end":"#010203"},"font":"/nonexistent/face.ttf","buttons":{"ptt":"/user/hand/left/input/y","cancel":"/user/hand/right/input/b","right_grip":""}})");
    auto config = load_config(path);
    assert((config.theme.background == Rgba{0x12, 0x3a, 0xbc, 255}));
    assert((config.theme.accent == Rgba{0xab, 0xcd, 0xef, 255}));
    assert(config.buttons.at("right_grip").empty());
    assert(resolve_font(argv[1], config.font) == (std::filesystem::path(argv[1]) / "fonts/Inconsolata-Regular.ttf").string());
    auto manifest = action_manifest(argv[1], config);
    assert(std::filesystem::is_regular_file(manifest));
    assert(get(manifest).find("bindings_frame_controller.json") != std::string::npos);
    auto generated = get(manifest.parent_path() / "bindings_frame_controller.json");
    assert(generated.find("/user/hand/left/input/y") != std::string::npos);
    assert(generated.find("/actions/frameyap/in/cancel") != std::string::npos);
    assert(generated.find("/actions/frameyap/in/right_grip") == std::string::npos);
    assert(generated.find("/user/hand/right/input/a") != std::string::npos);
    assert(generated.find("/user/hand/right/input/y") != std::string::npos);
    config.buttons["cancel"] = ""; config.buttons["insert"] = ""; config.buttons["enter"] = "";
    auto disabled = action_manifest(argv[1], config);
    generated = get(disabled.parent_path() / "bindings_frame_controller.json");
    assert(generated.find("/actions/frameyap/in/cancel") == std::string::npos);
    assert(generated.find("/actions/frameyap/in/insert") == std::string::npos);
    assert(generated.find("/actions/frameyap/in/enter") == std::string::npos);
    assert(action_manifest(argv[1], {}) == std::filesystem::absolute(std::filesystem::path(argv[1]) / "actions.json"));
    put(path, R"({"buttons":{"ptt":"/user/hand/left/input/grip"}})");
    fails([&] { action_manifest(argv[1], load_config(path)); }); // overlapping bindings are never silently chosen
    put(path, R"({"buttons":{"ptt":"/user/hand/left/input/x\"}hack"}})");
    fails([&] { load_config(path); });
    put(path, R"({"theme":{"ink":"#FG0000"}})");
    fails([&] { load_config(path); });
    put(path, R"({"font":42})");
    fails([&] { load_config(path); });
    put(path, R"({"font":"a","font":"b"})");
    fails([&] { load_config(path); });
    put(path, "{\"font\":\"broken\"");
    fails([&] { load_config(path); });
    std::filesystem::remove_all(dir);
}
