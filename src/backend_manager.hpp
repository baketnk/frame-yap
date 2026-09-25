#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

namespace frameyap {
struct BackendEntry {
    std::string id, name, state, reason, source, license, license_text, attribution, model_path;
    uint64_t bytes = 0;
    std::string manifest_sha256;
};
// All hashing/metadata comes from model_files.py via backend-service.py.
// This class never opens the network or blocks waiting for verification/install.
class BackendManager {
public:
    BackendManager(std::string python, std::string service, std::string manifest_dir,
                   std::string model_store, std::string installer, std::string legacy_model = {});
    ~BackendManager();
    BackendManager(const BackendManager&) = delete;
    BackendManager& operator=(const BackendManager&) = delete;
    void refresh();
    void install(const std::string& id, const std::string& consent_sha256); // immutable digest captured from displayed confirmation
    void poll();
    void cancel() noexcept;
    const std::vector<BackendEntry>& entries() const { return entries_; }
    const std::string& note() const { return note_; }
    bool busy() const { return pid_ > 0; }
    bool installing() const { return installing_; }
    size_t revision() const { return revision_; }
    bool checked() const { return checked_; }
    std::string model_path(const std::string& id) const;
    const std::string& manifest_dir() const { return manifest_; }
private:
    void launch(std::vector<std::string> arguments, bool installing);
    void line(const std::string& text);
    std::string python_, service_, manifest_, store_, installer_, legacy_model_;
    std::vector<BackendEntry> entries_;
    std::string note_ = "Checking local models (offline)...";
    std::string buffer_;
    pid_t pid_ = -1;
    int output_ = -1;
    size_t revision_ = 0;
    bool installing_ = false, done_ = false, failed_ = false, checked_ = false;
    bool install_failed_ = false; // retain failed-install feedback through the offline recheck
    std::string install_error_; // sanitized, bounded installer error; not success authority
    bool eof_ = false;
    size_t output_bytes_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
};
}
