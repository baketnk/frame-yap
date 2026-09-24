#include "worker.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace frameyap {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t max_frame = 65536;
constexpr size_t max_text = 4096;
constexpr size_t max_debug_log = 4 * 1024 * 1024;
constexpr std::string_view truncation = "\n[worker diagnostics truncated; further output discarded]\n";

void close_fd(int& fd) { if (fd >= 0) { ::close(fd); fd = -1; } }
void put32(unsigned char* p, uint32_t n) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(n >> (8 * i));
}
void put64(unsigned char* p, uint64_t n) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(n >> (8 * i));
}
uint32_t get32(const unsigned char* p) {
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n |= uint32_t(p[i]) << (8 * i);
    return n;
}
uint64_t get64(const unsigned char* p) {
    uint64_t n = 0;
    for (int i = 0; i < 8; ++i) n |= uint64_t(p[i]) << (8 * i);
    return n;
}
bool valid_utf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        unsigned char first = static_cast<unsigned char>(text[i]);
        if (first == 0) return false;
        if (first < 0x80) { ++i; continue; }
        unsigned count = first >= 0xc2 && first <= 0xdf ? 2 :
                         first >= 0xe0 && first <= 0xef ? 3 :
                         first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (!count || i + count > text.size()) return false;
        for (unsigned j = 1; j < count; ++j)
            if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80) return false;
        unsigned char second = static_cast<unsigned char>(text[i + 1]);
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return false;
        i += count;
    }
    return true;
}
std::string safe_request_error(std::string_view error) {
    // Treat E payloads as untrusted too: older/alternate workers might include
    // raw exception messages. Only fixed diagnostic labels reach UI or logs.
    for (const auto* stage : {"audio", "inference", "response", "unknown"}) {
        for (const auto* category : {"MemoryError", "ImportError", "OSError", "UnicodeError", "TypeError",
                                     "ValueError", "KeyError", "IndexError", "RuntimeError", "Exception"}) {
            auto allowed = std::string("transcription failed [") + stage + ": " + category + "]";
            if (error == allowed) return allowed;
        }
    }
    return "transcription failed"; // compatible legacy/unknown error, never raw text
}
// Walk from / using directory fds: no symlinks, even in intermediate components.
// Root-owned system ancestors (including sticky /tmp) are permitted, but a
// writable non-sticky ancestor could redirect private diagnostics elsewhere.
int state_directory() {
    const char* base = ::getenv("XDG_STATE_HOME");
    std::string path;
    if (base && *base) path = base;
    else {
        const char* home = ::getenv("HOME");
        if (!home || !*home) throw std::runtime_error("advanced debug needs HOME or XDG_STATE_HOME");
        path = std::string(home) + "/.local/state";
    }
    if (path.empty() || path[0] != '/')
        throw std::runtime_error("advanced debug state path must be absolute");
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    path += path == "/" ? "frameyap" : "/frameyap";
    int dir = ::open("/", O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) throw std::runtime_error("cannot open debug state root");
    try {
        size_t pos = 1;
        while (pos < path.size()) {
            auto end = path.find('/', pos);
            if (end == std::string::npos) end = path.size();
            auto part = path.substr(pos, end - pos);
            if (part.empty() || part == "." || part == "..")
                throw std::runtime_error("unsafe debug state path");
            if (::mkdirat(dir, part.c_str(), 0700) && errno != EEXIST)
                throw std::runtime_error("cannot create private debug state directory");
            int next = ::openat(dir, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) throw std::runtime_error("unsafe debug state directory");
            struct stat st{};
            bool final = end == path.size();
            if (::fstat(next, &st) || !S_ISDIR(st.st_mode) ||
                (final && (st.st_uid != ::geteuid() || (st.st_mode & 07777) != 0700)) ||
                (!final && ((st.st_uid != ::geteuid() && st.st_uid != 0) ||
                  ((st.st_mode & 0022) && !(st.st_uid == 0 && (st.st_mode & S_ISVTX)))))) {
                ::close(next);
                throw std::runtime_error("unsafe debug state directory permissions");
            }
            ::close(dir); dir = next;
            pos = end + 1;
        }
        return dir;
    } catch (...) { ::close(dir); throw; }
}

