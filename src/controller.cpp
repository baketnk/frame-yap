#include "controller.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace frameyap {
namespace {
std::string state_label(State state) {
    switch (state) {
    case State::Warming: return "Warming - on-device model";
    case State::Ready: return "Ready - hold X or click Record";
    case State::Recording: return "RECORDING";
    case State::Transcribing: return "Transcribing - on device";
    case State::Review: return "Review - Type approves CURRENT focus";
    case State::Queued: return "Input queued - not a delivery receipt";
    case State::Error: return "Unavailable - Record to retry";
    }
    return {};
}
} // namespace

Controller::Controller(ControllerAudio& audio, ControllerWorker& worker,
                       DeliveryFactory delivery, FocusFactory focus,
                       std::vector<std::string> quick_inputs, bool auto_insert,
                       bool advanced_debug, bool close_mic_when_idle, PacedDelivery* paced)
    : audio_(audio), worker_(worker), delivery_(std::move(delivery)), paced_(paced),
      focus_factory_(std::move(focus)), quick_inputs_(std::move(quick_inputs)),
      auto_insert_(auto_insert), advanced_debug_(advanced_debug),
      close_mic_when_idle_(close_mic_when_idle) {}

void Controller::warm() {
    if (paced_) paced_->cancel();
    status_note_.clear(); quick_open_ = false;
    armed_focus_.reset(); audio_.close(); worker_.stop(); session_ = Session{};
    worker_.start(advanced_debug_);
    detail_ = "Loading local model; microphone closed. Record again when Ready.";
}
void Controller::initialize() {
    try { warm(); }
    catch (const std::exception& e) { session_.fail(); detail_ = e.what(); }
}
void Controller::backend_changed(bool verified, std::string detail) {
    const bool partial = paced_ && paced_->began();
    if (paced_) paced_->cancel();
    armed_focus_.reset(); audio_.close(); worker_.stop(); session_ = Session{};
    quick_open_ = false; status_note_.clear();
    backend_available_ = verified;
    if (!verified) {
        session_.fail(); detail_ = detail.empty() ? "Model not installed and verified. Choose Install in Settings." : std::move(detail);
    } else {
        try { warm(); } catch (const std::exception& e) { session_.fail(); detail_ = e.what(); }
    }
    if (partial) {
        status_note_ = "Input stopped - partial delivery uncertain";
        detail_ += " Earlier input may have queued; remainder and Enter dropped.";
    }
}
void Controller::shutdown() {
    if (paced_) paced_->cancel();
    armed_focus_.reset(); audio_.close(); session_.cancel(); worker_.stop();
}
void Controller::request_failed(const std::string& error) {
    armed_focus_.reset(); session_.fail();
    // Neither raw worker output nor malformed transcript is printed here.
    detail_ = error + "; model ready. Record to retry.";
    std::cerr << "FrameYap worker: " << error << '\n';
}
void Controller::delivery_detail(DeliveryResult result, bool submit) {
    switch (result) {
    case DeliveryResult::Ignored: break;
    case DeliveryResult::TextQueued:
        detail_ = "Text queued to current focus (trailing space if it fits). Enter remains explicit.";
        status_note_ = "Text queued - Enter not sent"; break;
    case DeliveryResult::EnterQueued:
        detail_ = submit ? "Explicit Enter queued to current focus; not a delivery receipt."
                         : "Input queued to current focus; not a delivery receipt.";
        status_note_ = "Enter queued - check destination"; break;
    case DeliveryResult::TextUncertain:
        detail_ = "Text delivery uncertain; Enter not sent. Not retried; check destination.";
        status_note_ = "Text uncertain - Enter not sent"; break;
    case DeliveryResult::EnterUncertain:
        detail_ = "Enter delivery uncertain; not retried. Check destination.";
        status_note_ = "Enter uncertain - check destination"; break;
    case DeliveryResult::TextQueuedEnterUnavailable:
        detail_ = "Text queued; Enter unavailable and not sent. Check destination.";
        status_note_ = "Text queued - Enter unavailable"; break;
    }
}
std::shared_ptr<ControllerFocus> Controller::manual_focus() {
    auto focus = focus_factory_();
    if (!focus || !focus->arm()) throw std::runtime_error("Current target not verified; review kept");
    return std::shared_ptr<ControllerFocus>(std::move(focus));
}
void Controller::queue_paced(std::string text, bool enter, bool review,
                             std::shared_ptr<ControllerFocus> focus, bool automatic) {
    auto guard = [focus = std::move(focus)] { return focus->valid(); };
    paced_->start(std::move(text), enter, std::move(guard), [this, review] {
        if (review) {
            auto consumed = session_.take_insert();
            if (!consumed) throw std::runtime_error("Review was revoked");
            std::fill(consumed->begin(), consumed->end(), '\0');
        }
    });
    paced_auto_ = automatic;
    paced_submit_ = enter;
    detail_ = "Input paced to captured focus; check destination. Cancel stops remaining input.";
    status_note_ = "Input pacing - not a delivery receipt";
    quick_open_ = false;
}
void Controller::stop_record() {
    if (session_.state() != State::Recording) return;
    auto pcm = audio_.finish();
    try {
        if (session_.finish(pcm.size())) {
            worker_.submit(session_.id(), pcm);
            detail_ = armed_focus_ ? "Release complete; checking stable focus before auto insert." :
                                     "Release complete. Review before typing.";
        } else { armed_focus_.reset(); detail_ = "Short tap discarded (minimum 200ms)."; }
    } catch (...) {
        std::fill(pcm.begin(), pcm.end(), 0.0f);
        throw;
    }
    std::fill(pcm.begin(), pcm.end(), 0.0f);
    if (close_mic_when_idle_) audio_.close();
}
void Controller::start_record() {
    if ((paced_ && paced_->active()) || session_.state() == State::Review ||
        session_.state() == State::Transcribing || session_.state() == State::Warming) return;
    if (!backend_available_) return;
    if (!worker_.ready()) { warm(); return; }
    if (!audio_.open()) audio_.prepare(); // only on PTT when close-when-idle is enabled
    if (session_.state() == State::Error) session_.cancel();
    if (!session_.record()) return;
    status_note_.clear(); armed_focus_.reset();
    if (auto_insert_) {
        auto candidate = focus_factory_();
        if (candidate && candidate->arm()) armed_focus_ = std::move(candidate);
    }
    audio_.start();
    detail_ = auto_insert_ && !armed_focus_ ? "Focus unverified; recording will require manual Type." :
              "Release to finish. Cancel discards. Maximum 20 seconds.";
}
void Controller::tick() {
    if (armed_focus_) {
        bool valid = false;
        try { valid = armed_focus_->valid(); } catch (const std::exception&) {}
        if (!valid) {
            armed_focus_.reset();
            detail_ = "Focus changed or became uncertain; transcript will require manual Type.";
        }
    }
    try {
        if (auto reply = worker_.poll()) {
            if (reply->id == session_.id() && session_.state() == State::Transcribing) {
                if (!reply->error.empty()) {
                    // Correlated E replies are request-level failures, not model failures.
                    request_failed("transcription failed");
                } else {
                    try { session_.reply(reply->id, reply->text); }
                    catch (const std::exception&) {
                        // Malformed controls/UTF-8 are also request-level failures.
                        // The worker has already consumed this reply and stays loaded.
                        request_failed("transcription failed");
                    }
                    if (session_.state() != State::Error) {
                        detail_ = session_.text().empty() ? "No speech recognized; try again." :
                                   "Focus your destination, then Type. Cancel discards.";
                        if (session_.state() == State::Review && auto_insert_ && armed_focus_) {
                            try {
                                if (paced_) {
                                    auto text = session_.text();
                                    if (text.back() != ' ' && text.size() < 4096) text += ' ';
                                    queue_paced(std::move(text), false, true,
                                        std::shared_ptr<ControllerFocus>(std::move(armed_focus_)), true);
                                } else {
                                    // Fake synchronous path retains its existing single-send contract.
                                    const DeliveryFactory guarded = [&]() -> std::unique_ptr<DeliveryLease> {
                                        auto lease = delivery_();
                                        if (!armed_focus_ || !armed_focus_->valid())
                                            throw std::runtime_error("Focus changed during input authorization");
                                        return lease;
                                    };
                                    const auto outcome = deliver_insert(session_, guarded);
                                    delivery_detail(outcome, false);
                                    if (outcome == DeliveryResult::TextQueued)
                                        detail_ = "Auto insert queued text to verified focus (space if it fits); never Enter. Not a delivery receipt.";
                                }
                            } catch (const std::exception&) {
                                detail_ = "Auto insert blocked; text kept for manual review. Check focus and Type.";
                            }
                        }
                        armed_focus_.reset();
                    }
                }
            }
        }
        if (worker_.ready()) session_.ready();
    } catch (const std::exception& e) {
        const bool partial = paced_ && paced_->began();
        if (paced_) paced_->cancel();
        armed_focus_.reset(); audio_.close(); worker_.stop();
        if (session_.state() != State::Review && session_.state() != State::Queued) session_.fail();
        detail_ = e.what(); // Preserve an already-correlated preview on worker failure.
        if (partial) {
            status_note_ = "Input stopped - partial delivery uncertain";
            detail_ += "; earlier input may have queued; remainder and Enter dropped.";
        }
    }
    if (paced_) {
        if (auto outcome = paced_->tick()) {
            if (!outcome->began) {
                detail_ = outcome->focus_lost ? "Focus changed before input; review kept for a new manual Type." :
                    "Input unavailable before delivery; review kept if pending.";
                status_note_ = "Input blocked - check destination";
            } else {
                delivery_detail(outcome->result, paced_submit_);
                if (outcome->focus_lost)
                    detail_ = "Focus changed during pacing; remaining input and Enter dropped. Earlier input may have queued.";
                else if (paced_auto_ && outcome->result == DeliveryResult::TextQueued)
                    detail_ = "Auto insert queued text to continuously verified focus; never Enter. Not a delivery receipt.";
            }
            paced_auto_ = paced_submit_ = false;
        }
    }
    try {
        // Default: keep the device open and drain idle samples. Optional privacy
        // policy: open at PTT, close immediately when a clip is complete.
        if (!close_mic_when_idle_ && session_.state() == State::Ready &&
            !audio_.open() && worker_.ready()) audio_.prepare();
        if (audio_.open() && audio_.poll()) stop_record();
    } catch (const std::exception& e) {
        const bool partial = paced_ && paced_->began();
        if (paced_) paced_->cancel();
        audio_.close(); session_.fail(); detail_ = e.what(); armed_focus_.reset();
        if (partial) {
            status_note_ = "Input stopped - partial delivery uncertain";
            detail_ += "; earlier input may have queued; remainder and Enter dropped.";
        }
    }
    if (quick_open_ && (session_.state() == State::Recording || session_.state() == State::Transcribing ||
                        session_.state() == State::Warming || session_.state() == State::Error))
        quick_open_ = false;
}
Panel Controller::panel() const {
    auto status = session_.state() == State::Error && worker_.ready()
        ? "Retry available - local model still loaded" : state_label(session_.state());
    Panel result{quick_open_ ? "Quick phrases" : status_note_.empty() ? status : status_note_,
                 session_.text(), detail_,
                 !(paced_ && paced_->active()) && session_.state() != State::Error && session_.state() != State::Warming && session_.state() != State::Transcribing,
                 session_.state() == State::Recording,
                 !(paced_ && paced_->active()) && backend_available_ && session_.state() != State::Warming && session_.state() != State::Transcribing && session_.state() != State::Review};
    result.quick_open = quick_open_;
    result.quick_selected = quick_selected_;
    result.quick_inputs = quick_inputs_;
    if (result.recording) result.status += " - " + std::to_string(audio_.seconds()) + " / 20s";
    return result;
}
void Controller::settings(bool auto_insert, bool advanced_debug, bool close_mic_when_idle) {
    const bool delivery_setting_changed = auto_insert_ != auto_insert || advanced_debug_ != advanced_debug;
    const bool was_partial = delivery_setting_changed && paced_ && paced_->began();
    if (auto_insert_ != auto_insert) {
        const bool partial = paced_ && paced_->began();
        if (paced_) paced_->cancel();
        auto_insert_ = auto_insert;
        armed_focus_.reset(); // never retroactively authorize a clip
        detail_ = auto_insert ? "Auto insert enabled for future clips only when X focus stays verified. Never Enter." :
                                "Auto insert disabled; review before Type.";
        if (partial) {
            status_note_ = "Input stopped - partial delivery uncertain";
            detail_ += " Earlier input may have queued; remainder and Enter dropped.";
        }
    }
    if (close_mic_when_idle_ != close_mic_when_idle) {
        close_mic_when_idle_ = close_mic_when_idle;
        if (close_mic_when_idle_ && session_.state() != State::Recording) audio_.close();
        detail_ = close_mic_when_idle_ ? "Mic closes between clips. Opening on PTT may delay or clip speech." :
                                        "Mic stays open while idle; samples are discarded. Other apps may still hear you.";
    }
    if (advanced_debug_ != advanced_debug) {
        const bool partial = paced_ && paced_->began();
        advanced_debug_ = advanced_debug;
        if (backend_available_) {
            try { warm(); }
            catch (const std::exception& e) { session_.fail(); detail_ = e.what(); }
        } else { if (paced_) paced_->cancel(); armed_focus_.reset(); audio_.close(); worker_.stop(); session_.fail(); }
        if (partial) {
            status_note_ = "Input stopped - partial delivery uncertain";
            detail_ += " Earlier input may have queued; remainder and Enter dropped.";
        }
    }
    if (was_partial) {
        status_note_ = "Input stopped - partial delivery uncertain";
        if (detail_.find("Earlier input may have queued") == std::string::npos)
            detail_ += " Earlier input may have queued; remainder and Enter dropped.";
    }
}
void Controller::action(UiAction action) {
    if (action == UiAction::Quit) { if (paced_) paced_->cancel(); quit_ = true; return; }
    if (paced_ && paced_->active() && action != UiAction::Cancel) return;
    try {
        switch (action) {
        case UiAction::Toggle:
        case UiAction::Record:
            if (quick_open_) break;
            if (session_.state() == State::Recording) stop_record(); else start_record();
            break;
        case UiAction::BeginRecord: if (!quick_open_) start_record(); break;
        case UiAction::EndRecord: stop_record(); break;
        case UiAction::Cancel: {
            if (quick_open_) { quick_open_ = false; break; }
            status_note_.clear();
            const bool partial = paced_ && paced_->began();
            if (paced_) paced_->cancel();
            auto state = session_.state();
            armed_focus_.reset(); audio_.cancel(); session_.cancel();
            detail_ = close_mic_when_idle_ ? "Discarded. Microphone closed between clips." :
                                             "Discarded. Idle microphone samples are discarded.";
            if (close_mic_when_idle_) audio_.close();
            if (state == State::Warming || state == State::Transcribing) {
                audio_.close(); worker_.stop(); session_.fail(); detail_ = "Cancelled. Record to reload local worker.";
            } else if (state == State::Error && !worker_.ready()) {
                audio_.close(); session_.fail(); detail_ = "Worker unavailable. Record to reload local worker.";
            }
            if (partial) {
                status_note_ = "Input stopped - partial delivery uncertain";
                detail_ = "Earlier input may have queued; remainder and Enter dropped. Check destination.";
            }
            break;
        }
        case UiAction::Insert:
            if (!quick_open_ && session_.state() == State::Review) {
                if (paced_) {
                    auto text = session_.text();
                    if (text.back() != ' ' && text.size() < 4096) text += ' ';
                    queue_paced(std::move(text), false, true, manual_focus());
                } else delivery_detail(deliver_insert(session_, delivery_), false);
            } else if (!quick_open_ && (session_.state() == State::Ready || session_.state() == State::Queued)) {
                // A deliberate Type press with nothing to review is an explicit Enter.
                if (paced_) queue_paced({}, true, false, manual_focus());
                else delivery_detail(deliver_enter(session_, delivery_), true);
            }
            break;
        case UiAction::Enter:
            if (quick_open_) {
                quick_open_ = false; // explicit quick selection, even on unavailable input
                if (paced_) {
                    const auto& quick = quick_inputs_.at(quick_selected_);
                    if (quick.empty() || quick.size() > 64 || literal_text(quick) != quick)
                        throw std::runtime_error("Invalid quick input");
                    queue_paced(quick, true, false, manual_focus());
                } else delivery_detail(deliver_quick(quick_inputs_.at(quick_selected_), delivery_), true);
            } else if (paced_) {
                if (session_.state() == State::Review) {
                    auto text = session_.text();
                    if (text.back() != ' ' && text.size() < 4096) text += ' ';
                    queue_paced(std::move(text), true, true, manual_focus());
                } else if (session_.state() == State::Ready || session_.state() == State::Queued)
                    queue_paced({}, true, false, manual_focus());
            } else delivery_detail(deliver_enter(session_, delivery_), true);
            break;
        case UiAction::QuickChat:
            if (panel().enabled && session_.state() != State::Recording && !quick_inputs_.empty()) {
                if (quick_open_) quick_selected_ = (quick_selected_ + 1) % quick_inputs_.size();
                else { quick_selected_ = 0; quick_open_ = true; }
            }
            break;
        case UiAction::Quit: break;
        }
    } catch (const std::exception& e) {
        if (session_.state() == State::Recording || session_.state() == State::Transcribing) {
            audio_.close(); session_.fail(); armed_focus_.reset(); status_note_.clear();
        } else status_note_ = "Input unavailable - check destination";
        detail_ = e.what();
    }
}
} // namespace frameyap
