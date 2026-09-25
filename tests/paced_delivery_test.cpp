#include "paced_delivery.hpp"
#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>
using namespace frameyap;
using namespace std::chrono_literals;
namespace {
struct Fake {
    using Time = std::chrono::steady_clock::time_point;
    Time now{};
    std::vector<std::string> events;
    std::vector<Time> times;
    bool focus = true, fail_acquire = false, fail_text = false, fail_enter = false;
    bool lose_on_acquire = false;
    int active = 0, acquisitions = 0, releases = 0;
    struct Lease : DeliveryLease {
        Fake& f;
        explicit Lease(Fake& value) : f(value) { assert(f.active++ == 0); }
        ~Lease() override { --f.active; }
        void text(const std::string& text) override {
            f.events.push_back(text); f.times.push_back(f.now);
            if (f.fail_text) throw std::runtime_error("uncertain text");
        }
        void enter() override {
            f.events.push_back("<Enter>"); f.times.push_back(f.now);
            if (f.fail_enter) throw std::runtime_error("uncertain Enter");
        }
    };
    DeliveryFactory acquire() {
        return [this]() -> std::unique_ptr<DeliveryLease> {
            ++acquisitions;
            if (fail_acquire) throw std::runtime_error("unavailable");
            auto lease = std::make_unique<Lease>(*this);
            if (lose_on_acquire) focus = false;
            return lease;
        };
    }
    PacedDelivery make() {
        return PacedDelivery(acquire(), [this] { return now; }, [this] { ++releases; });
    }
};
void drain(Fake& f, PacedDelivery& p) {
    for (int n = 0; p.active() && n < 300; ++n) {
        p.tick(); f.now += 150ms;
    }
    assert(!p.active());
}
void bounds_and_gap() {
    Fake f; auto p = f.make();
    const std::string ascii(4096, 'a');
    int begun = 0;
    p.start(ascii, true, [&] { return f.focus; }, [&] { ++begun; });
    assert(f.events.empty() && begun == 0); // start never commits
    auto outcome = p.tick();
    assert(!outcome && begun == 1 && f.events == std::vector<std::string>{std::string(24, 'a')});
    f.now += 149ms; p.tick(); assert(f.events.size() == 1);
    f.now += 1ms; drain(f, p);
    assert(f.events.size() == 172); // ceil(4096/24) text commits plus Enter
    assert(f.events.back() == "<Enter>");
    std::string joined;
    for (size_t i = 0; i + 1 < f.events.size(); ++i) {
        assert(f.events[i].size() <= 24);
        joined += f.events[i];
    }
    assert(joined == ascii && begun == 1);
    // Another explicit action must respect the previous task's final commit.
    f.now -= 100ms;
    p.start("next", false, [&] { return f.focus; }, [] {});
    p.tick(); assert(f.events.size() == 172);
    f.now += 99ms; p.tick(); assert(f.events.size() == 172);
    f.now += 1ms; p.tick(); assert(f.events.back() == "next");
    for (size_t i = 1; i < f.times.size(); ++i) assert(f.times[i] - f.times[i-1] >= 150ms);
    assert(f.active == 0);
    f.now += 150ms; p.tick(); assert(f.releases == 1);
    p.tick(); assert(f.releases == 1);
    // 81 repeated ASCII codepoints exceed the keycode rotating-table window:
    // four 24-point messages, never a single oversized commit.
    p.start(std::string(81, 'x'), false, [&] { return f.focus; }, [] {});
    auto before = f.events.size(); drain(f, p);
    assert(f.events.size() == before + 4);
    assert(f.events[before].size() == 24 && f.events.back().size() == 9);
}
void unicode_and_partial() {
    Fake f; auto p = f.make();
    const std::string mixed = "界😀éa";
    std::string payload;
    for (int n = 0; n < 30; ++n) payload += mixed;
    p.start(payload, true, [&] { return f.focus; }, [] {});
    drain(f, p);
    std::string joined;
    for (const auto& batch : f.events) if (batch != "<Enter>") {
        assert(literal_text(batch) == batch);
        joined += batch;
    }
    assert(joined == payload && f.events.size() == 6 && f.events.back() == "<Enter>");
    f.now += 150ms;
    p.start("lost before", true, [&] { return f.focus; }, [] {});
    f.focus = false;
    auto before = f.events.size();
    auto blocked = p.tick();
    assert(blocked && !blocked->began && blocked->focus_lost && f.events.size() == before);
    f.focus = true;
    p.start(std::string(80, 'z'), true, [&] { return f.focus; }, [] {});
    p.tick(); assert(f.events.size() == before + 1);
    f.focus = false; f.now += 150ms;
    auto partial = p.tick();
    assert(partial && partial->began && partial->focus_lost && f.events.size() == before + 1);
    assert(f.events.back() != "<Enter>");
    f.focus = true; f.now += 150ms;
    p.start("text then maybe Enter", true, [&] { return f.focus; }, [] {});
    p.tick();
    f.focus = false; f.now += 150ms;
    partial = p.tick();
    assert(partial && partial->began && partial->focus_lost);
    assert(f.events.back() == "text then maybe Enter" && !p.active());
}
void permit_revocation() {
    Fake f; bool permitted = true;
    PacedDelivery p(f.acquire(), [&] { return f.now; }, {}, [&] { return permitted; });
    int consumed = 0;
    p.start("review", true, [&] { return f.focus; }, [&] { ++consumed; });
    permitted = false;
    auto stopped = p.tick();
    assert(stopped && !stopped->began && stopped->focus_lost && consumed == 0 && f.events.empty());
    permitted = true;
    p.start(std::string(81, 'p'), true, [&] { return f.focus; }, [&] { ++consumed; });
    p.tick(); assert(consumed == 1 && f.events.size() == 1);
    permitted = false; f.now += 150ms;
    stopped = p.tick();
    assert(stopped && stopped->began && f.events.size() == 1 && !p.active());
}
void refusal_cancel_uncertain() {
    Fake f; auto p = f.make();
    int consumed = 0;
    f.lose_on_acquire = true;
    try { p.start("review", false, [&] { return f.focus; }, [&] { ++consumed; }); assert(false); }
    catch (const std::runtime_error&) {}
    assert(!p.active() && consumed == 0 && f.events.empty() && f.active == 0);
    f.lose_on_acquire = false; f.focus = true; f.fail_acquire = true;
    try { p.start("review", true, [&] { return f.focus; }, [&] { ++consumed; }); assert(false); }
    catch (const std::runtime_error&) {}
    assert(!p.active() && consumed == 0 && f.events.empty());
    f.fail_acquire = false;
    p.start("cancelled before first", true, [&] { return f.focus; }, [&] { ++consumed; });
    p.cancel(); p.tick(); assert(consumed == 0 && f.events.empty() && f.active == 0);
    p.start(std::string(100, 'a'), true, [&] { return f.focus; }, [&] { ++consumed; });
    p.tick(); assert(consumed == 1 && f.events.size() == 1);
    p.cancel(); f.now += 300ms; p.tick(); assert(f.events.size() == 1 && f.releases == 2);
    f.fail_text = true;
    p.start("uncertain", true, [&] { return f.focus; }, [&] { ++consumed; });
    auto out = p.tick();
    assert(out && out->began && out->result == DeliveryResult::TextUncertain);
    assert(f.events.size() == 2 && consumed == 2);
    f.fail_text = false; f.now += 150ms;
    p.start("ok", true, [&] { return f.focus; }, [] {});
    p.tick(); f.now += 150ms; f.fail_enter = true;
    out = p.tick(); assert(out && out->result == DeliveryResult::EnterUncertain);
    assert(f.events.back() == "<Enter>");
    f.now += 150ms; f.fail_enter = false;
    p.start("body", true, [&] { return f.focus; }, [] {});
    p.tick(); f.now += 150ms; f.fail_acquire = true;
    out = p.tick(); assert(out && out->result == DeliveryResult::TextQueuedEnterUnavailable);
    assert(f.events.back() == "body" && f.active == 0);
}
} // namespace
int main() { bounds_and_gap(); unicode_and_partial(); permit_revocation(); refusal_cancel_uncertain(); }
