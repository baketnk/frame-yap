#include "focus_guard.hpp"
#include <xcb/xcb.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

using namespace frameyap;
namespace {
struct Free { void operator()(void* p) const { std::free(p); } };
template<class T> using Reply = std::unique_ptr<T, Free>;
xcb_atom_t atom(xcb_connection_t* c, const char* name) {
    auto cookie = xcb_intern_atom(c, 0, std::strlen(name), name);
    Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(c, cookie, nullptr));
    assert(reply && reply->atom);
    return reply->atom;
}
void focus(xcb_connection_t* c, xcb_window_t root, xcb_window_t window,
           xcb_atom_t active, xcb_atom_t gamescope) {
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, active, XCB_ATOM_WINDOW, 32, 1, &window);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, gamescope, XCB_ATOM_CARDINAL, 32, 1, &window);
    xcb_set_input_focus(c, XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME);
    auto cookie = xcb_get_input_focus(c);
    Reply<xcb_get_input_focus_reply_t> reply(xcb_get_input_focus_reply(c, cookie, nullptr));
    assert(reply && reply->focus == window);
}
}
int main() {
    int index = 0;
    xcb_connection_t* c = xcb_connect(nullptr, &index);
    assert(c && !xcb_connection_has_error(c));
    auto screens = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < index; ++i) xcb_screen_next(&screens);
    assert(screens.rem);
    const auto root = screens.data->root;
    const auto active = atom(c, "_NET_ACTIVE_WINDOW"), gamescope = atom(c, "GAMESCOPE_FOCUSED_WINDOW");
    const auto a = xcb_generate_id(c), b = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, a, root, 0, 0, 160, 100, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screens.data->root_visual, 0, nullptr);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, b, root, 180, 0, 160, 100, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screens.data->root_visual, 0, nullptr);
    xcb_map_window(c, a); xcb_map_window(c, b);
    focus(c, root, a, active, gamescope);
    FocusGuard stable;
    assert(stable.arm() && stable.valid());
    focus(c, root, b, active, gamescope);
    focus(c, root, a, active, gamescope);
    assert(!stable.valid()); // loss/regain of the same ID cannot authorize delivery
    assert(!stable.arm());
    FocusGuard new_capture;
    assert(new_capture.arm() && new_capture.valid());
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, active, XCB_ATOM_WINDOW, 32, 1, &b);
    auto cookie = xcb_get_input_focus(c);
    Reply<xcb_get_input_focus_reply_t> reply(xcb_get_input_focus_reply(c, cookie, nullptr));
    assert(reply);
    assert(!new_capture.valid()); // compositor/X disagreement fails closed
    focus(c, root, a, active, gamescope);
    FocusGuard destroyed;
    assert(destroyed.arm());
    xcb_destroy_window(c, a);
    cookie = xcb_get_input_focus(c);
    reply.reset(xcb_get_input_focus_reply(c, cookie, nullptr));
    assert(reply);
    assert(!destroyed.valid());
    xcb_disconnect(c);
    std::cout << "focus guard XCB transitions passed\n";
}
