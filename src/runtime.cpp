#include "runtime.hpp"
#include "audio.hpp"
#include "core.hpp"
#include "overlay.hpp"
#include "text_input.hpp"
#include "worker.hpp"
#include "instance_lock.hpp"
#include "focus_guard.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

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
std::string state_label(State state) {
    switch (state) {
    case State::Warming: return "Warming - on-device Redux CPU";
    case State::Ready: return "Ready - hold X or click Record";
    case State::Recording: return "RECORDING";
    case State::Transcribing: return "Transcribing - on device";
    case State::Review: return "Review - Insert approves CURRENT focus";
    case State::Queued: return "Input queued - not a delivery receipt";
    case State::Error: return "Unavailable - Record to retry";
    }
    return {};
}
}
int run(const Options& options) {
    InstanceLock lock;
    interrupted = 0;
    auto old_int = std::signal(SIGINT, signal_stop);
    auto old_term = std::signal(SIGTERM, signal_stop);
    struct Restore { decltype(old_int) a, b; ~Restore() { std::signal(SIGINT, a); std::signal(SIGTERM, b); } } restore{old_int, old_term};
    Overlay overlay(options.assets, options.font, options.mount);
    Worker worker;
    Audio audio;
    Session session;
    std::string detail = "Review mode. Other apps may also hear your mic. Enter is explicit.";
    bool quit = false;
    bool advanced_debug = overlay.advanced_debug();
    bool auto_insert = overlay.auto_insert();
    std::unique_ptr<FocusGuard> armed_focus;
    const DeliveryFactory acquire = [&]() -> std::unique_ptr<DeliveryLease> {
        auto input = std::make_unique<NativeDeliveryLease>(options.socket);
        if (interrupted) throw std::runtime_error("Input cancelled before delivery");
        return input;
    };
    auto delivery_detail = [&](DeliveryResult result, bool submit) {
        switch (result) {
        case DeliveryResult::Ignored: break;
        case DeliveryResult::TextQueued:
            detail = "Text and trailing space queued to current focus. Enter remains explicit."; break;
        case DeliveryResult::EnterQueued:
            detail = submit ? "Explicit Enter queued to current focus; not a delivery receipt."
                            : "Input queued to current focus; not a delivery receipt."; break;
        case DeliveryResult::TextUncertain:
            detail = "Text delivery uncertain; Enter not sent. Not retried; check destination."; break;
        case DeliveryResult::EnterUncertain:
            detail = "Enter delivery uncertain; not retried. Check destination."; break;
        case DeliveryResult::TextQueuedEnterUnavailable:
            detail = "Text and trailing space queued; Enter unavailable and not sent. Check destination."; break;
        }
    };
    auto warm = [&] {
        audio.close(); worker.stop(); session = Session{}; armed_focus.reset();
        worker.start(options.python, options.worker, options.model, options.threads, advanced_debug);
        detail = "Loading local model; microphone closed. Record again when Ready.";
    };
    auto stop_record = [&] {
        if (session.state() != State::Recording) return;
        auto pcm = audio.finish();
        if (session.finish(pcm.size())) {
            worker.submit(session.id(), pcm);
            detail = armed_focus ? "Release complete; checking stable focus before auto insert." :
                                   "Release complete. Review before inserting.";
        } else { armed_focus.reset(); detail = "Short tap discarded (minimum 200ms)."; }
        std::fill(pcm.begin(), pcm.end(), 0.0f);
    };
    auto start_record = [&] {
        if (session.state() == State::Review || session.state() == State::Transcribing || session.state() == State::Warming) return;
        if (!worker.ready()) { warm(); return; }
        if (!audio.open()) audio.prepare(); // recovery only; normal PTT never opens the device
        if (session.state() == State::Error) session.cancel();
        if (!session.record()) return;
        armed_focus.reset();
        if (auto_insert) {
            auto candidate = std::make_unique<FocusGuard>();
            if (candidate->arm()) armed_focus = std::move(candidate);
        }
        audio.start();
        detail = auto_insert && !armed_focus ? "Focus unverified; recording will require manual Insert." :
                 "Release to finish. Cancel discards. Maximum 20 seconds.";
    };
    try { warm(); }
    catch (const std::exception& e) { session.fail(); detail = e.what(); }
    while (!quit && !interrupted) {
        if (armed_focus && !armed_focus->valid()) {
            armed_focus.reset();
            detail = "Focus changed or became uncertain; transcript will require manual Insert.";
        }
        try {
            if (auto reply = worker.poll()) {
                if (reply->id == session.id() && session.state() == State::Transcribing) {
                    if (!reply->error.empty()) {
                        // E is a request-local error. The child still owns its
                        // loaded model and can accept the next utterance.
                        session.fail(); detail = reply->error + "; model ready. Record to retry.";
                        // Worker::poll allows only fixed diagnostic labels here,
                        // never exception messages, audio, paths or recognized text.
                        std::cerr << "FrameYap worker: " << reply->error << '\n';
                    } else {
                        session.reply(reply->id, reply->text);
                        detail = session.text().empty() ? "No speech recognized; try again." :
                                 "Focus your destination, then Insert. Cancel discards.";
                        if (session.state() == State::Review && auto_insert && armed_focus && armed_focus->valid()) {
                            // The exclusive IME lease can take time to acquire.
                            // Recheck *after* acquisition and before consuming the review.
                            const DeliveryFactory guarded = [&]() -> std::unique_ptr<DeliveryLease> {
                                auto lease = acquire();
                                if (!armed_focus || !armed_focus->valid())
                                    throw std::runtime_error("Focus changed during input authorization");
                                return lease;
                            };
                            try {
                                const auto outcome = deliver_insert(session, guarded);
                                delivery_detail(outcome, false);
                                if (outcome == DeliveryResult::TextQueued)
                                    detail = "Auto insert queued text + space to verified focus; never Enter. Not a delivery receipt.";
                            } catch (const std::exception&) {
                                detail = "Auto insert blocked; text kept for manual review. Check focus and Insert.";
                            }
                        }
                        armed_focus.reset();
                    }
                }
            }
            if (worker.ready()) session.ready();
        } catch (const std::exception& e) {
            armed_focus.reset(); audio.close(); worker.stop();
            if (session.state() != State::Review && session.state() != State::Queued) session.fail();
            detail = e.what(); // Preserve an already-correlated preview if the worker dies.
        }
        try {
            // Prepare after model warm-up, not on PTT. Keep draining/discarding
            // idle samples so neither a device transition nor old speech reaches
            // the next clip. A capture failure still requires an explicit retry.
            if (session.state() == State::Ready && !audio.open() && worker.ready()) audio.prepare();
            if (audio.open() && audio.poll()) stop_record();
        } catch (const std::exception& e) {
            // A microphone failure is not a model failure.
            audio.close(); session.fail(); detail = e.what();
            armed_focus.reset();
        }
        // Drawing precedes input polling: Enter is disabled until Ready is visible.
        const auto status = session.state() == State::Error && worker.ready()
            ? "Retry available - local model still loaded" : state_label(session.state());
        Panel panel{status, session.text(), detail,
                    session.state() != State::Error && session.state() != State::Warming && session.state() != State::Transcribing,
                    session.state() == State::Recording,
                    session.state() != State::Warming && session.state() != State::Transcribing && session.state() != State::Review};
        if (panel.recording) panel.status += " - " + std::to_string(audio.seconds()) + " / 20s";
        overlay.draw(panel);
        auto actions = overlay.poll();
        if (auto_insert != overlay.auto_insert()) {
            auto_insert = overlay.auto_insert();
            armed_focus.reset(); // a toggle never retroactively authorizes a capture
            detail = auto_insert ? "Auto insert enabled for future clips only when X focus stays verified. Never Enter." :
                                   "Auto insert disabled; review before Insert.";
        }
        if (advanced_debug != overlay.advanced_debug()) {
            advanced_debug = overlay.advanced_debug();
            // Consent changes take effect before any more work or delivery. The
            // Settings warning makes the worker restart/cancellation explicit.
            try { warm(); }
            catch (const std::exception& e) { session.fail(); detail = e.what(); }
            std::erase_if(actions, [](UiAction action) { return action != UiAction::Quit; });
        }
        for (auto action : actions) {
            try {
                switch (action) {
                case UiAction::Quit: quit = true; break;
                case UiAction::Toggle:
                case UiAction::Record:
                    if (session.state() == State::Recording) stop_record(); else start_record();
                    break;
                case UiAction::BeginRecord: start_record(); break;
                case UiAction::EndRecord: stop_record(); break;
                case UiAction::Cancel: {
                    auto state = session.state();
                    armed_focus.reset(); audio.cancel(); session.cancel(); detail = "Discarded. Idle microphone samples are discarded.";
                    if (state == State::Warming || state == State::Transcribing) {
                        audio.close(); worker.stop(); session.fail(); detail = "Cancelled. Record to reload local worker.";
                    } else if (state == State::Error && !worker.ready()) {
                        audio.close();
                        session.fail(); detail = "Worker unavailable. Record to reload local worker.";
                    }
                    break;
                }
                case UiAction::Insert:
                    delivery_detail(deliver_insert(session, acquire), false);
                    break;
                case UiAction::Enter:
                    delivery_detail(deliver_enter(session, acquire), true);
                    break;
                }
            } catch (const std::exception& e) {
                // Input lease failures preserve preview. Capture failures close
                // only the microphone; submit failures stop their own child if
                // the IPC stream was partially written.
                if (session.state() == State::Recording || session.state() == State::Transcribing) {
                    audio.close(); session.fail();
                }
                detail = e.what();
            }
            if (quit || interrupted) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    audio.close(); session.cancel(); worker.stop();
    return 0;
}
}
