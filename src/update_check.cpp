#include "update_check.hpp"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <regex>

extern char** environ;
namespace frameyap {
namespace {
bool release_version(const std::string& version) {
    static const std::regex pattern(R"((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.[1-9][0-9]{11})");
    return std::regex_match(version, pattern);
}
std::optional<std::filesystem::path> terminal_executable() {
    const char* path = std::getenv("PATH");
    if (!path) return {};
    const std::string dirs(path);
    for (size_t start = 0; start <= dirs.size();) {
        const size_t end = dirs.find(':', start);
        const auto candidate = std::filesystem::path(dirs.substr(start, end - start)) / "konsole";
        struct stat info{};
        if (candidate.is_absolute() && ::stat(candidate.c_str(), &info) == 0 &&
            S_ISREG(info.st_mode) && ::access(candidate.c_str(), X_OK) == 0) return candidate;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}
}
UpdateCheck::UpdateCheck(std::filesystem::path helper, std::string current, std::string python)
    : helper_(std::move(helper)), current_(std::move(current)), python_(std::move(python)) {}
UpdateCheck::~UpdateCheck() { stop(); }
void UpdateCheck::stop() noexcept {
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM); // exact owned checker, never SteamVR or the terminal
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < 20; ++i) {
            auto result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_ || (result < 0 && errno == ECHILD)) { reaped = true; break; }
            if (result < 0 && errno != EINTR) break;
            ::usleep(5000);
        }
        if (!reaped) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        }
        pid_ = -1;
    }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    output_.clear(); eof_ = false;
}
bool UpdateCheck::start() {
    if (busy() || !helper_.is_absolute() || !std::filesystem::is_regular_file(helper_) ||
        !release_version(current_)) return false;
    int pipe[2];
    if (::pipe2(pipe, O_CLOEXEC) < 0) return false;
    if (::fcntl(pipe[0], F_SETFL, O_NONBLOCK) < 0) {
        ::close(pipe[0]); ::close(pipe[1]); return false;
    }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) { ::close(pipe[0]); ::close(pipe[1]); return false; }
    error = posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, pipe[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, pipe[1]);
    const auto helper = helper_.string();
    std::vector<std::string> args{python_, "-I", helper, "--check", "--current", current_};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t child = -1;
    if (!error) error = ::posix_spawnp(&child, python_.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe[1]);
    if (error) { ::close(pipe[0]); return false; }
    pid_ = child; fd_ = pipe[0]; eof_ = false; output_.clear();
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    return true;
}
std::optional<UpdateResult> UpdateCheck::poll() {
    if (!busy()) return {};
    if (std::chrono::steady_clock::now() > deadline_) { stop(); return UpdateResult{UpdateResult::State::Failed, {}}; }
    if (!eof_) {
        char bytes[128];
        for (;;) {
            const auto n = ::read(fd_, bytes, sizeof bytes);
            if (n > 0) {
                output_.append(bytes, size_t(n));
                if (output_.size() > 128) { stop(); return UpdateResult{UpdateResult::State::Failed, {}}; }
            } else if (n == 0) { ::close(fd_); fd_ = -1; eof_ = true; break; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else { stop(); return UpdateResult{UpdateResult::State::Failed, {}}; }
        }
    }
    if (!eof_) return {};
    int status = 0;
    const auto reaped = ::waitpid(pid_, &status, WNOHANG);
    if (reaped == 0) return {};
    pid_ = -1;
    if (reaped < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
        return UpdateResult{UpdateResult::State::Failed, {}};
    if (output_ == "CURRENT\n") return UpdateResult{UpdateResult::State::Current, {}};
    constexpr std::string_view prefix = "AVAILABLE ";
    if (output_.starts_with(prefix) && output_.ends_with('\n')) {
        auto version = output_.substr(prefix.size(), output_.size() - prefix.size() - 1);
        if (release_version(version) && version != current_)
            return UpdateResult{UpdateResult::State::Available, std::move(version)};
    }
    return UpdateResult{UpdateResult::State::Failed, {}};
}
bool open_update_terminal(const std::filesystem::path& helper, const std::string& version) {
    if (!helper.is_absolute() || !std::filesystem::is_regular_file(helper) || !release_version(version)) return false;
    const auto terminal = terminal_executable();
    if (!terminal) return false;
    const auto script = helper.string();
    std::vector<std::string> args{terminal->string(), "--hold", "-e", "python3", "-I", script, "--version", version};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    // Konsole is an independent desktop window and must survive FrameYap quitting.
    // Double fork avoids a zombie; the close-on-exec pipe detects launch errors.
    int pipe[2];
    if (::pipe2(pipe, O_CLOEXEC)) return false;
    const pid_t intermediate = ::fork();
    if (intermediate < 0) { ::close(pipe[0]); ::close(pipe[1]); return false; }
    if (intermediate == 0) {
        ::close(pipe[0]);
        const pid_t child = ::fork();
        if (child == 0) {
            ::execv(args[0].c_str(), argv.data());
            const int error = errno;
            (void)::write(pipe[1], &error, sizeof(error));
        } else if (child < 0) {
            const int error = errno;
            (void)::write(pipe[1], &error, sizeof(error));
        }
        ::_exit(child < 0 ? 1 : child == 0 ? 127 : 0);
    }
    ::close(pipe[1]);
    int error = 0;
    ssize_t count;
    do { count = ::read(pipe[0], &error, sizeof(error)); } while (count < 0 && errno == EINTR);
    ::close(pipe[0]);
    int status = 0;
    pid_t reaped;
    do { reaped = ::waitpid(intermediate, &status, 0); } while (reaped < 0 && errno == EINTR);
    return count == 0 && reaped == intermediate && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
} // namespace frameyap
