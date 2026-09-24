#include "core.hpp"
#include "text_input.hpp"
#include "gamescope-input-method-server.h"
#include <wayland-server.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace frameyap;
using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string("line ") + std::to_string(__LINE__) + ": " #condition); } while (false)

template <typename F> void throws(F&& f) {
    try { f(); } catch (const std::runtime_error&) { return; }
    CHECK(false);
}

// This fixture never connects to the ambient compositor: the client receives only
// this private socket name, scoped to a freshly-created 0700 runtime directory.
class RuntimeDir {
public:
    RuntimeDir() {
        if (const char* old = std::getenv("XDG_RUNTIME_DIR")) { prior_ = old; had_prior_ = true; }
        char pattern[] = "/tmp/frameyap-wayland-test-XXXXXX";
        char* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("mkdtemp failed");
        path_ = created;
        struct stat st{};
        if (stat(path_.c_str(), &st) != 0 || (st.st_mode & 0777) != 0700) {
            rmdir(path_.c_str());
            throw std::runtime_error("runtime directory is not private");
        }
        if (setenv("XDG_RUNTIME_DIR", path_.c_str(), 1)) {
            rmdir(path_.c_str());
            throw std::runtime_error("setenv failed");
        }
    }
    ~RuntimeDir() {
        if (had_prior_) setenv("XDG_RUNTIME_DIR", prior_.c_str(), 1);
        else unsetenv("XDG_RUNTIME_DIR");
        unlink((path_ + "/frameyap-fake").c_str());
        unlink((path_ + "/frameyap-fake.lock").c_str());
        rmdir(path_.c_str());
    }
    RuntimeDir(const RuntimeDir&) = delete;
    RuntimeDir& operator=(const RuntimeDir&) = delete;
private:
    std::string path_, prior_;
    bool had_prior_ = false;
};

struct Event {
    std::string kind, text;
    uint32_t number = 0;
};
struct Snapshot {
    int creates = 0, destroyed = 0;
    bool bad_seat = false;
    std::vector<Event> events;
};
class FakeServer {
public:
    enum class Mode { Ready, Unavailable, Stalled };
    explicit FakeServer(Mode mode = Mode::Ready) : mode_(mode) {
        display_.reset(wl_display_create());
        CHECK(display_ != nullptr);
        CHECK(wl_display_add_socket(display_.get(), "frameyap-fake") == 0);
        CHECK(wl_global_create(display_.get(), &wl_seat_interface, 1, this, bind_seat));
        CHECK(wl_global_create(display_.get(), &gamescope_input_method_manager_interface,
                               2, this, bind_manager));
        thread_ = std::thread([this] {
            if (mode_ == Mode::Stalled) {
                while (!stop_) std::this_thread::sleep_for(5ms);
                return;
            }
            while (!stop_) {
                wl_event_loop_dispatch(wl_display_get_event_loop(display_.get()), 10);
                wl_display_flush_clients(display_.get());
            }
        });
    }
    ~FakeServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        // Destroy client resources while callback state/mutex are still alive,
        // including on a test assertion failure.
        wl_display_destroy_clients(display_.get());
        display_.reset();
    }
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;
    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return state_;
    }
    bool wait_destroyed(int count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 1s, [&] { return state_.destroyed >= count; });
    }
    static constexpr const char* socket = "frameyap-fake";
private:
    struct DisplayDeleter { void operator()(wl_display* p) const { if (p) wl_display_destroy(p); } };
    RuntimeDir runtime_;
    std::unique_ptr<wl_display, DisplayDeleter> display_;
    Mode mode_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Snapshot state_;

    static FakeServer& owner(wl_resource* r) {
        return *static_cast<FakeServer*>(wl_resource_get_user_data(r));
    }
    static void bind_seat(wl_client* client, void* data, uint32_t version, uint32_t id) {
        auto* r = wl_resource_create(client, &wl_seat_interface, version, id);
        if (!r) { wl_client_post_no_memory(client); return; }
        wl_resource_set_implementation(r, nullptr, data, nullptr);
    }
    static void bind_manager(wl_client* client, void* data, uint32_t version, uint32_t id) {
        auto* r = wl_resource_create(client, &gamescope_input_method_manager_interface, version, id);
        if (!r) { wl_client_post_no_memory(client); return; }
        static const struct gamescope_input_method_manager_interface methods{destroy, create};
        wl_resource_set_implementation(r, &methods, data, nullptr);
    }
    static void destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
    static void create(wl_client* client, wl_resource* manager, wl_resource* seat, uint32_t id) {
        auto& server = owner(manager);
        auto* r = wl_resource_create(client, &gamescope_input_method_interface,
                                     wl_resource_get_version(manager), id);
        if (!r) { wl_client_post_no_memory(client); return; }
        static const struct gamescope_input_method_interface methods{
            destroy, commit, set_string, set_action, nullptr, nullptr, nullptr, nullptr};
        wl_resource_set_implementation(r, &methods, &server, ime_destroyed);
        {
            std::lock_guard lock(server.mutex_);
            ++server.state_.creates;
            if (!seat || wl_resource_get_client(seat) != client ||
                std::strcmp(wl_resource_get_class(seat), "wl_seat") != 0 ||
                wl_resource_get_version(manager) != 2) server.state_.bad_seat = true;
        }
        if (server.mode_ == Mode::Ready) gamescope_input_method_send_done(r, 42);
        else gamescope_input_method_send_unavailable(r);
    }
    static void ime_destroyed(wl_resource* r) {
        auto& server = owner(r);
        {
            std::lock_guard lock(server.mutex_);
            ++server.state_.destroyed;
        }
        server.changed_.notify_all();
    }
    static void commit(wl_client*, wl_resource* r, uint32_t serial) {
        auto& server = owner(r);
        std::lock_guard lock(server.mutex_);
        server.state_.events.push_back({"commit", "", serial});
    }
    static void set_string(wl_client*, wl_resource* r, const char* text) {
        auto& server = owner(r);
        std::lock_guard lock(server.mutex_);
        server.state_.events.push_back({"string", text, 0});
    }
    static void set_action(wl_client*, wl_resource* r, uint32_t action) {
        auto& server = owner(r);
        std::lock_guard lock(server.mutex_);
        server.state_.events.push_back({"action", "", action});
    }
};

