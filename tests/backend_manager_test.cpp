#include "backend_manager.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace frameyap;
namespace {
const std::string digest(64, 'a');
void put(const std::filesystem::path& file, const std::string& contents) {
    std::ofstream out(file); out << contents;
}
void until_idle(BackendManager& manager) {
    for (int n = 0; n < 500 && manager.busy(); ++n) {
        manager.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(!manager.busy());
}
std::string status() {
    return "print('ST\\tfake\\tFake%20backend\\tnot_installed\\tmissing_files\\t7\\thttps%3A%2F%2Fexample.org\\tMIT\\tTest%20license\\tFixture\\t/tmp/fake\\t" + digest + "', flush=True)\n"
           " print('DONE', flush=True)\n";
}
}
int main() {
    auto path = std::filesystem::temp_directory_path() / ("frameyap-backend-test-" + std::to_string(getpid()));
    std::filesystem::create_directory(path);
    auto service = path / "service.py";
    put(service, "import sys\nif '--status' in sys.argv:\n " + status() +
                 "else:\n assert sys.argv[sys.argv.index('--expected-manifest-sha256') + 1] == '" + digest + "'\n"
                 " print('{\"ok\": true, \"event\": \"complete\"}', flush=True)\n");
    {
        BackendManager manager("python3", service.string(), path.string(), path.string(), service.string());
        assert(!manager.busy());
        manager.refresh(); assert(manager.busy());
        until_idle(manager);
        assert(manager.entries().size() == 1 && manager.entries()[0].id == "fake");
        assert(manager.entries()[0].bytes == 7 && manager.entries()[0].name == "Fake backend");
        assert(manager.entries()[0].state == "not_installed" && manager.checked());
        assert(manager.entries()[0].manifest_sha256 == digest);
        assert(manager.revision() == 1 && manager.model_path("fake") == "/tmp/fake");
        try { manager.install("unknown", digest); assert(false); } catch (const std::runtime_error&) {}
        try { manager.install("fake", std::string(64, 'b')); assert(false); } catch (const std::runtime_error&) {}
        assert(!manager.busy()); // mismatched consent cannot launch a helper
        manager.install("fake", digest);
        assert(manager.installing());
        until_idle(manager); // success must trigger a fresh status check
        assert(manager.revision() == 2 && manager.checked());
        manager.refresh(); manager.cancel(); assert(!manager.busy());
    }
    // Installer output larger than one poll budget may remain after helper exit.
    // Success still requires EOF followed by a fresh offline status check.
    put(service, "import os, sys\nif '--status' in sys.argv:\n " + status() +
                 "else:\n assert os.get_blocking(1), 'child stdout must be blocking'\n"
                 " for _ in range(3000): print('{\"event\":\"complete\",\"ok\":true}')\n");
    {
        BackendManager manager("python3", service.string(), path.string(), path.string(), service.string());
        manager.refresh(); until_idle(manager); assert(manager.checked());
        manager.install("fake", digest); until_idle(manager);
        assert(manager.checked() && manager.revision() == 2);
    }
    // A fast-exiting helper can leave output behind. Read through EOF, then
    // reject any trailing record after DONE, never accept a success substring.
    put(service, "import sys\nif True:\n " + status() + "print('trailing', flush=True)\n");
    {
        BackendManager manager("python3", service.string(), path.string(), path.string(), service.string());
        manager.refresh(); until_idle(manager);
        assert(!manager.checked() && manager.note() == "Local model check failed.");
    }
    put(service, "import sys\nif '--status' in sys.argv:\n " + status() +
                 "else:\n print('{\"event\":\"complete\",\"ok\":true}')\n raise SystemExit(1)\n");
    {
        BackendManager manager("python3", service.string(), path.string(), path.string(), service.string());
        manager.refresh(); until_idle(manager); assert(manager.checked());
        manager.install("fake", digest); until_idle(manager);
        assert(manager.checked() && manager.revision() == 3); // failure plus offline recheck
        assert(manager.note() == "Install failed: installer exited unsuccessfully; local model status checked offline.");
        manager.refresh(); until_idle(manager);
        assert(manager.note() == "Install failed: installer exited unsuccessfully; local model status checked offline.");
    }
    // JSON is display-only. Per-file progress appears before process exit and
    // a structured error survives rechecks without echoing terminal controls.
    put(service, "import sys, time\nif '--status' in sys.argv:\n " + status() +
                 "else:\n print('{\"ok\":true,\"event\":\"model_file\",\"file\":\"weights.dat\",\"state\":\"downloading\",\"bytes\":7}', flush=True)\n"
                 " time.sleep(.2)\n"
                 " print('{\"ok\":true,\"event\":\"model_file\",\"file\":\"weights.dat\",\"state\":\"verified\"}', flush=True)\n"
                 " time.sleep(.2)\n"
                 " print('{\"ok\":false,\"code\":\"manifest_mismatch\",\"message\":\"metadata changed \\\\u001b[31m\"}', flush=True)\n"
                 " raise SystemExit(1)\n");
    {
        BackendManager manager("python3", service.string(), path.string(), path.string(), service.string());
        manager.refresh(); until_idle(manager); manager.install("fake", digest);
        bool downloading = false, verified = false;
        for (int n = 0; n < 500 && manager.busy(); ++n) {
            manager.poll();
            downloading |= manager.note() == "Downloading: weights.dat";
            verified |= manager.note() == "Verified file: weights.dat";
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        assert(downloading && verified && manager.checked());
        assert(manager.note().find("manifest_mismatch: metadata changed ?[31m") != std::string::npos);
        manager.refresh();
        assert(manager.note().find("metadata changed") != std::string::npos); // during recheck
        until_idle(manager);
        assert(manager.note().find("metadata changed") != std::string::npos);
    }
    // A lying success event, unknown event, and malformed object never certify
    // installation: exit status and the fresh offline check still decide.
    put(service, "import sys\nif '--status' in sys.argv:\n " + status() +
                 "else:\n print('{\"ok\":true,\"event\":\"complete\"}', flush=True)\n"
                 " print('{\"ok\":true,\"event\":\"model_file\",\"state\":\"unknown\",\"file\":\"fake\"}', flush=True)\n"
                 " print('{\"ok\":true,\"event\":\"model_file\",\"file\":{}}', flush=True)\n"
                 " raise SystemExit(1)\n");
    {
        BackendManager manager("python3", service.string(), path.string(), path.string(), service.string());
        manager.refresh(); until_idle(manager); manager.install("fake", digest); until_idle(manager);
        assert(manager.checked() && manager.note().find("Install failed:") == 0);
    }
    // A truncated object cannot inject even advisory error text. Complete lines
    // are bounded too, not only a partial line waiting for its newline.
    for (bool oversized : {false, true}) {
        put(service, "import sys\nif '--status' in sys.argv:\n " + status() +
                     "else:\n " + (oversized ? std::string("print('x' * 9000, flush=True)\n") :
                         std::string("print('{\"ok\":false,\"message\":\"not-real\",', flush=True)\n")) +
                     " raise SystemExit(1)\n");
        BackendManager manager("python3", service.string(), path.string(), path.string(), service.string());
        manager.refresh(); until_idle(manager); manager.install("fake", digest); until_idle(manager);
        assert(manager.note().find("not-real") == std::string::npos);
        if (oversized) assert(!manager.checked() && manager.note().find("oversized") != std::string::npos);
        else assert(manager.checked() && manager.note().find("installer exited unsuccessfully") != std::string::npos);
    }
    // Cancellation gives the exact owned child a bounded SIGTERM cleanup window.
    auto started = path / "started", terminated = path / "terminated";
    put(service, "import pathlib, signal, sys, time\nif '--status' in sys.argv:\n " + status() +
                 "else:\n"
                 " def stop(*_):\n  pathlib.Path('" + terminated.string() + "').write_text('done')\n  raise SystemExit(1)\n"
                 " signal.signal(signal.SIGTERM, stop)\n"
                 " pathlib.Path('" + started.string() + "').write_text('ready')\n"
                 " time.sleep(60)\n");
    {
        BackendManager manager("python3", service.string(), path.string(), path.string(), service.string());
        manager.refresh(); until_idle(manager); assert(manager.checked());
        manager.install("fake", digest);
        for (int n = 0; n < 500 && !std::filesystem::exists(started); ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(std::filesystem::exists(started));
        manager.cancel();
        assert(!manager.busy() && std::filesystem::exists(terminated));
    }
    std::filesystem::remove_all(path);
}