// Open existing files without following links; never rotate someone else's file,
// a hardlink or a permissive target. Both names are checked before any mutation.
void check_log_target(int dir, const char* name) {
    int fd = ::openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return;
        throw std::runtime_error("unsafe existing worker debug log");
    }
    struct stat st{};
    bool valid = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 st.st_uid == ::geteuid() && (st.st_mode & 07777) == 0600 &&
                 st.st_nlink == 1 && st.st_size <= static_cast<off_t>(max_debug_log);
    ::close(fd);
    if (!valid) throw std::runtime_error("unsafe existing worker debug log");
}
int create_debug_log() {
    int dir = state_directory();
    try {
        check_log_target(dir, "worker-debug.log");
        check_log_target(dir, "worker-debug.previous.log");
        if (::unlinkat(dir, "worker-debug.previous.log", 0) && errno != ENOENT)
            throw std::runtime_error("cannot rotate worker debug log");
        if (::renameat(dir, "worker-debug.log", dir, "worker-debug.previous.log") && errno != ENOENT)
            throw std::runtime_error("cannot rotate worker debug log");
        int fd = ::openat(dir, "worker-debug.log", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) throw std::runtime_error("cannot create private worker debug log");
        ::close(dir);
        return fd;
    } catch (...) { ::close(dir); throw; }
}
void log_bytes(int fd, const unsigned char* data, size_t size) {
    while (size) {
        ssize_t n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("worker debug log write failed");
        data += n; size -= static_cast<size_t>(n);
    }
}
void check_runtime(const char* path) {
    if (!path || path[0] != '/') throw std::runtime_error("XDG_RUNTIME_DIR must be an absolute private directory");
    struct stat st{};
    if (::lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 0077))
        throw std::runtime_error("XDG_RUNTIME_DIR must be owned by this user and inaccessible to others (no symlinks)");
}
// Blocking SIGPIPE only on this thread avoids changing the host application's signal disposition.
ssize_t safe_write(int fd, const void* data, size_t size) {
    sigset_t mask{}, prior{}, pending{};
    sigemptyset(&mask); sigaddset(&mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &mask, &prior)) throw std::runtime_error("cannot block SIGPIPE");
    sigpending(&pending);
    bool was_pending = sigismember(&pending, SIGPIPE);
    ssize_t result = ::write(fd, data, size);
    int saved = errno;
    if (result < 0 && saved == EPIPE && !was_pending) {
        timespec zero{};
        ::sigtimedwait(&mask, nullptr, &zero);
    }
    pthread_sigmask(SIG_SETMASK, &prior, nullptr);
    errno = saved;
    return result;
}
void write_all(int fd, const unsigned char* data, size_t size) {
    while (size) {
        ssize_t n = safe_write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("worker pipe write failed");
        data += n; size -= static_cast<size_t>(n);
    }
}
} // namespace

struct Worker::State {
    pid_t pid = -1;
    int to_child = -1, from_child = -1, debug_pipe = -1, debug_file = -1;
    size_t debug_written = 0;
    bool debug_truncated = false;
    std::string dir;
    bool loaded = false;
    std::optional<uint64_t> pending;
    Clock::time_point deadline{};
    std::vector<unsigned char> input;
    std::chrono::milliseconds warmup_timeout{120000}, request_timeout{60000};

    void drain_debug() {
        if (debug_pipe < 0) return;
        // Limit work per poll: even an endlessly noisy child cannot trap the UI.
        size_t budget = 256 * 1024;
        unsigned char block[4096];
        while (budget) {
            ssize_t n = ::read(debug_pipe, block, std::min(budget, sizeof block));
            if (n > 0) {
                budget -= static_cast<size_t>(n);
                if (!debug_truncated) {
                    size_t left = max_debug_log - truncation.size() - debug_written;
                    size_t count = std::min(left, static_cast<size_t>(n));
                    log_bytes(debug_file, block, count);
                    debug_written += count;
                    if (count < static_cast<size_t>(n)) {
                        log_bytes(debug_file, reinterpret_cast<const unsigned char*>(truncation.data()), truncation.size());
                        debug_written += truncation.size();
                        debug_truncated = true;
                    }
                }
                continue;
            }
            if (n == 0) { close_fd(debug_pipe); return; }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                throw std::runtime_error("worker debug pipe read failed");
            return;
        }
    }
};

Worker::Worker(std::chrono::milliseconds warmup, std::chrono::milliseconds request)
    : state_(std::make_unique<State>()) {
    if (warmup.count() <= 0 || request.count() <= 0) throw std::invalid_argument("positive worker deadlines required");
    state_->warmup_timeout = warmup;
    state_->request_timeout = request;
}
Worker::~Worker() { stop(); }

void Worker::stop() {
    auto& s = *state_;
    close_fd(s.to_child);
    close_fd(s.from_child);
    if (s.pid > 0) {
        ::kill(s.pid, SIGTERM); // Only our direct child, never a process group.
        auto until = Clock::now() + std::chrono::milliseconds(500);
        int status = 0;
        while (::waitpid(s.pid, &status, WNOHANG) == 0 && Clock::now() < until) {
            try { s.drain_debug(); } catch (...) {} // stop/destructor are best effort
            ::usleep(10000);
        }
        if (::waitpid(s.pid, &status, WNOHANG) == 0) {
            ::kill(s.pid, SIGKILL);
            while (::waitpid(s.pid, &status, 0) < 0 && errno == EINTR) {}
        }
        s.pid = -1;
    }
    try { s.drain_debug(); } catch (...) {}
    close_fd(s.debug_pipe);
    close_fd(s.debug_file);
    s.debug_written = 0; s.debug_truncated = false;
    if (!s.dir.empty()) {
        ::unlink((s.dir + "/clip.raw").c_str());
        ::rmdir(s.dir.c_str());
        s.dir.clear();
    }
    s.input.clear(); s.pending.reset(); s.loaded = false;
}

