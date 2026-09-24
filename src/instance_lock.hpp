#pragma once
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace frameyap {
// Shared contract with install.sh. Held for the entire runtime/check/registration
// operation, never acquired by default/help/version or hardware-free tests.
class InstanceLock {
public:
    InstanceLock() {
        const char* installed = std::getenv("FRAMEYAP_INSTALL_ROOT");
        const char* data = std::getenv("XDG_DATA_HOME");
        const char* home = std::getenv("HOME");
        if ((!installed || !*installed) && (!data || !*data) && (!home || !*home))
            throw std::runtime_error("HOME or XDG_DATA_HOME required");
        auto root = installed && *installed ? std::filesystem::path(installed) :
            std::filesystem::path(data && *data ? data : std::string(home) + "/.local/share") / "frameyap";
        if (!root.is_absolute() || std::filesystem::is_symlink(root))
            throw std::runtime_error("Data directory must be absolute and not a symlink");
        std::filesystem::create_directories(root);
        fd_ = open((root / ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat st{};
        if (fd_ < 0 || fstat(fd_, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || flock(fd_, LOCK_EX | LOCK_NB)) {
            if (fd_ >= 0) close(fd_);
            fd_ = -1;
            throw std::runtime_error("FrameYap already running/installing, or unsafe install lock");
        }
    }
    ~InstanceLock() { if (fd_ >= 0) close(fd_); }
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
private:
    int fd_ = -1;
};
}
