#include "controller.hpp"
#include <cassert>
#include <chrono>
#include <deque>
#include <stdexcept>
#include <string>

using namespace frameyap;
namespace {
struct AudioFake final : ControllerAudio {
    bool opened = false, timeout = false;
    int prepares = 0, closes = 0, starts = 0, polls = 0, finishes = 0;
    size_t samples = 3200;
    bool open() const override { return opened; }
    void prepare() override { opened = true; ++prepares; }
    void start() override { assert(opened); ++starts; }
    bool poll() override { ++polls; return timeout; }
    std::vector<float> finish() override { ++finishes; timeout = false; return std::vector<float>(samples); }
    void cancel() override {}
    void close() override { if (opened) ++closes; opened = false; }
    int seconds() const override { return 0; }
};
struct WorkerFake final : ControllerWorker {
    bool loaded = false;
    int starts = 0, stops = 0, submissions = 0;
    bool debug = false;
    uint64_t request = 0;
    std::deque<WorkerReply> replies;
    void start(bool advanced) override { loaded = true; ++starts; debug = advanced; }
    bool ready() const override { return loaded; }
    void submit(uint64_t id, const std::vector<float>& pcm) override {
        assert(loaded && pcm.size() == 3200); request = id; ++submissions;
    }
    std::optional<WorkerReply> poll() override {
        if (replies.empty()) return std::nullopt;
        auto result = replies.front(); replies.pop_front(); return result;
    }
    void stop() override { loaded = false; ++stops; replies.clear(); }
};
struct FocusState { bool armed = true, valid = true; int checks = 0, destroyed = 0; };
struct FocusFake final : ControllerFocus {
    explicit FocusFake(FocusState& state) : state(state) {}
    ~FocusFake() override { ++state.destroyed; }
    bool arm() override { return state.armed; }
    bool valid() override { ++state.checks; return state.valid; }
    FocusState& state;
};
struct InputState {
    int text = 0, enter = 0, acquisitions = 0;
    bool unavailable = false, fail_text = false, lose_focus_on_acquire = false;
    std::string sent;
    std::vector<std::string> events;
};
struct Lease final : DeliveryLease {
    explicit Lease(InputState& state) : state(state) {}
    void text(const std::string& literal) override {
        ++state.text; state.sent = literal; state.events.push_back(literal);
        if (state.fail_text) throw std::runtime_error("ambiguous text send");
    }
    void enter() override { ++state.enter; state.events.push_back("<Enter>"); }
    InputState& state;
};
struct Fixture {
    AudioFake audio;
    WorkerFake worker;
    FocusState focus;
    InputState input;
    DeliveryFactory acquire() {
        return [this]() -> std::unique_ptr<DeliveryLease> {
            ++input.acquisitions;
            if (input.lose_focus_on_acquire) focus.valid = false;
            if (input.unavailable) throw std::runtime_error("input unavailable");
            return std::make_unique<Lease>(input);
        };
    }
    Controller make(bool automatic = false, bool close_mic = false, PacedDelivery* paced = nullptr) {
        return Controller(audio, worker, acquire(),
            [this]() -> std::unique_ptr<ControllerFocus> { return std::make_unique<FocusFake>(focus); },
            {"Hi", "Next"}, automatic, false, close_mic, paced);
    }
};
void ready(Controller& c) { c.initialize(); c.tick(); assert(c.state() == State::Ready); }
void release(Controller& c, Fixture& f) {
    c.action(UiAction::BeginRecord);
    assert(c.state() == State::Recording);
    c.action(UiAction::EndRecord);
    assert(c.state() == State::Transcribing && f.worker.submissions > 0);
}
void result(Controller& c, Fixture& f, std::string text) {
    f.worker.replies.push_back({f.worker.request, std::move(text), {}});
    c.tick();
}
}
int main() {
    {
        Fixture f; auto c = f.make(false); ready(c);
        release(c, f); result(c, f, "pending preview");
        assert(c.state() == State::Review);
        c.backend_changed(false, "Model not installed");
        assert(c.state() == State::Error && !c.panel().record_available);
        assert(c.panel().transcript.empty() && !f.worker.loaded && !f.audio.opened);
        int previous_starts = f.worker.starts;
        c.action(UiAction::BeginRecord);
        assert(f.worker.starts == previous_starts && f.input.text == 0);
        c.action(UiAction::Insert); assert(f.input.text == 0);
        c.backend_changed(true); c.tick();
        assert(c.state() == State::Ready && f.worker.starts == previous_starts + 1);
        release(c, f);
        c.backend_changed(false);
        f.worker.replies.push_back({f.worker.request, "stale", {}});
        c.tick(); assert(c.panel().transcript.empty() && f.input.text == 0);
        c.shutdown();
    }
    {
        Fixture f; auto c = f.make(true); ready(c);
        c.action(UiAction::BeginRecord); // held PTT, auto-focus armed
        assert(c.state() == State::Recording && f.focus.destroyed == 0);
        c.backend_changed(true); // selected row clicked again: restart, never retain held clip
        assert(c.state() == State::Warming && !f.audio.opened && f.focus.destroyed == 1);
        int submitted = f.worker.submissions;
        c.action(UiAction::EndRecord); // old PTT release cannot submit after restart
        c.tick(); assert(c.state() == State::Ready && f.worker.submissions == submitted);
        c.action(UiAction::Insert); // nothing to review: Type is an explicit Enter
        assert(f.input.text == 0 && f.input.enter == 1);
        release(c, f);
        f.focus.valid = false; result(c, f, "review to discard");
        assert(c.state() == State::Review && f.input.text == 0);
        c.backend_changed(false, "Busy or missing selected model");
        assert(c.state() == State::Error && !c.panel().record_available && c.panel().transcript.empty());
        int starts = f.worker.starts;
        c.action(UiAction::Cancel); // a stale Cancel cannot re-enable an unavailable model
        c.tick(); c.action(UiAction::BeginRecord); c.action(UiAction::Enter);
        assert(c.state() == State::Error && f.worker.starts == starts &&
               f.input.enter == 1 && f.input.text == 0);
        c.shutdown();
    }
    {
        Fixture f; auto c = f.make(); ready(c);
        release(c, f); result(c, f, "review to discard");
        assert(c.state() == State::Review);
        c.backend_changed(true); // current selected row: revoke review and restart
        assert(c.state() == State::Warming && c.panel().transcript.empty());
        c.action(UiAction::Insert); c.action(UiAction::EndRecord);
        assert(f.input.text == 0 && f.worker.submissions == 1);
        c.shutdown();
    }
    {
        Fixture f; auto c = f.make(); ready(c);
        assert(f.audio.prepares == 1 && f.audio.opened); // default idle drain
        c.tick(); assert(f.audio.polls >= 2);
        release(c, f);
        c.action(UiAction::Enter); // Never submit while transcribing.
        assert(f.input.enter == 0 && f.input.text == 0);
        // Malformed controls and bad UTF-8 are request failures, not worker failures.
        result(c, f, std::string("bad\x1b[31m"));
        assert(c.state() == State::Error && f.worker.loaded && f.worker.starts == 1);
        assert(c.panel().detail.find("transcription failed") != std::string::npos);
        assert(c.panel().transcript.empty() && f.input.text == 0);
        release(c, f);
        result(c, f, std::string("bad\xc3\x28"));
        assert(c.state() == State::Error && f.worker.loaded && f.worker.starts == 1);
        release(c, f);
        f.worker.replies.push_back({f.worker.request, {}, "transcription failed"});
        c.tick();
        assert(c.state() == State::Error && f.worker.loaded && f.worker.starts == 1);
        release(c, f); result(c, f, "hello");
        assert(c.state() == State::Review && c.panel().transcript == "hello");
        assert(f.input.text == 0 && f.input.enter == 0);
        c.action(UiAction::Insert);
        assert(f.input.text == 1 && f.input.sent == "hello " && f.input.enter == 0);
        c.action(UiAction::Insert); // at most once; a second press is only an Enter
        assert(f.input.text == 1 && f.input.enter == 1);
        release(c, f); result(c, f, std::string(4096, 'x'));
        c.action(UiAction::Insert);
        assert(f.input.sent.size() == 4096 && f.input.sent.back() == 'x');
        assert(c.panel().detail.find("trailing space if it fits") != std::string::npos);
        c.shutdown();
    }
    {
        Fixture f; auto c = f.make(); ready(c);
        c.action(UiAction::QuickChat);
        assert(c.panel().quick_open && c.panel().quick_selected == 0);
        c.action(UiAction::Record); assert(c.state() == State::Ready);
        c.action(UiAction::QuickChat); assert(c.panel().quick_selected == 1);
        c.action(UiAction::Cancel); assert(!c.panel().quick_open && f.input.text == 0);
        c.action(UiAction::QuickChat); c.action(UiAction::Enter);
        assert(f.input.text == 1 && f.input.sent == "Hi" && f.input.enter == 1);
        assert(!c.panel().quick_open);
        c.action(UiAction::Cancel); // close quick chat never dispatches a stale Enter
        assert(f.input.enter == 1);
        c.action(UiAction::QuickChat); f.input.unavailable = true;
        c.action(UiAction::Enter);
        assert(!c.panel().quick_open && f.input.enter == 1);
        c.action(UiAction::Enter); // explicit new Enter alone, still unavailable
        assert(f.input.enter == 1);
        c.shutdown();
    }
    {
        Fixture f; auto c = f.make(true); ready(c);
        release(c, f); result(c, f, "auto");
        assert(c.state() == State::Queued && f.input.sent == "auto " && f.input.enter == 0);
        assert(c.panel().detail.find("never Enter") != std::string::npos);
        c.tick(); assert(f.input.text == 1); // duplicate delivery prohibited
        release(c, f);
        f.focus.valid = false;
        result(c, f, "review");
        assert(c.state() == State::Review && f.input.text == 1);
        c.action(UiAction::Insert);
        assert(f.input.sent == "review " && f.input.text == 2 && f.input.enter == 0);
        c.shutdown();
    }
    {
        Fixture f; auto c = f.make(true); ready(c);
        release(c, f);
        f.input.lose_focus_on_acquire = true;
        result(c, f, "focus changed during lease");
        assert(c.state() == State::Review && f.input.text == 0);
        f.input.lose_focus_on_acquire = false;
        c.action(UiAction::Insert);
        assert(f.input.text == 1 && f.input.enter == 0);
        c.shutdown();
    }
    {
        Fixture f; auto c = f.make(true); ready(c);
        release(c, f);
        f.input.fail_text = true;
        result(c, f, "ambiguous");
        assert(c.state() == State::Queued && f.input.text == 1 && f.input.enter == 0);
        c.action(UiAction::Insert);
        assert(f.input.text == 1); // uncertain delivery cannot be retried
        c.shutdown();
    }
    {
        Fixture f; auto c = f.make(true); ready(c);
        release(c, f);
        f.input.unavailable = true; result(c, f, "keep");
        assert(c.state() == State::Review && f.input.text == 0);
        f.input.unavailable = false;
        c.action(UiAction::Insert);
        assert(f.input.sent == "keep " && f.input.enter == 0);
        c.settings(false, false, false);
        release(c, f);
        c.action(UiAction::Cancel); // cancel outstanding inference; do not deliver late replies
        assert(c.state() == State::Error && !f.worker.loaded);
        f.worker.replies.push_back({f.worker.request, "stale", {}});
        c.tick(); assert(c.panel().transcript.empty() && f.input.text == 1);
        c.action(UiAction::Record); c.tick(); assert(c.state() == State::Ready);
        f.audio.samples = 3199;
        c.action(UiAction::Record); c.action(UiAction::EndRecord);
        assert(c.state() == State::Ready && f.worker.submissions == 2);
        c.shutdown();
    }
    {
        Fixture f; auto c = f.make(false); ready(c);
        c.action(UiAction::Record);
        c.settings(true, false, true); // neither setting retroactively arms focus
        c.action(UiAction::EndRecord);
        assert(!f.audio.opened);
        result(c, f, "manual");
        assert(c.state() == State::Review && f.input.text == 0 && f.input.enter == 0);
        c.shutdown();
    }
    {
        Fixture f; auto c = f.make(false, true); ready(c);
        assert(!f.audio.opened && f.audio.prepares == 0);
        release(c, f);
        assert(f.audio.prepares == 1 && !f.audio.opened && f.audio.closes == 1);
        result(c, f, "text"); assert(!f.audio.opened && c.state() == State::Review);
        c.action(UiAction::Insert);
        c.action(UiAction::Record); assert(f.audio.opened);
        c.action(UiAction::Cancel); assert(!f.audio.opened);
        c.settings(false, false, false); c.tick();
        assert(f.audio.opened); // default resumes idle drain on next Ready tick
        c.settings(false, true, true);
        assert(f.worker.debug && f.worker.starts == 2 && !f.audio.opened);
        c.tick(); assert(c.state() == State::Ready && !f.audio.opened);
        c.shutdown();
    }
    // Native-style scheduler: authorization is captured at action start, not
    // reacquired at the end of an asynchronous multi-commit task.
    {
        Fixture f;
        auto now = std::chrono::steady_clock::time_point{};
        PacedDelivery paced(f.acquire(), [&] { return now; });
        auto c = f.make(false, false, &paced); ready(c);
        release(c, f); result(c, f, std::string(81, 'x'));
        c.action(UiAction::Enter);
        assert(c.state() == State::Review && f.input.events.empty() && !c.panel().record_available);
        c.action(UiAction::Record); c.action(UiAction::Insert);
        assert(c.state() == State::Review && f.input.events.empty());
        c.tick();
        assert(c.state() == State::Queued && f.input.events.size() == 1 && f.input.events[0].size() == 24);
        // Runtime reapplies settings every frame; unchanged values must not say
        // delivery stopped while the remaining batches are still active.
        c.settings(false, false, false);
        assert(paced.active() && c.panel().status == "Input pacing - not a delivery receipt");
        assert(c.panel().detail.find("dropped") == std::string::npos);
        c.action(UiAction::Record); assert(c.state() == State::Queued);
        for (int i = 0; i < 4; ++i) {
            now += std::chrono::milliseconds(150); c.tick();
        }
        assert(f.input.events.size() == 5 && f.input.enter == 1);
        std::string joined;
        for (int i = 0; i < 4; ++i) joined += f.input.events[i];
        assert(joined == std::string(81, 'x') + " " && f.input.events.back() == "<Enter>");
        // With nothing left to review, Type is an explicit Enter; it still obeys the gap.
        c.action(UiAction::Insert); assert(f.input.events.size() == 5);
        c.action(UiAction::Enter); // ignored while the Type-Enter is pending
        c.tick(); assert(f.input.enter == 1);
        now += std::chrono::milliseconds(150); c.tick(); assert(f.input.enter == 2);
        c.action(UiAction::Enter); now += std::chrono::milliseconds(150); c.tick();
        assert(f.input.enter == 3 && f.input.text == 4);
        c.shutdown();
    }
    {
        Fixture f;
        auto now = std::chrono::steady_clock::time_point{};
        PacedDelivery paced(f.acquire(), [&] { return now; });
        auto c = f.make(false, false, &paced); ready(c);
        release(c, f); result(c, f, "review preserved");
        c.action(UiAction::Insert); f.focus.valid = false; c.tick();
        assert(c.state() == State::Review && c.panel().transcript == "review preserved" && f.input.events.empty());
        f.focus.valid = true; f.input.lose_focus_on_acquire = true;
        c.action(UiAction::Insert);
        assert(c.state() == State::Review && f.input.events.empty());
        f.input.lose_focus_on_acquire = false; f.focus.valid = true;
        c.action(UiAction::Insert); c.tick();
        assert(c.state() == State::Queued && f.input.text == 1 && f.input.enter == 0);
        c.shutdown();
    }
    {
        Fixture f;
        auto now = std::chrono::steady_clock::time_point{};
        PacedDelivery paced(f.acquire(), [&] { return now; });
        auto c = f.make(true, false, &paced); ready(c);
        release(c, f); result(c, f, std::string(81, 'a'));
        assert(c.state() == State::Queued && f.input.text == 1 && f.input.enter == 0);
        f.focus.valid = false; now += std::chrono::milliseconds(150); c.tick();
        assert(f.input.text == 1 && f.input.enter == 0 && !paced.active());
        assert(c.panel().detail.find("Focus changed during pacing") != std::string::npos);
        f.focus.valid = true;
        c.action(UiAction::Insert); assert(f.input.text == 1); // auto cannot silently retarget
        c.shutdown();
    }
    {
        Fixture f;
        auto now = std::chrono::steady_clock::time_point{};
        PacedDelivery paced(f.acquire(), [&] { return now; });
        auto c = f.make(true, false, &paced); ready(c);
        f.input.lose_focus_on_acquire = true;
        release(c, f); result(c, f, "auto review");
        assert(c.state() == State::Review && c.panel().transcript == "auto review" && f.input.events.empty());
        f.focus.valid = true; f.input.lose_focus_on_acquire = false;
        c.action(UiAction::Insert); c.tick();
        assert(c.state() == State::Queued && f.input.sent == "auto review ");
        c.shutdown();
    }
    {
        Fixture f;
        auto now = std::chrono::steady_clock::time_point{};
        PacedDelivery paced(f.acquire(), [&] { return now; });
        auto c = f.make(false, false, &paced); ready(c);
        release(c, f); result(c, f, std::string(81, 'a'));
        c.action(UiAction::Enter); c.tick();
        c.action(UiAction::Cancel); now += std::chrono::milliseconds(150); c.tick();
        assert(f.input.text == 1 && f.input.enter == 0 && c.panel().transcript.empty());
        release(c, f); result(c, f, std::string(81, 'b'));
        c.action(UiAction::Enter); now += std::chrono::milliseconds(150); c.tick();
        c.settings(false, true, false); now += std::chrono::milliseconds(150); c.tick();
        assert(f.input.text == 2 && f.input.enter == 0 && c.state() == State::Ready);
        release(c, f); result(c, f, std::string(81, 'c'));
        c.action(UiAction::Enter);
        c.backend_changed(true); // model restart drops a task before first send
        now += std::chrono::milliseconds(150); c.tick();
        assert(c.state() == State::Ready && f.input.text == 2 && f.input.enter == 0);
        release(c, f); result(c, f, std::string(81, 'd'));
        c.action(UiAction::Enter); c.action(UiAction::Quit);
        now += std::chrono::milliseconds(150); c.tick();
        assert(c.quitting() && f.input.text == 2 && f.input.enter == 0);
        c.shutdown();
    }
}