void Worker::start(const std::string& python, const std::string& script,
                   const std::string& model, int threads, bool advanced_debug) {
    if (state_->pid > 0) throw std::logic_error("worker already started");
    if (python.empty() || script.empty() || model.empty() || threads < 1 || threads > 64)
        throw std::invalid_argument("python, script, local model and 1..64 threads required");
    check_runtime(::getenv("XDG_RUNTIME_DIR"));
    try {
        // The model's internal files are checked by the child before loading, never fetched.
        std::string pattern = std::string(::getenv("XDG_RUNTIME_DIR")) + "/frameyap-XXXXXX";
        std::vector<char> tmp(pattern.begin(), pattern.end()); tmp.push_back('\0');
        if (!::mkdtemp(tmp.data())) throw std::runtime_error("cannot create private clip directory");
        state_->dir = tmp.data();
        ::chmod(state_->dir.c_str(), 0700);
        if (advanced_debug) state_->debug_file = create_debug_log();
        int in[2]{-1,-1}, out[2]{-1,-1}, debug[2]{-1,-1};
        if (::pipe2(in, O_CLOEXEC) || ::pipe2(out, O_CLOEXEC) ||
            (advanced_debug && ::pipe2(debug, O_CLOEXEC))) {
            close_fd(in[0]); close_fd(in[1]); close_fd(out[0]); close_fd(out[1]);
            close_fd(debug[0]); close_fd(debug[1]);
            throw std::runtime_error("cannot create worker pipes");
        }
        std::string thread_arg = std::to_string(threads);
        // OpenVR/SDL already have threads. posix_spawn avoids allocations and
        // setenv in a forked child of a multithreaded process.
        std::vector<std::string> environment;
        for (char** e = environ; *e; ++e) {
            std::string_view entry(*e);
            if (entry.starts_with("PYTHONDONTWRITEBYTECODE=")) continue;
            environment.emplace_back(*e);
        }
        environment.emplace_back("PYTHONDONTWRITEBYTECODE=1");
        std::vector<char*> envp;
        for (auto& value : environment) envp.push_back(value.data());
        envp.push_back(nullptr);
        const char* args[] = {python.c_str(), script.c_str(), "--model", model.c_str(),
            "--threads", thread_arg.c_str(), "--clip-dir", state_->dir.c_str(),
            advanced_debug ? "--advanced-debug" : nullptr, nullptr};
        posix_spawn_file_actions_t actions;
        int error = posix_spawn_file_actions_init(&actions);
        if (error) {
            close_fd(in[0]); close_fd(in[1]); close_fd(out[0]); close_fd(out[1]);
            close_fd(debug[0]); close_fd(debug[1]);
            throw std::runtime_error("cannot initialize worker spawn");
        }
        error = posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        if (!error && advanced_debug) error = posix_spawn_file_actions_adddup2(&actions, debug[1], STDERR_FILENO);
        if (!error && !advanced_debug) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        for (int fd : std::array<int, 6>{in[0], in[1], out[0], out[1], debug[0], debug[1]})
            if (!error && fd > STDERR_FILENO) error = posix_spawn_file_actions_addclose(&actions, fd);
        pid_t pid = -1;
        if (!error) error = posix_spawnp(&pid, python.c_str(), &actions, nullptr,
                                        const_cast<char* const*>(args), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        if (error) {
            close_fd(in[0]); close_fd(in[1]); close_fd(out[0]); close_fd(out[1]);
            close_fd(debug[0]); close_fd(debug[1]);
            throw std::runtime_error("cannot launch configured Python worker");
        }
        close_fd(in[0]); close_fd(out[1]); close_fd(debug[1]);
        state_->pid = pid; state_->to_child = in[1]; state_->from_child = out[0];
        state_->debug_pipe = debug[0];
        for (int fd : {state_->from_child, state_->debug_pipe}) {
            if (fd < 0) continue;
            int flags = ::fcntl(fd, F_GETFL);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK))
                throw std::runtime_error("cannot set nonblocking worker pipe");
        }
        state_->deadline = Clock::now() + state_->warmup_timeout;
    } catch (...) { stop(); throw; }
}

bool Worker::ready() const { return state_->pid > 0 && state_->loaded; }

