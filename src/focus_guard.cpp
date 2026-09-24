#include "focus_guard.hpp"

#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>
#include <memory>

namespace frameyap {
namespace {
struct ReplyDeleter { void operator()(void* p) const { std::free(p); } };
template<class T> using Reply = std::unique_ptr<T, ReplyDeleter>;
}

struct FocusGuard::Impl {
    xcb_connection_t* connection = nullptr;
    xcb_window_t root = XCB_WINDOW_NONE;
    xcb_window_t target = XCB_WINDOW_NONE;
    xcb_atom_t active_atom = XCB_ATOM_NONE;
    xcb_atom_t gamescope_atom = XCB_ATOM_NONE;
    bool attempted = false;
    bool armed = false;
    bool dead = false;

    ~Impl() { if (connection) xcb_disconnect(connection); }

    bool failed() const { return dead || !connection || xcb_connection_has_error(connection); }

    bool atom(const char* name, xcb_atom_t& value) {
        auto cookie = xcb_intern_atom(connection, 0,
            static_cast<uint16_t>(std::strlen(name)), name);
        xcb_generic_error_t* error = nullptr;
        Reply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(connection, cookie, &error));
        Reply<xcb_generic_error_t> error_owner(error);
        if (!reply || error || failed()) return false;
        value = reply->atom;
        return value != XCB_ATOM_NONE;
    }

    bool select(xcb_window_t window, uint32_t mask) {
        auto cookie = xcb_change_window_attributes_checked(connection, window,
            XCB_CW_EVENT_MASK, &mask);
        Reply<xcb_generic_error_t> error(xcb_request_check(connection, cookie));
        return !error && !failed();
    }

    bool property_window(xcb_atom_t property, xcb_atom_t required_type,
                         xcb_window_t& result) {
        auto cookie = xcb_get_property(connection, 0, root, property,
            required_type, 0, 1);
        xcb_generic_error_t* error = nullptr;
        Reply<xcb_get_property_reply_t> reply(xcb_get_property_reply(connection, cookie, &error));
        Reply<xcb_generic_error_t> error_owner(error);
        if (!reply || error || failed() || reply->type != required_type ||
            reply->format != 32 || reply->value_len != 1 || reply->bytes_after != 0)
            return false;
        result = *static_cast<const uint32_t*>(xcb_get_property_value(reply.get()));
        return result != XCB_WINDOW_NONE;
    }

    bool focus(xcb_window_t& result) {
        auto cookie = xcb_get_input_focus(connection);
        xcb_generic_error_t* error = nullptr;
        Reply<xcb_get_input_focus_reply_t> reply(xcb_get_input_focus_reply(connection, cookie, &error));
        Reply<xcb_generic_error_t> error_owner(error);
        if (!reply || error || failed()) return false;
        result = reply->focus;
        return result != XCB_WINDOW_NONE;
    }

    bool keys_up() {
        auto cookie = xcb_query_keymap(connection);
        xcb_generic_error_t* error = nullptr;
        Reply<xcb_query_keymap_reply_t> reply(xcb_query_keymap_reply(connection, cookie, &error));
        Reply<xcb_generic_error_t> error_owner(error);
        if (!reply || error || failed()) return false;
        for (unsigned char byte : reply->keys) if (byte != 0) return false;
        return true;
    }

    bool snapshot(xcb_window_t& current) {
        xcb_window_t active = XCB_WINDOW_NONE, gamescope = XCB_WINDOW_NONE, actual = XCB_WINDOW_NONE;
        if (!property_window(active_atom, XCB_ATOM_WINDOW, active) ||
            !property_window(gamescope_atom, XCB_ATOM_CARDINAL, gamescope) ||
            !focus(actual) || active != gamescope || active != actual || !keys_up())
            return false;
        current = active;
        return true;
    }

