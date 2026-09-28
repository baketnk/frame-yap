#include "update_check.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace frameyap;
namespace {
UpdateResult finish(UpdateCheck& checker) {
    for (int i = 0; i < 300; ++i) {
        if (auto result = checker.poll()) return *result;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(false && "update helper did not complete");
    return {UpdateResult::State::Failed, {}};
}
}
int main(int argc, char** argv) {
    assert(argc == 2);
    char pattern[] = "/tmp/frameyap-update-XXXXXX";
    const auto root = fs::path(::mkdtemp(pattern));
    const auto helper = root / "fixture.py";
    UpdateCheck check(helper, "0.1.202609282232", argv[1]);
    assert(!check.busy() && !check.poll());
    assert(!check.start()); // missing file; no child, no network
    std::ofstream(helper) << "import time\ntime.sleep(.2)\nprint('AVAILABLE 0.1.202609292232')\n";
    assert(check.start() && check.busy());
    assert(!check.start()); // one explicit check at a time
    assert(!check.poll()); // interaction loop does not wait for network/child
    auto result = finish(check);
    assert(result.state == UpdateResult::State::Available && result.version == "0.1.202609292232");
    assert(!check.busy());
    std::ofstream(helper) << "print('CURRENT')\n";
    assert(check.start());
    assert(finish(check).state == UpdateResult::State::Current);
    std::ofstream(helper) << "print('AVAILABLE 0.1.20260929;echo surprise')\n";
    assert(check.start());
    assert(finish(check).state == UpdateResult::State::Failed);
    std::ofstream(helper) << "print('x'*400)\n";
    assert(check.start());
    assert(finish(check).state == UpdateResult::State::Failed);
    std::ofstream(helper) << "raise SystemExit(1)\n";
    assert(check.start());
    assert(finish(check).state == UpdateResult::State::Failed);
    assert(!open_update_terminal(root / "missing.py", "0.1.202609292232"));
    assert(!open_update_terminal(helper, "0.1.20260929;echo surprise"));
    const char* path = ::getenv("PATH");
    const std::string old_path = path ? path : "";
    ::setenv("PATH", root.c_str(), 1);
    assert(!open_update_terminal(helper, "0.1.202609292232")); // no Konsole
    ::setenv("PATH", old_path.c_str(), 1);
    fs::remove_all(root);
}