void test_text() {
    FakeServer server;
    {
        TextInput input(server.socket);
        auto initial = server.snapshot();
        CHECK(initial.creates == 1 && !initial.bad_seat && initial.events.empty());
        input.text("Hello 世界 😀\r\nnext\tline\xe2\x80\xa8 end");
        auto state = server.snapshot();
        CHECK(state.events.size() == 2);
        CHECK(state.events[0].kind == "string");
        CHECK(state.events[0].text == "Hello 世界 😀  next line  end");
        CHECK(state.events[1].kind == "commit" && state.events[1].number == 42);
        // Authorization is consumed even when a duplicate delivery is attempted.
        throws([&] { input.enter(); });
        CHECK(server.snapshot().events.size() == 2);
    }
    CHECK(server.wait_destroyed(1));
    CHECK(server.snapshot().events.size() == 2);
}

void test_enter() {
    FakeServer server;
    {
        TextInput input(server.socket);
        CHECK(server.snapshot().events.empty());
        input.enter();
        auto state = server.snapshot();
        CHECK(state.events.size() == 2);
        CHECK(state.events[0].kind == "action" &&
              state.events[0].number == GAMESCOPE_INPUT_METHOD_ACTION_SUBMIT);
        CHECK(state.events[1].kind == "commit" && state.events[1].number == 42);
        throws([&] { input.text("duplicate"); });
        CHECK(server.snapshot().events.size() == 2);
    }
    CHECK(server.wait_destroyed(1));
    CHECK(server.snapshot().events.size() == 2);
}

void test_invalid() {
    FakeServer server;
    {
        TextInput input(server.socket);
        throws([&] { input.text(std::string("x\0y", 3)); });
        throws([&] { input.text("\x1b[31m"); });
        throws([&] { input.text("\xc0\xaf"); });
        throws([&] { input.text("\xed\xa0\x80"); });
        throws([&] { input.text("\xf4\x90\x80\x80"); });
        throws([&] { input.text("\xe2\x82"); });
        throws([&] { input.text("\xc2\x85"); });
        throws([&] { input.text(std::string(4097, 'a')); });
        throws([&] { input.text("\t\n"); });
        CHECK(server.snapshot().events.empty());
        input.text("submit; $(echo x)");
        auto events = server.snapshot().events;
        CHECK(events.size() == 2 && events[0].text == "submit; $(echo x)");
        CHECK(events[1].kind == "commit");
    }
    CHECK(server.wait_destroyed(1));
}

void test_review_insert() {
    Session session;
    session.ready();
    CHECK(session.record() && session.finish(16000));
    auto id = session.id();
    CHECK(session.reply(id, "send\n世界"));
    CHECK(session.state() == State::Review);
    FakeServer server;
    {
        TextInput input(server.socket);
        CHECK(server.snapshot().events.empty()); // Discovery is not insertion.
        auto approved = session.take_insert();
        CHECK(approved == "send 世界" && !session.take_insert());
        CHECK(!session.reply(id, "duplicate"));
        input.text(*approved);
        auto events = server.snapshot().events;
        CHECK(events.size() == 2 && events[0].kind == "string");
        CHECK(events[0].text == "send 世界");
        CHECK(events[1].kind == "commit" && events[1].number == 42);
    }
    CHECK(server.wait_destroyed(1));
}

void test_unavailable() {
    FakeServer server(FakeServer::Mode::Unavailable);
    throws([&] { TextInput input(server.socket); });
    CHECK(server.wait_destroyed(1));
    auto state = server.snapshot();
    CHECK(state.creates == 1 && state.events.empty());
}

void test_stalled() {
    FakeServer server(FakeServer::Mode::Stalled);
    auto start = std::chrono::steady_clock::now();
    throws([&] { TextInput input(server.socket); });
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= 700ms && elapsed < 4s);
    CHECK(server.snapshot().creates == 0);
}

int main() {
    try {
        test_text(); test_enter(); test_invalid(); test_review_insert();
        test_unavailable(); test_stalled();
        std::cout << "native fake Wayland input checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << "text_input_test: " << e.what() << '\n';
        return 1;
    }
}