    bool drain_events() {
        while (xcb_generic_event_t* event = xcb_poll_for_event(connection)) {
            const uint8_t type = event->response_type & 0x7f;
            bool bad = type == 0; // asynchronous X error
            if (type == XCB_PROPERTY_NOTIFY) {
                const auto* e = reinterpret_cast<xcb_property_notify_event_t*>(event);
                if (e->window == root && (e->atom == active_atom || e->atom == gamescope_atom)) bad = true;
            } else if (type == XCB_FOCUS_OUT) {
                const auto* e = reinterpret_cast<xcb_focus_out_event_t*>(event);
                if (e->event == target) bad = true;
            } else if (type == XCB_DESTROY_NOTIFY) {
                const auto* e = reinterpret_cast<xcb_destroy_notify_event_t*>(event);
                if (e->window == target) bad = true;
            } else if (type == XCB_CREATE_NOTIFY || type == XCB_UNMAP_NOTIFY ||
                       type == XCB_MAP_NOTIFY || type == XCB_MAP_REQUEST ||
                       type == XCB_REPARENT_NOTIFY || type == XCB_CONFIGURE_NOTIFY ||
                       type == XCB_CONFIGURE_REQUEST || type == XCB_GRAVITY_NOTIFY ||
                       type == XCB_RESIZE_REQUEST || type == XCB_CIRCULATE_NOTIFY ||
                       type == XCB_CIRCULATE_REQUEST) {
                // Any structural change on the observed target is ambiguous.
                xcb_window_t window = XCB_WINDOW_NONE;
                switch (type) {
                case XCB_CREATE_NOTIFY: window = reinterpret_cast<xcb_create_notify_event_t*>(event)->parent; break;
                case XCB_UNMAP_NOTIFY: window = reinterpret_cast<xcb_unmap_notify_event_t*>(event)->window; break;
                case XCB_MAP_NOTIFY: window = reinterpret_cast<xcb_map_notify_event_t*>(event)->window; break;
                case XCB_MAP_REQUEST: window = reinterpret_cast<xcb_map_request_event_t*>(event)->window; break;
                case XCB_REPARENT_NOTIFY: window = reinterpret_cast<xcb_reparent_notify_event_t*>(event)->window; break;
                case XCB_CONFIGURE_NOTIFY: window = reinterpret_cast<xcb_configure_notify_event_t*>(event)->window; break;
                case XCB_CONFIGURE_REQUEST: window = reinterpret_cast<xcb_configure_request_event_t*>(event)->window; break;
                case XCB_GRAVITY_NOTIFY: window = reinterpret_cast<xcb_gravity_notify_event_t*>(event)->window; break;
                case XCB_RESIZE_REQUEST: window = reinterpret_cast<xcb_resize_request_event_t*>(event)->window; break;
                case XCB_CIRCULATE_NOTIFY: window = reinterpret_cast<xcb_circulate_notify_event_t*>(event)->window; break;
                case XCB_CIRCULATE_REQUEST: window = reinterpret_cast<xcb_circulate_request_event_t*>(event)->window; break;
                }
                if (window == target) bad = true;
            }
            std::free(event);
            if (bad) return false;
        }
        return !failed();
    }

    bool check() {
        if (!armed || failed() || !drain_events()) { dead = true; return false; }
        xcb_window_t current = XCB_WINDOW_NONE;
        if (!snapshot(current) || current != target || !drain_events()) {
            dead = true;
            return false;
        }
        return true;
    }
};

FocusGuard::FocusGuard() : impl_(std::make_unique<Impl>()) {}
FocusGuard::~FocusGuard() = default;

bool FocusGuard::arm() {
    auto& p = *impl_;
    if (p.attempted || p.dead) return false;
    p.attempted = true;
    int screen_number = 0;
    p.connection = xcb_connect(nullptr, &screen_number);
    if (!p.connection || xcb_connection_has_error(p.connection)) { p.dead = true; return false; }
    const xcb_setup_t* setup = xcb_get_setup(p.connection);
    auto iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_number && iter.rem; ++i) xcb_screen_next(&iter);
    if (!iter.rem) { p.dead = true; return false; }
    p.root = iter.data->root;
    if (!p.atom("_NET_ACTIVE_WINDOW", p.active_atom) ||
        !p.atom("GAMESCOPE_FOCUSED_WINDOW", p.gamescope_atom)) { p.dead = true; return false; }
    xcb_window_t before = XCB_WINDOW_NONE;
    if (!p.snapshot(before)) { p.dead = true; return false; }
    p.target = before;
    if (!p.select(p.root, XCB_EVENT_MASK_PROPERTY_CHANGE) ||
        !p.select(p.target, XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY)) {
        p.dead = true;
        return false;
    }
    // Validate after subscriptions so a target/property transition during setup
    // cannot silently authorize the capture.
    xcb_window_t after = XCB_WINDOW_NONE;
    if (!p.snapshot(after) || after != p.target || !p.drain_events()) {
        p.dead = true;
        return false;
    }
    p.armed = true;
    return true;
}

bool FocusGuard::valid() { return impl_->check(); }
void FocusGuard::invalidate() { impl_->dead = true; impl_->armed = false; }

} // namespace frameyap
