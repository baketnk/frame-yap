#include "backend_manager.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <map>
#include <optional>
#include <regex>
#include <spawn.h>
#include <signal.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

namespace frameyap {
namespace {
std::string decode(const std::string& value) {
    std::string output;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%') {
            if (i + 2 >= value.size()) throw std::runtime_error("bad status encoding");
            unsigned n = 0;
            auto [end, error] = std::from_chars(value.data() + i + 1, value.data() + i + 3, n, 16);
            if (error != std::errc{} || end != value.data() + i + 3) throw std::runtime_error("bad status encoding");
            output += char(n); i += 2;
        } else output += value[i];
    }
    if (output.size() > 1100 || std::any_of(output.begin(), output.end(), [](unsigned char c) {
        return c < 32 || c == 127;
    })) throw std::runtime_error("unsafe status text");
    return output;
}
// The installed shell script emits one flat JSON object per line. Read only
// known fields; malformed/unknown records have no UI or success authority.
// Pipe byte and line caps are enforced by poll() before this parser runs.
struct JsonValue { std::string text; bool quoted = false; };
using JsonObject = std::map<std::string, JsonValue>;
bool json_string(const std::string& s, size_t& pos, std::string& out) {
    if (pos >= s.size() || s[pos++] != '"') return false;
    while (pos < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[pos++]);
        if (c == '"') return true;
        if (c < 32) return false;
        if (c != '\\') { out += char(c); continue; }
        if (pos == s.size()) return false;
        c = static_cast<unsigned char>(s[pos++]);
        switch (c) {
        case '"': case '\\': case '/': out += char(c); break;
        case 'b': case 'f': case 'n': case 'r': case 't': out += ' '; break;
        case 'u': {
            if (s.size() - pos < 4) return false;
            unsigned n = 0;
            auto [end, err] = std::from_chars(s.data() + pos, s.data() + pos + 4, n, 16);
            if (err != std::errc{} || end != s.data() + pos + 4) return false;
            pos += 4;
            out += n >= 32 && n < 127 ? char(n) : '?';
            break;
        }
        default: return false;
        }
    }
    return false;
}
std::optional<JsonObject> json_event(const std::string& s) {
    size_t pos = 0;
    auto space = [&] { while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos; };
    space();
    if (pos == s.size() || s[pos++] != '{') return {};
    JsonObject result;
    bool closed = false;
    space();
    if (pos < s.size() && s[pos] == '}') { ++pos; closed = true; }
    else while (pos < s.size() && result.size() < 16) {
        std::string key, value;
        if (!json_string(s, pos, key) || key.size() > 40) return {};
        space();
        if (pos == s.size() || s[pos++] != ':') return {};
        space();
        bool quoted = pos < s.size() && s[pos] == '"';
        if (quoted) { if (!json_string(s, pos, value)) return {}; }
        else {
            size_t start = pos;
            while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ' ' && s[pos] != '\t') ++pos;
            value = s.substr(start, pos - start);
            if (value != "true" && value != "false" && value != "null" &&
                (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))) return {};
        }
        if (!result.emplace(std::move(key), JsonValue{std::move(value), quoted}).second) return {};
        space();
        if (pos < s.size() && s[pos] == '}') { ++pos; closed = true; break; }
        if (pos == s.size() || s[pos++] != ',') return {};
        space();
    }
    space();
    if (!closed || pos != s.size()) return {};
    return result;
}
std::string safe_label(const std::string& s, size_t max) {
    std::string result;
    for (unsigned char c : s) {
        if (result.size() == max) break;
        result += c >= 32 && c < 127 ? char(c) : '?';
    }
    return result;
}
std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= line.size()) {
        size_t end = line.find('\t', start);
        if (end == std::string::npos) end = line.size();
        result.push_back(decode(line.substr(start, end - start)));
        start = end + 1;
    }
    return result;
}
}
BackendManager::BackendManager(std::string python, std::string service, std::string manifest_dir,
                               std::string model_store, std::string installer, std::string legacy_model)
    : python_(std::move(python)), service_(std::move(service)), manifest_(std::move(manifest_dir)),
      store_(std::move(model_store)), installer_(std::move(installer)), legacy_model_(std::move(legacy_model)) {}
