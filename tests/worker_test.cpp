#include "worker.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sys/stat.h>
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
        worker.start(argv[1], argv[2], "request-error", 2);
        until([&] { worker.poll(); return worker.ready(); });
        worker.submit(200, clip);
        until([&] { reply = worker.poll(); return reply.has_value(); });
        assert(reply->id == 200 && reply->error == "transcription failed" && worker.ready());
        worker.submit(201, clip); // A request-local failure must not unload Redux.
        until([&] { reply = worker.poll(); return reply.has_value(); });
        assert(reply->id == 201 && reply->text == "héllo 世界" && worker.ready());
        worker.stop();
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
        // A separate fake worker proves stderr is not part of the framed pipe,
        // even when diagnostics fill its pipe before the ready frame.
        char scratch[] = "/tmp/frameyap-debug-test-XXXXXX";
        if (!::mkdtemp(scratch)) throw std::runtime_error("debug fixture setup failed");
        const auto root = std::filesystem::path(scratch);
        try {
            const auto script = root / "fake.py";
            {
                std::ofstream out(script);
                out << R"PY(import argparse, os, struct, sys
p = argparse.ArgumentParser()
p.add_argument('--model')
p.add_argument('--threads')
p.add_argument('--clip-dir')
p.add_argument('--advanced-debug', action='store_true')
a = p.parse_args()
protocol = os.dup(1)
if a.advanced_debug:
    os.dup2(2, 1)  # Match the production Python protocol/stdout split.
    os.write(2, b'PRIVATE TRANSCRIPT /private/model/path\n')
    os.write(1, b'PRIVATE STDOUT from model\n')
    if a.model == 'spam':
        for _ in range(1300): os.write(2, b'x' * 4096)
else:
    null = os.open(os.devnull, os.O_WRONLY)
    os.dup2(null, 1)
    os.close(null)
os.write(protocol, struct.pack('<I', 1) + b'Y')
while True:
    request = os.read(0, 13)
    if not request: break
    assert len(request) == 13 and request[4:5] == b'T'
    if a.model == 'unsafe':
        value = b'E' + request[5:] + b'transcription failed [inference: RuntimeError] PRIVATE SPEECH'
    elif a.model == 'safe':
        value = b'E' + request[5:] + b'transcription failed [inference: RuntimeError]'
    else:
        value = b'R' + request[5:] + b'ok'
    os.write(protocol, struct.pack('<I', len(value)) + value)
)PY";
            }
            const auto state = root / "state";
            ::setenv("XDG_STATE_HOME", state.c_str(), 1);
            const auto logs = state / "frameyap";
            const auto current = logs / "worker-debug.log";
            const auto previous = logs / "worker-debug.previous.log";
            worker.start(argv[1], script, "safe", 2); // OFF must not create files.
            until([&] { worker.poll(); return worker.ready(); });
            worker.submit(300, clip);
            until([&] { reply = worker.poll(); return reply.has_value(); });
            assert(reply->error == "transcription failed [inference: RuntimeError]");
            worker.stop();
            assert(!std::filesystem::exists(logs));

            worker.start(argv[1], script, "spam", 2, true);
            until([&] { worker.poll(); return worker.ready(); });
            worker.submit(301, clip);
            until([&] { reply = worker.poll(); return reply.has_value(); });
            assert(reply->text == "ok");
            worker.stop();
            assert(std::filesystem::status(logs).permissions() == std::filesystem::perms::owner_all);
            assert(std::filesystem::status(current).permissions() ==
                   (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
            assert(std::filesystem::file_size(current) <= 4 * 1024 * 1024);
            std::ifstream input(current, std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            assert(body.find("PRIVATE TRANSCRIPT /private/model/path") != std::string::npos);
            assert(body.find("PRIVATE STDOUT from model") != std::string::npos);
            assert(body.find("[worker diagnostics truncated; further output discarded]") != std::string::npos);
            worker.start(argv[1], script, "unsafe", 2, true);
            until([&] { worker.poll(); return worker.ready(); });
            worker.submit(302, clip);
            until([&] { reply = worker.poll(); return reply.has_value(); });
            assert(reply->error == "transcription failed" && worker.ready());
            worker.stop();
            assert(std::filesystem::file_size(previous) <= 4 * 1024 * 1024);
            assert(std::filesystem::file_size(current) < 4096);
            auto size = std::filesystem::file_size(current);
            worker.start(argv[1], script, "ok", 2); // OFF retains earlier logs.
            until([&] { worker.poll(); return worker.ready(); });
            worker.stop();
            assert(std::filesystem::file_size(current) == size && std::filesystem::exists(previous));
            std::filesystem::remove(current);
            std::filesystem::create_symlink(script, current);
            bool refused = false;
            try { worker.start(argv[1], script, "ok", 2, true); }
            catch (const std::runtime_error&) { refused = true; }
            assert(refused && !worker.ready());
            std::filesystem::remove(current);
            { std::ofstream out(current); out << "existing"; }
            ::chmod(current.c_str(), 0644);
            refused = false;
            try { worker.start(argv[1], script, "ok", 2, true); }
            catch (const std::runtime_error&) { refused = true; }
            assert(refused);
            std::filesystem::remove(current);
            { std::ofstream out(current); out << "linked"; }
            ::chmod(current.c_str(), 0600);
            std::filesystem::create_hard_link(current, root / "outside-link");
            refused = false;
            try { worker.start(argv[1], script, "ok", 2, true); }
            catch (const std::runtime_error&) { refused = true; }
            assert(refused);
            std::filesystem::remove(root / "outside-link");
            std::filesystem::remove(current);
            std::filesystem::remove_all(logs);
            std::filesystem::create_directory_symlink(root, logs);
            refused = false;
            try { worker.start(argv[1], script, "ok", 2, true); }
            catch (const std::runtime_error&) { refused = true; }
            assert(refused);
            std::filesystem::remove(logs);
            std::filesystem::create_directory(logs);
            ::chmod(logs.c_str(), 0755);
            refused = false;
            try { worker.start(argv[1], script, "ok", 2, true); }
            catch (const std::runtime_error&) { refused = true; }
            assert(refused);
            ::unsetenv("XDG_STATE_HOME");
            ::setenv("HOME", root.c_str(), 1);
            worker.start(argv[1], script, "ok", 2, true);
            until([&] { worker.poll(); return worker.ready(); });
            worker.stop();
            assert(std::filesystem::exists(root / ".local/state/frameyap/worker-debug.log"));
            ::setenv("XDG_STATE_HOME", "relative/state", 1);
            refused = false;
            try { worker.start(argv[1], script, "ok", 2, true); }
            catch (const std::runtime_error&) { refused = true; }
            assert(refused);
            std::filesystem::remove_all(root);
        } catch (...) { std::filesystem::remove_all(root); throw; }
        std::filesystem::remove(runtime);
    } catch (...) {
        std::filesystem::remove(runtime);
        throw;
    }
}
