#include "runtime.hpp"
#include "cli.hpp"
#include "audio.hpp"
#include "controller.hpp"
#include "backend_manager.hpp"
#include "config.hpp"
#include "overlay.hpp"
#include "text_input.hpp"
#include "worker.hpp"
#include "instance_lock.hpp"
#include "focus_guard.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <csignal>
#include <memory>
#include <stdexcept>
#include <thread>

namespace frameyap {
namespace {
volatile sig_atomic_t interrupted = 0;
void signal_stop(int) { interrupted = 1; }
class NativeDeliveryLease final : public DeliveryLease {
public:
    explicit NativeDeliveryLease(const std::string& socket) : input_(socket) {}
    void text(const std::string& literal) override { input_.text(literal); }
    void enter() override { input_.enter(); }
private:
    TextInput input_;
};
class NativeAudio final : public ControllerAudio {
public:
    bool open() const override { return audio_.open(); }
    void prepare() override { audio_.prepare(); }
    void start() override { audio_.start(); }
    bool poll() override { return audio_.poll(); }
    std::vector<float> finish() override { return audio_.finish(); }
    void cancel() override { audio_.cancel(); }
    void close() override { audio_.close(); }
    int seconds() const override { return audio_.seconds(); }
private:
    Audio audio_;
};
class NativeWorker final : public ControllerWorker {
public:
    explicit NativeWorker(const Options& options) : options_(options) {}
    void configure(std::string backend, std::string model, std::string manifest, std::string root) {
        backend_ = std::move(backend); model_ = std::move(model);
        manifest_ = std::move(manifest); root_ = std::move(root);
    }
    void start(bool advanced_debug) override {
        const std::string script = backend_.empty() ? options_.worker : root_ + "/python/frameyap/backend_worker.py";
        worker_.start(options_.python, script, backend_.empty() ? options_.model : model_, options_.threads,
                      advanced_debug, backend_, manifest_, root_);
    }
    bool ready() const override { return worker_.ready(); }
    void submit(uint64_t id, const std::vector<float>& pcm) override { worker_.submit(id, pcm); }
    std::optional<WorkerReply> poll() override { return worker_.poll(); }
    void stop() override { worker_.stop(); }
private:
    const Options& options_;
    Worker worker_;
    std::string backend_, model_, manifest_, root_;
};
class NativeFocus final : public ControllerFocus {
public:
    bool arm() override { return guard_.arm(); }
    bool valid() override { return guard_.valid(); }
private:
    FocusGuard guard_;
};
} // namespace
int run(const Options& options) {
    InstanceLock lock;
    // Authorized sends retain the IME between per-action leases. Release it on
    // every exit path, after any active delivery stack has unwound; never between
    // text and its explicitly requested Enter. This is not a delivery receipt.
    struct ReleaseInput {
        ~ReleaseInput() { try { TextInput::release_idle(); } catch (...) {} }
    } release_input;
    interrupted = 0;
    auto old_int = std::signal(SIGINT, signal_stop);
    auto old_term = std::signal(SIGTERM, signal_stop);
    struct Restore { decltype(old_int) a, b; ~Restore() { std::signal(SIGINT, a); std::signal(SIGTERM, b); } } restore{old_int, old_term};
    Overlay overlay(options.assets, options.font, options.mount);
    NativeWorker worker(options);
    NativeAudio audio;
    const DeliveryFactory acquire = [&]() -> std::unique_ptr<DeliveryLease> {
        auto input = std::make_unique<NativeDeliveryLease>(options.socket);
        if (interrupted) throw std::runtime_error("Input cancelled before delivery");
        return input;
    };
    PacedDelivery paced(acquire, std::chrono::steady_clock::now,
                        [] { TextInput::release_idle(); }, [] { return !interrupted; });
    Controller controller(audio, worker, acquire, [] { return std::make_unique<NativeFocus>(); },
                          overlay.quick_inputs(), overlay.auto_insert(), overlay.advanced_debug(),
                          overlay.close_mic_when_idle(), &paced);
    namespace fs = std::filesystem;
    const auto root = asset_root(options.assets);
    const auto manifest = options.manifest_dir.empty() ? root / "assets/backends" : fs::path(options.manifest_dir);
    const auto service = root / "scripts/backend-service.py";
    const bool generic = (options.worker.empty() || is_managed_worker(options.assets, options.worker)) &&
                         fs::is_regular_file(root / "python/frameyap/backend_worker.py") &&
                         fs::is_regular_file(service) && fs::is_directory(manifest);
    const char* data = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    const fs::path store = !options.model_store.empty() ? fs::path(options.model_store) :
        data && fs::path(data).is_absolute() ? fs::path(data) / "frameyap/models" :
        home && fs::path(home).is_absolute() ? fs::path(home) / ".local/share/frameyap/models" : root / "models";
    const auto installer = fs::is_regular_file(root / "bin/install.sh") ? root / "bin/install.sh" : root / "install.sh";
    std::unique_ptr<BackendManager> models;
    std::string selected = options.backend.empty() ? load_config(default_config_path()).backend : options.backend;
    size_t revision = 0;
    std::string selection_note;
    if (generic) {
        models = std::make_unique<BackendManager>("python3", service.string(), manifest.string(),
                    store.string(), fs::is_regular_file(installer) ? installer.string() : "", options.model);
        models->refresh();
        controller.backend_changed(false, "Checking installed models offline. Recording unavailable until verified.");
    } else controller.initialize();
    bool previous_debug = overlay.advanced_debug();
    while (!controller.quitting() && !interrupted) {
        if (models) {
            models->poll();
            if (models->revision() != revision) {
                revision = models->revision();
                auto it = std::find_if(models->entries().begin(), models->entries().end(),
                    [&](const auto& entry) { return entry.id == selected; });
                const bool verified = models->checked() && it != models->entries().end() &&
                                      it->state == "installed_verified" && !models->busy();
                if (verified) worker.configure(selected, models->model_path(selected), manifest.string(), root.string());
                controller.backend_changed(verified, it == models->entries().end() ?
                    "Selected backend unavailable. Choose a listed model in Settings." :
                    "Selected model not installed/verified. Select Install in Settings.");
            }
        }
        controller.tick();
        // Drawing precedes polling actions: Enter is disabled until Ready is visible.
        auto panel = controller.panel();
        if (models) {
            panel.selected_backend = selected;
            panel.model_note = selection_note.empty() ? models->note() : selection_note;
            panel.model_busy = models->busy();
            for (const auto& entry : models->entries())
                panel.models.push_back({entry.id, entry.name,
                    entry.id == selected && controller.state() == State::Warming ? "loading" :
                    entry.id == selected && controller.state() == State::Ready ? "ready" :
                    entry.id == selected && controller.state() == State::Error && entry.state == "installed_verified" ? "failed" :
                    entry.state, entry.source, entry.license, entry.license_text, entry.attribution, entry.bytes,
                    entry.state == "installed_verified", entry.manifest_sha256});
        }
        overlay.draw(panel);
        auto actions = overlay.poll();
        const auto model_actions = overlay.take_model_actions();
        // A model-row click revokes the current clip/review even when it targets
        // the selected row, is rejected while busy, or carries stale consent.
        // Do not dispatch the Cancel/EndRecord generated when the overlay resets
        // held PTT: it could turn an unavailable backend back into Ready.
        for (const auto& change : model_actions) {
            if (!models) continue;
            try {
                if (change.install) {
                    if (change.id == selected) {
                        models->install(change.id, change.manifest_sha256);
                        selection_note.clear();
                    }
                } else if (change.id != selected && !models->busy()) {
                    const auto& entries = models->entries();
                    auto it = std::find_if(entries.begin(), entries.end(),
                        [&](const auto& entry) { return entry.id == change.id; });
                    if (it == entries.end()) continue;
                    selected = change.id;
                    selection_note = save_backend(default_config_path(), selected) ? "" :
                        "Backend preference not saved; selection lasts only this session.";
                }
            } catch (const std::exception&) {
                selection_note = "Model metadata changed or installer unavailable. Recheck and confirm again.";
            }
        }
        if (models && !model_actions.empty()) {
            const auto& entries = models->entries();
            auto it = std::find_if(entries.begin(), entries.end(),
                [&](const auto& entry) { return entry.id == selected; });
            const bool verified = models->checked() && !models->busy() &&
                                  it != entries.end() && it->state == "installed_verified";
            if (verified) worker.configure(selected, it->model_path, manifest.string(), root.string());
            controller.backend_changed(verified, verified ? "" :
                "Selected model unavailable or being checked. Recording disabled until offline verification.");
        }
        bool debug_changed = overlay.advanced_debug() != previous_debug;
        controller.settings(overlay.auto_insert(), overlay.advanced_debug(), overlay.close_mic_when_idle());
        // Debug consent and model actions cancel work before any stale input.
        if (debug_changed || !model_actions.empty()) {
            for (auto action : actions) if (action == UiAction::Quit) controller.action(action);
        } else {
            for (auto action : actions) {
                controller.action(action);
                if (controller.quitting() || interrupted) break;
            }
        }
        previous_debug = overlay.advanced_debug();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    controller.shutdown();
    if (models) models->cancel();
    return 0;
}
} // namespace frameyap