std::string BackendManager::model_path(const std::string& id) const {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.id == id; });
    return it == entries_.end() ? store_ + "/" + id : it->model_path;
}
BackendManager::~BackendManager() { cancel(); }
void BackendManager::cancel() noexcept {
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM); // exact owned child only; service execs installer
        int status = 0;
        bool reaped = false;
        // Give the owned installer a short chance to remove its own download
        // temp in a SIGTERM handler; never wait indefinitely on the overlay.
        for (int n = 0; n < 20; ++n) {
            pid_t result = ::waitpid(pid_, &status, WNOHANG);
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
    if (output_ >= 0) { ::close(output_); output_ = -1; }
    buffer_.clear(); installing_ = false; eof_ = false; output_bytes_ = 0;
}
void BackendManager::launch(std::vector<std::string> args, bool installing) {
    if (busy()) throw std::logic_error("backend operation already running");
    int pipe[2];
    if (::pipe2(pipe, O_CLOEXEC)) throw std::runtime_error("backend status pipe unavailable");
    // Pipe ends have independent flags: only the parent's read must be
    // nonblocking. A nonblocking child stdout can lose a burst to EAGAIN.
    if (::fcntl(pipe[0], F_SETFL, O_NONBLOCK) < 0) {
        ::close(pipe[0]); ::close(pipe[1]);
        throw std::runtime_error("backend status pipe unavailable");
    }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    bool actions_initialized = error == 0;
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, pipe[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, pipe[1]);
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t child = -1;
    if (!error) error = posix_spawnp(&child, args[0].c_str(), &actions, nullptr, argv.data(), environ);
    if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
    ::close(pipe[1]);
    if (error) { ::close(pipe[0]); throw std::runtime_error("could not start local backend helper"); }
    pid_ = child; output_ = pipe[0]; installing_ = installing;
    done_ = failed_ = eof_ = false; buffer_.clear(); output_bytes_ = 0;
    if (installing) { install_failed_ = false; install_error_.clear(); }
    deadline_ = std::chrono::steady_clock::now() + (installing ? std::chrono::minutes(30) : std::chrono::minutes(5));
    note_ = installing ? "Downloading pinned model after explicit consent..." :
        install_failed_ ? "Install failed: " +
            (install_error_.empty() ? std::string("installer exited unsuccessfully") : install_error_) +
            "; rechecking local model files..." : "Checking local model files (offline)...";
}
void BackendManager::refresh() {
    if (busy()) return;
    entries_.clear(); checked_ = false;
    std::vector<std::string> args{python_, service_, "--status", "--manifest-dir", manifest_, "--model-store", store_};
    if (!legacy_model_.empty()) args.insert(args.end(), {"--legacy-model", legacy_model_});
    launch(std::move(args), false);
}
void BackendManager::install(const std::string& id, const std::string& consent_sha256) {
    if (busy() || !checked_ || installer_.empty()) throw std::runtime_error("Installer unavailable or model status unchecked");
    auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& e) { return e.id == id; });
    if (found == entries_.end() || found->state == "installed_verified" ||
        found->manifest_sha256 != consent_sha256 || consent_sha256.size() != 64)
        throw std::runtime_error("Selected backend metadata changed; review consent again");
    launch({python_, service_, "--install", "--backend", id, "--installer", installer_,
            "--manifest-dir", manifest_, "--model-store", store_,
            "--expected-manifest-sha256", consent_sha256}, true);
}
void BackendManager::line(const std::string& text) {
    if (installing_) {
        // These are advisory, bounded display events only. Exit zero followed
        // by a fresh offline check is the sole route to installed status.
        auto object = json_event(text);
        if (!object) return;
        auto field = [&](const char* key, bool quoted) -> std::string {
            auto it = object->find(key);
            return it != object->end() && it->second.quoted == quoted ? it->second.text : "";
        };
        if (field("ok", false) == "false") {
            const auto code = field("code", true);
            const auto message = field("message", true);
            if (!message.empty()) {
                install_error_ = safe_label(message, 130);
                if (code == "manifest_mismatch" || code == "usage")
                    install_error_ = code + ": " + install_error_;
                note_ = "Install error: " + install_error_;
                ++revision_;
            }
        } else if (field("ok", false) == "true" && field("event", true) == "model_file") {
            auto state = field("state", true);
            auto file = field("file", true);
            if ((state == "downloading" || state == "verified") && !file.empty()) {
                note_ = (state == "downloading" ? "Downloading: " : "Verified file: ") + safe_label(file, 95);
                ++revision_;
            }
        }
        return;
    }
    if (done_ || failed_) throw std::runtime_error("backend status after terminal record");
    const auto f = fields(text);
    if (f.size() == 1 && f[0] == "DONE") { done_ = true; return; }
    if (f.size() == 2 && f[0] == "ERROR") { failed_ = true; return; }
    if (f.size() != 12 || f[0] != "ST" || f[1].empty() || f[1].size() > 48 ||
        (f[3] != "not_installed" && f[3] != "installed_verified" && f[3] != "invalid") ||
        entries_.size() >= 32) throw std::runtime_error("invalid backend status record");
    uint64_t bytes = 0;
    auto [end, error] = std::from_chars(f[5].data(), f[5].data() + f[5].size(), bytes);
    if (error != std::errc{} || end != f[5].data() + f[5].size() ||
        std::any_of(entries_.begin(), entries_.end(), [&](const auto& e) { return e.id == f[1]; }))
        throw std::runtime_error("invalid backend status metadata");
    static const std::regex id_pattern("[a-z][a-z0-9_-]{0,47}");
    static const std::regex digest_pattern("[0-9a-f]{64}");
    if (f[10].empty() || f[10][0] != '/' || !std::regex_match(f[1], id_pattern) ||
        !std::regex_match(f[11], digest_pattern)) throw std::runtime_error("invalid model location or fingerprint");
    entries_.push_back({f[1], f[2], f[3], f[4], f[6], f[7], f[8], f[9], f[10], bytes, f[11]});
}
void BackendManager::poll() {
    if (!busy()) return;
    try {
        if (std::chrono::steady_clock::now() > deadline_) throw std::runtime_error("backend operation timed out");
        size_t budget = 64 * 1024;
        char chunk[4096];
        while (!eof_ && budget) {
            ssize_t count = ::read(output_, chunk, std::min(budget, sizeof chunk));
            if (count > 0) {
                budget -= size_t(count); output_bytes_ += size_t(count);
                if (output_bytes_ > 256 * 1024) throw std::runtime_error("excessive backend output");
                buffer_.append(chunk, size_t(count));
                for (auto end = buffer_.find('\n'); end != std::string::npos; end = buffer_.find('\n')) {
                    if (end > 8192) throw std::runtime_error("oversized backend status line");
                    line(buffer_.substr(0, end)); buffer_.erase(0, end + 1);
                }
                if (buffer_.size() > 8192) throw std::runtime_error("oversized backend status line");
            } else if (count == 0) { eof_ = true; ::close(output_); output_ = -1; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else throw std::runtime_error("backend status pipe failed");
        }
        if (!eof_) return; // a successful child may still have unread pipe data
        int status = 0;
        pid_t result = ::waitpid(pid_, &status, WNOHANG);
        if (result < 0) throw std::runtime_error("backend helper wait failed");
        if (!result) return;
        pid_ = -1;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || failed_ || !buffer_.empty() ||
            (!installing_ && !done_)) {
            bool was_installing = installing_;
            installing_ = false;
            if (was_installing) {
                // Source metadata may have changed since consent. Recheck before
                // another attempt; never keep stale manifest consent reusable.
                install_failed_ = true;
                checked_ = false;
                refresh();
            } else note_ = install_failed_ ? "Install failed: " +
                        (install_error_.empty() ? std::string("installer exited unsuccessfully") : install_error_) +
                        "; local model recheck failed." : "Local model check failed.";
            ++revision_;
            return;
        }
        if (installing_) { installing_ = false; note_ = "Install finished; verifying local model files..."; refresh(); }
        else { checked_ = true; note_ = install_failed_ ? "Install failed: " +
                        (install_error_.empty() ? std::string("installer exited unsuccessfully") : install_error_) +
                        "; local model status checked offline." : "Local model status checked offline."; ++revision_; }
    } catch (const std::exception& error) {
        bool was_installing = installing_;
        if (was_installing && install_error_.empty()) install_error_ = safe_label(error.what(), 130);
        cancel();
        if (was_installing) checked_ = false;
        if (was_installing) install_failed_ = true;
        note_ = was_installing ? "Install failed: " + install_error_ + ". Restart to recheck models."
                               : install_failed_ ? "Install failed: " + install_error_ + "; local model recheck failed."
                                                 : "Local model check failed.";
        ++revision_;
    }
}
} // namespace frameyap
