#include "instance_lock.hpp"
#include <iostream>
#include <memory>
int main() {
    char pattern[] = "/tmp/frameyap-lock-test-XXXXXX";
    char* dir = mkdtemp(pattern);
    if (!dir) return 1;
    setenv("FRAMEYAP_INSTALL_ROOT", dir, 1);
    try {
        auto first = std::make_unique<frameyap::InstanceLock>();
        bool rejected = false;
        try { frameyap::InstanceLock second; } catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("second instance acquired lock");
        first.reset();
        { frameyap::InstanceLock next; }
        auto lock = std::filesystem::path(dir) / ".lock";
        std::filesystem::remove(lock);
        std::filesystem::create_symlink("foreign", lock);
        rejected = false;
        try { frameyap::InstanceLock unsafe; } catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("followed unsafe lock symlink");
        std::filesystem::remove(lock);
        std::filesystem::remove(dir);
        std::cout << "instance lock checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(dir);
        return 1;
    }
}
