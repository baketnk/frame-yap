#include "companion_apps.hpp"
#include <cerrno>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

namespace frameyap {
namespace {
bool executable(const std::filesystem::path& path) {
    struct stat info{};
    return path.is_absolute() && ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           ::access(path.c_str(), X_OK) == 0;
}
}
std::optional<std::filesystem::path> find_companion(std::string_view name) {
    if (name != "tnkplan" && name != "tnkboard") return {};
    if (const char* home = std::getenv("HOME")) {
        const auto launcher = std::filesystem::path(home) / ".local/bin" / name;
        if (executable(launcher)) return launcher;
    }
    if (const char* path = std::getenv("PATH")) {
        const std::string dirs(path);
        for (size_t start = 0; start <= dirs.size();) {
            const size_t end = dirs.find(':', start);
            const auto launcher = std::filesystem::path(dirs.substr(start, end - start)) / name;
            // Ignore empty/relative PATH entries: never execute from an untrusted cwd.
            if (executable(launcher)) return launcher;
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    return {};
}
bool launch_companion(const std::filesystem::path& launcher) {
    if (!executable(launcher)) return false;
    const auto path = launcher.string();
    char* const args[] = {const_cast<char*>(path.c_str()), nullptr};
    // Double fork so the long-lived companion is adopted rather than becoming
    // a zombie owned by FrameYap. An O_CLOEXEC pipe reports exec failure only.
    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) != 0) return false;
    const pid_t intermediate = ::fork();
    if (intermediate < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return false; }
    if (intermediate == 0) {
        ::close(pipefd[0]);
        const pid_t child = ::fork();
        if (child < 0) {
            const int error = errno;
            (void)::write(pipefd[1], &error, sizeof(error));
        } else if (child == 0) {
            ::execv(path.c_str(), args);
            const int error = errno;
            (void)::write(pipefd[1], &error, sizeof(error));
        }
        ::_exit(child < 0 ? 1 : 0);
    }
    ::close(pipefd[1]);
    int error = 0;
    ssize_t count;
    do { count = ::read(pipefd[0], &error, sizeof(error)); } while (count < 0 && errno == EINTR);
    ::close(pipefd[0]);
    int status = 0;
    pid_t reaped;
    do { reaped = ::waitpid(intermediate, &status, 0); } while (reaped < 0 && errno == EINTR);
    return count == 0 && reaped == intermediate && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}
