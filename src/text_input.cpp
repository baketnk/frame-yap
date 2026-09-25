#include "text_input.hpp"
#include "core.hpp"
#include "paced_delivery.hpp"
#include "gamescope-input-method-client.h"
#include <wayland-client.h>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <poll.h>
#include <stdexcept>
#include <thread>

namespace frameyap {
struct TextInput::Impl {
    std::string socket;
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    gamescope_input_method_manager* manager = nullptr;
    gamescope_input_method* ime = nullptr;
    uint32_t serial = 0;
    uint32_t seat_name = 0, manager_name = 0;
    bool done = false, unavailable = false, ambiguous = false;
    std::chrono::steady_clock::time_point retire_after{};
    // The connection remains owned by retained() after an authorized send, not
    // by a temporary delivery lease. Do not evict it on a compositor sync:
    // Gamescope may still have keystrokes/keymap work queued for the target.
    static std::shared_ptr<Impl>& retained() {
        static std::shared_ptr<Impl> connection;
        return connection;
    }
    static std::weak_ptr<Impl>& active() {
        static std::weak_ptr<Impl> lease;
        return lease;
    }
    ~Impl() {
        // Local proxy destruction then socket close avoids a blocking flush on teardown.
        if (ime) wl_proxy_destroy(reinterpret_cast<wl_proxy*>(ime));
        if (manager) wl_proxy_destroy(reinterpret_cast<wl_proxy*>(manager));
        if (seat) wl_seat_destroy(seat);
        if (registry) wl_registry_destroy(registry);
        if (display) wl_display_disconnect(display);
    }
    static void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        auto& self = *static_cast<Impl*>(data);
        if (std::strcmp(interface, "wl_seat") == 0) {
            if (self.seat) { self.ambiguous = true; return; }
            self.seat_name = name;
            self.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        }
        if (std::strcmp(interface, "gamescope_input_method_manager") == 0 && version >= 2) {
            if (self.manager) { self.ambiguous = true; return; }
            self.manager_name = name;
            self.manager = static_cast<gamescope_input_method_manager*>(wl_registry_bind(registry, name, &gamescope_input_method_manager_interface, 2));
        }
    }
    static void removed(void* data, wl_registry*, uint32_t name) {
        auto& self = *static_cast<Impl*>(data);
        if (name == self.seat_name || name == self.manager_name) self.unavailable = true;
    }
    static void unavailable_event(void* data, gamescope_input_method*) { static_cast<Impl*>(data)->unavailable = true; }
    static void done_event(void* data, gamescope_input_method*, uint32_t serial) {
        auto& self = *static_cast<Impl*>(data); self.serial = serial; self.done = true;
    }
    static void synced(void* data, wl_callback*, uint32_t) { *static_cast<bool*>(data) = true; }
    void roundtrip() {
        bool complete = false;
        wl_callback* callback = wl_display_sync(display);
        if (!callback) throw std::runtime_error("Gamescope sync allocation failed");
        const wl_callback_listener listener{synced};
        wl_callback_add_listener(callback, &listener, &complete);
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        try {
            while (!complete) {
                if (wl_display_dispatch_pending(display) < 0) throw std::runtime_error("Gamescope disconnected");
                if (complete) break;
                int flushed = wl_display_flush(display);
                if (flushed < 0 && errno != EAGAIN) throw std::runtime_error("Gamescope flush failed");
                while (wl_display_prepare_read(display) != 0) {
                    if (wl_display_dispatch_pending(display) < 0) throw std::runtime_error("Gamescope disconnected");
                }
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
                pollfd fd{wl_display_get_fd(display), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
                int result = ms > 0 ? ::poll(&fd, 1, static_cast<int>(ms)) : 0;
                if (result <= 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    wl_display_cancel_read(display);
                    if (result < 0 && errno == EINTR) continue;
                    throw std::runtime_error("Gamescope unavailable or timed out");
                }
                if (fd.revents & POLLIN) {
                    if (wl_display_read_events(display) < 0) throw std::runtime_error("Gamescope read failed");
                } else wl_display_cancel_read(display);
            }
        } catch (...) { wl_callback_destroy(callback); throw; }
        wl_callback_destroy(callback);
        if (unavailable) throw std::runtime_error("Gamescope IME unavailable (Steam keyboard may own it)");
    }
    void check_authorization(bool used) const {
        if (used || unavailable || !done) throw std::runtime_error("Input authorization is no longer valid");
    }
    void commit() {
        // Also cover uncertain sends. Normal idle ticks already wait this long;
        // shutdown needs the same bounded grace without sending anything else.
        struct Cooldown {
            Impl& self;
            ~Cooldown() { self.retire_after = std::chrono::steady_clock::now() + PacedDelivery::gap; }
        } cooldown{*this};
        gamescope_input_method_commit(ime, serial);
        // A roundtrip only acknowledges compositor processing, not target consumption.
        roundtrip();
    }
};
TextInput::TextInput(const std::string& socket) {
    if (socket.empty()) throw std::runtime_error("Specify the Gamescope Wayland socket with --socket");
    if (!Impl::active().expired()) throw std::runtime_error("Gamescope IME lease already active");
    if (Impl::retained()) {
        if (Impl::retained()->socket != socket)
            throw std::runtime_error("Gamescope socket changed; release idle IME before retargeting");
        impl_ = Impl::retained();
        // A fresh compositor roundtrip observes unavailable/seat removal, not
        // delivery to the destination application. Failed acquisition must not
        // consume a review, nor automatically replay earlier uncertain input.
        try { impl_->roundtrip(); }
        catch (...) { Impl::retained().reset(); throw; }
        if (!impl_->done || impl_->unavailable) {
            Impl::retained().reset();
            throw std::runtime_error("Gamescope IME no longer ready");
        }
        Impl::active() = impl_;
        return;
    }
    impl_ = std::make_shared<Impl>();
    auto& p = *impl_;
    p.socket = socket;
    p.display = wl_display_connect(socket.c_str());
    if (!p.display) throw std::runtime_error("Cannot connect to Gamescope socket; check --socket");
    p.registry = wl_display_get_registry(p.display);
    if (!p.registry) throw std::runtime_error("Cannot get Gamescope registry");
    static const wl_registry_listener registry_listener{Impl::global, Impl::removed};
    wl_registry_add_listener(p.registry, &registry_listener, &p);
    p.roundtrip();
    if (!p.seat || !p.manager || p.ambiguous)
        throw std::runtime_error("Gamescope IME v2/unique seat unavailable on this socket");
    p.ime = gamescope_input_method_manager_create_input_method(p.manager, p.seat);
    if (!p.ime) throw std::runtime_error("Cannot create Gamescope IME");
    static const gamescope_input_method_listener ime_listener{Impl::unavailable_event, Impl::done_event};
    gamescope_input_method_add_listener(p.ime, &ime_listener, &p);
    p.roundtrip();
    if (!p.done) throw std::runtime_error("Gamescope IME not ready");
    Impl::active() = impl_;
}
TextInput::~TextInput() {
    if (Impl::active().lock() == impl_) Impl::active().reset();
}
void TextInput::release_idle() {
    if (!Impl::active().expired()) throw std::runtime_error("Cannot release an active Gamescope IME lease");
    if (Impl::retained()) std::this_thread::sleep_until(Impl::retained()->retire_after);
    Impl::retained().reset();
}
void TextInput::text(const std::string& literal) {
    auto validated = literal_text(literal);
    // A valid whole transcript can yield a batch containing only spaces. Keep
    // those bytes; only whole ASR replies use the all-whitespace => empty policy.
    if (validated.empty() && !literal.empty() && literal.find_first_not_of(' ') == std::string::npos)
        validated = literal;
    if (validated.empty()) throw std::runtime_error("No text to insert");
    // Gamescope's temporary keymap has a finite rotating keycode table. A
    // 4080-byte Wayland-valid payload can still corrupt its *front* before
    // commit. The UI scheduler sends at most 24 Unicode codepoints per commit;
    // reject accidental bypasses here, including the old synchronous path.
    std::size_t codepoints = 0;
    for (unsigned char byte : validated)
        if ((byte & 0xc0) != 0x80) ++codepoints;
    if (codepoints > 24) throw std::runtime_error("Input batch exceeds 24 codepoints");
    impl_->check_authorization(used_);
    used_ = true;
    Impl::retained() = impl_; // Retain even if the roundtrip fails ambiguously.
    gamescope_input_method_set_string(impl_->ime, validated.c_str());
    impl_->commit();
}
void TextInput::enter() {
    impl_->check_authorization(used_);
    used_ = true;
    Impl::retained() = impl_;
    gamescope_input_method_set_action(impl_->ime, GAMESCOPE_INPUT_METHOD_ACTION_SUBMIT);
    impl_->commit();
}
}