void Worker::submit(uint64_t id, const std::vector<float>& pcm) {
    auto& s = *state_;
    if (!ready() || s.pending) throw std::logic_error("worker not ready or request already pending");
    if (pcm.size() < 3200 || pcm.size() > 320000) throw std::invalid_argument("clip must be 0.2..20 seconds at 16 kHz");
    for (float value : pcm)
        if (!std::isfinite(value) || value < -1.0f || value > 1.0f)
            throw std::invalid_argument("PCM samples must be finite and in [-1, 1]");
    const std::string path = s.dir + "/clip.raw";
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("cannot create exclusive private clip");
    try {
        // IEEE-754 binary32 encoded explicitly little-endian, independent of host byte order.
        std::array<unsigned char, 4096> bytes{};
        size_t index = 0;
        for (float value : pcm) {
            uint32_t bits; static_assert(sizeof bits == sizeof value);
            std::memcpy(&bits, &value, sizeof bits);
            put32(bytes.data() + index, bits); index += 4;
            if (index == bytes.size()) { write_all(fd, bytes.data(), index); index = 0; }
        }
        if (index) write_all(fd, bytes.data(), index);
        if (::close(fd)) { fd = -1; throw std::runtime_error("clip close failed"); }
        fd = -1;
        unsigned char request[13]{};
        put32(request, 9); request[4] = 'T'; put64(request + 5, id);
        // Pipe is empty (single outstanding request); 13 bytes <= PIPE_BUF.
        write_all(s.to_child, request, sizeof request);
        s.pending = id;
        s.deadline = Clock::now() + s.request_timeout;
    } catch (...) {
        if (fd >= 0) ::close(fd);
        ::unlink(path.c_str());
        stop(); // Any partial pipe write means protocol synchronization is unknown.
        throw;
    }
}

std::optional<WorkerReply> Worker::poll() {
    auto& s = *state_;
    if (s.pid <= 0) return std::nullopt;
    try {
        s.drain_debug();
        if ((!s.loaded || s.pending) && Clock::now() > s.deadline)
            throw std::runtime_error(s.loaded ? "worker transcription timed out" : "worker warmup timed out");
        unsigned char block[4096];
        bool eof = false;
        for (;;) {
            ssize_t n = ::read(s.from_child, block, sizeof block);
            if (n > 0) {
                s.input.insert(s.input.end(), block, block + n);
                if (s.input.size() > max_frame + 4) throw std::runtime_error("worker frame exceeded limit");
                continue;
            }
            if (n == 0) { eof = true; break; }
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) throw std::runtime_error("worker pipe read failed");
            break;
        }
        if (s.input.size() >= 4) {
            auto size = get32(s.input.data());
            if (size > max_frame || size < 1) throw std::runtime_error("invalid worker frame length");
            if (s.input.size() >= 4 + size) {
                if (s.input.size() != 4 + size) throw std::runtime_error("unexpected extra worker frame");
                char type = static_cast<char>(s.input[4]);
                if (type == 'Y' && size == 1 && !s.loaded && !s.pending) {
                    if (eof) throw std::runtime_error("worker exited after warmup");
                    s.loaded = true; s.input.clear(); return std::nullopt;
                }
                if (type == 'F' && !s.loaded) {
                    if (size == 2 && s.input[5] == 'M')
                        throw std::runtime_error("Missing/mismatched pinned local model weights or private clip directory; check --model");
                    if (size == 2 && s.input[5] == 'I')
                        throw std::runtime_error("Authorized Python lacks compatible CPU moondream/torch dependencies; check --python");
                    throw std::runtime_error("Local Redux model failed to load; check authorized CPU runtime and weights");
                }
                if ((type != 'R' && type != 'E') || size < 9 || !s.loaded || !s.pending ||
                    get64(s.input.data() + 5) != *s.pending || size - 9 > max_text)
                    throw std::runtime_error("invalid or stale worker reply");
                WorkerReply reply{*s.pending, {}, {}};
                std::string text(reinterpret_cast<const char*>(s.input.data() + 13), size - 9);
                if (!valid_utf8(text)) throw std::runtime_error("invalid worker UTF-8 reply");
                if (type == 'R') reply.text = std::move(text);
                else reply.error = safe_request_error(text);
                s.pending.reset(); s.input.clear();
                ::unlink((s.dir + "/clip.raw").c_str());
                return reply;
            }
        }
        if (eof) throw std::runtime_error("worker exited or closed its pipe");
        int status{};
        if (::waitpid(s.pid, &status, WNOHANG) == s.pid) {
            s.pid = -1;
            throw std::runtime_error("worker exited unexpectedly");
        }
        return std::nullopt;
    } catch (...) { stop(); throw; }
}
} // namespace frameyap
