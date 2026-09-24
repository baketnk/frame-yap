#include "worker.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace frameyap;
using namespace std::chrono_literals;

static void until(const std::function<bool()>& fn) {
    auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!fn()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("fixture timed out");
        std::this_thread::sleep_for(5ms);
    }
}

int main(int argc, char** argv) {
    // CMake should pass its Python interpreter and the absolute test_worker.py path.
    if (argc != 3) return 2;
    char runtime[] = "/tmp/frameyap-test-XXXXXX";
    if (!::mkdtemp(runtime)) return 3;
    ::setenv("XDG_RUNTIME_DIR", runtime, 1);
    try {
        std::vector<float> clip(3200, 0.25f);
        Worker worker;
        worker.start(argv[1], argv[2], "ok", 2);
        assert(!worker.ready());
        until([&] { worker.poll(); return worker.ready(); });
        bool rejected = false;
        try { worker.submit(1, std::vector<float>(3199)); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
        worker.submit(100, clip);
        rejected = false;
        try { worker.submit(101, clip); }
        catch (const std::logic_error&) { rejected = true; }
        assert(rejected);
        std::optional<WorkerReply> reply;
        until([&] { reply = worker.poll(); return reply.has_value(); });
        assert(reply->id == 100 && reply->text == "héllo 世界" && reply->error.empty());
        worker.submit(101, clip);
        until([&] { reply = worker.poll(); return reply.has_value(); });
        assert(reply->id == 101);
        worker.stop();
        assert(!worker.ready());
        for (const auto& entry : std::filesystem::directory_iterator(runtime)) {
            (void)entry;
            throw std::runtime_error("private directory leaked");
        }
        worker.start(argv[1], argv[2], "fail", 2);
        bool failed = false;
        until([&] {
            try { worker.poll(); }
            catch (const std::runtime_error&) { failed = true; }
            return failed;
        });
        assert(!worker.ready());
        for (const auto& [mode, message] : std::vector<std::pair<const char*, const char*>>{
                 {"missing-model", "check --model"}, {"missing-import", "check --python"}}) {
            worker.start(argv[1], argv[2], mode, 2);
            failed = false;
            until([&] {
                try { worker.poll(); }
                catch (const std::runtime_error& e) { failed = std::string(e.what()).find(message) != std::string::npos; }
                return failed;
            });
            assert(!worker.ready());
        }
        worker.start(argv[1], argv[2], "stale", 2);
        until([&] { worker.poll(); return worker.ready(); });
        worker.submit(4, clip);
        failed = false;
        until([&] {
            try { worker.poll(); }
            catch (const std::runtime_error&) { failed = true; }
            return failed;
        });
        for (const char* mode : {"long", "oversized-frame", "duplicate"}) {
            worker.start(argv[1], argv[2], mode, 2);
            until([&] { worker.poll(); return worker.ready(); });
            worker.submit(5, clip);
            failed = false;
            until([&] {
                try { worker.poll(); }
                catch (const std::runtime_error&) { failed = true; }
                return failed;
            });
            assert(!worker.ready());
        }
        // Cancellation must not wait for inference or leak the private clip.
        worker.start(argv[1], argv[2], "hang", 2);
        until([&] { worker.poll(); return worker.ready(); });
        worker.submit(6, clip);
        auto cancel_start = std::chrono::steady_clock::now();
        worker.stop();
        assert(std::chrono::steady_clock::now() - cancel_start < 2s);
        assert(std::filesystem::is_empty(runtime));
        worker.start(argv[1], argv[2], "hang-warm", 2);
        worker.stop();
        assert(std::filesystem::is_empty(runtime));
        worker.start(argv[1], argv[2], "crash", 2);
        failed = false;
        until([&] {
            try { worker.poll(); }
            catch (const std::runtime_error&) { failed = true; }
            return failed;
        });
        worker.stop();
        Worker deadlines(2s, 50ms);
        deadlines.start(argv[1], argv[2], "hang", 2);
        until([&] { deadlines.poll(); return deadlines.ready(); });
        deadlines.submit(9, clip);
        failed = false;
        until([&] {
            try { deadlines.poll(); }
            catch (const std::runtime_error& e) { failed = std::string(e.what()).find("timed out") != std::string::npos; }
            return failed;
        });
        assert(std::filesystem::is_empty(runtime));
        Worker warm_deadline(50ms, 2s);
        warm_deadline.start(argv[1], argv[2], "hang-warm", 2);
        failed = false;
        until([&] {
            try { warm_deadline.poll(); }
            catch (const std::runtime_error& e) { failed = std::string(e.what()).find("timed out") != std::string::npos; }
            return failed;
        });
        assert(std::filesystem::is_empty(runtime));
        std::filesystem::remove(runtime);
    } catch (...) {
        std::filesystem::remove(runtime);
        throw;
    }
}
