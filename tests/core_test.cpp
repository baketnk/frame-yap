#include "core.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace frameyap;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)
void rejects(std::string text) { try { literal_text(text); } catch (const std::runtime_error&) { return; } CHECK(false); }
struct FakeDelivery {
    std::vector<std::string> events;
    int acquisitions = 0;
    int active_leases = 0;
    int fail_acquisition = 0;
    bool fail_text = false, fail_enter = false;
    struct Lease : DeliveryLease {
        FakeDelivery& fake;
        explicit Lease(FakeDelivery& f) : fake(f) { ++fake.active_leases; }
        ~Lease() override { --fake.active_leases; }
        void text(const std::string& literal) override {
            fake.events.push_back("text:" + literal);
            if (fake.fail_text) throw std::runtime_error("ambiguous text failure");
        }
        void enter() override {
            fake.events.push_back("enter");
            if (fake.fail_enter) throw std::runtime_error("ambiguous Enter failure");
        }
    };
    DeliveryFactory factory() {
        return [this]() -> std::unique_ptr<DeliveryLease> {
            ++acquisitions;
            if (active_leases || acquisitions == fail_acquisition)
                throw std::runtime_error("exclusive lease unavailable");
            return std::make_unique<Lease>(*this);
        };
    }
};
Session review(std::string_view transcript) {
    Session session;
    session.ready(); CHECK(session.record()); CHECK(session.finish(16000));
    CHECK(session.reply(session.id(), transcript)); CHECK(session.state() == State::Review);
    return session;
}
void delivery_checks() {
    {
        auto s = review("Hello 世界"); FakeDelivery fake;
        CHECK(deliver_insert(s, fake.factory()) == DeliveryResult::TextQueued);
        CHECK((fake.events == std::vector<std::string>{"text:Hello 世界 "}));
        CHECK(fake.acquisitions == 1 && s.state() == State::Queued);
        CHECK(deliver_insert(s, fake.factory()) == DeliveryResult::Ignored);
        CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::EnterQueued);
        CHECK((fake.events == std::vector<std::string>{"text:Hello 世界 ", "enter"}));
    }
    {
        auto s = review("one\n two"); FakeDelivery fake;
        CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::EnterQueued);
        CHECK((fake.events == std::vector<std::string>{"text:one  two ", "enter"}));
        CHECK(fake.acquisitions == 2 && fake.active_leases == 0 && s.state() == State::Queued);
        CHECK(deliver_insert(s, fake.factory()) == DeliveryResult::Ignored);
    }
    {
        Session s; FakeDelivery fake;
        CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::Ignored);
        s.ready(); CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::EnterQueued);
        CHECK((fake.events == std::vector<std::string>{"enter"}));
        CHECK(s.record()); CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::Ignored);
        CHECK(s.finish(16000)); CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::Ignored);
        s.fail(); CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::Ignored);
        CHECK(fake.acquisitions == 1);
    }
    {
        auto s = review("preserve"); FakeDelivery fake; fake.fail_acquisition = 1;
        try { (void)deliver_enter(s, fake.factory()); CHECK(false); }
        catch (const std::runtime_error&) {}
        CHECK(s.state() == State::Review && s.text() == "preserve" && fake.events.empty());
        CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::EnterQueued);
        CHECK((fake.events == std::vector<std::string>{"text:preserve ", "enter"}));
    }
    {
        auto s = review("uncertain"); FakeDelivery fake; fake.fail_text = true;
        CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::TextUncertain);
        CHECK(fake.acquisitions == 1 && fake.events.size() == 1);
        CHECK(s.state() == State::Queued && s.text().empty());
        CHECK(deliver_insert(s, fake.factory()) == DeliveryResult::Ignored);
    }
    {
        auto s = review("uncertain insert"); FakeDelivery fake; fake.fail_text = true;
        CHECK(deliver_insert(s, fake.factory()) == DeliveryResult::TextUncertain);
        CHECK(fake.acquisitions == 1 && fake.events.size() == 1);
        CHECK(s.state() == State::Queued && s.text().empty());
    }
    {
        auto s = review("first"); FakeDelivery fake; fake.fail_acquisition = 2;
        CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::TextQueuedEnterUnavailable);
        CHECK((fake.events == std::vector<std::string>{"text:first "}));
        CHECK(s.state() == State::Queued && fake.acquisitions == 2);
        CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::EnterQueued); // a NEW explicit press
        CHECK((fake.events == std::vector<std::string>{"text:first ", "enter"}));
    }
    {
        auto s = review("first"); FakeDelivery fake; fake.fail_enter = true;
        CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::EnterUncertain);
        CHECK((fake.events == std::vector<std::string>{"text:first ", "enter"}));
        CHECK(s.state() == State::Queued && fake.acquisitions == 2);
    }
    {
        auto s = review("already spaced "); FakeDelivery fake;
        CHECK(deliver_enter(s, fake.factory()) == DeliveryResult::EnterQueued);
        CHECK((fake.events == std::vector<std::string>{"text:already spaced ", "enter"}));
    }
    {
        auto s = review(std::string(4095, 'a')); FakeDelivery fake;
        CHECK(deliver_insert(s, fake.factory()) == DeliveryResult::TextQueued);
        CHECK(fake.events[0].size() == 5 + 4096 && fake.events[0].back() == ' ');
    }
    {
        auto s = review(std::string(4096, 'a')); FakeDelivery fake;
        try { (void)deliver_enter(s, fake.factory()); CHECK(false); }
        catch (const std::runtime_error&) {}
        CHECK(s.state() == State::Review && s.text().size() == 4096);
        CHECK(fake.acquisitions == 0 && fake.events.empty());
        auto with_existing_space = review(std::string(4095, 'a') + " ");
        CHECK(deliver_insert(with_existing_space, fake.factory()) == DeliveryResult::TextQueued);
        CHECK(fake.events[0] == "text:" + std::string(4095, 'a') + " ");
        CHECK(with_existing_space.state() == State::Queued && fake.acquisitions == 1);
    }
}
int main() {
    CHECK(literal_text("Hello 世界 😀") == "Hello 世界 😀");
    CHECK(literal_text("one\r\ntwo\tthree\xe2\x80\xa8") == "one  two three ");
    CHECK(literal_text("submit; $(echo x)") == "submit; $(echo x)");
    CHECK(literal_text(" \t\n").empty());
    CHECK(literal_text(std::string(4096, 'a')).size() == 4096);
    rejects(std::string(4097, 'a')); rejects(std::string("a\0b", 3));
    rejects("\x1b[0m"); rejects("\xc0\xaf"); rejects("\xed\xa0\x80");
    rejects("\xf4\x90\x80\x80"); rejects("\xe2\x82"); rejects("\xc2\x85");
    Session s;
    CHECK(!s.record()); s.ready(); CHECK(s.record());
    CHECK(!s.finish(3199)); CHECK(s.state() == State::Ready);
    CHECK(s.record()); auto cancelled = s.id(); CHECK(s.finish(3200));
    CHECK(!s.record()); s.cancel(); CHECK(!s.reply(cancelled, "stale"));
    CHECK(s.record()); auto current = s.id(); CHECK(s.finish(320000));
    CHECK(!s.reply(current - 1, "stale")); CHECK(s.reply(current, "text\n"));
    CHECK(!s.reply(current, "duplicate")); CHECK(!s.record());
    CHECK(s.take_insert() == "text "); CHECK(!s.take_insert());
    CHECK(!s.reply(current, "again")); CHECK(s.record()); CHECK(!s.finish(320001));
    CHECK(s.record()); CHECK(s.finish(16000)); CHECK(s.reply(s.id(), ""));
    CHECK(s.state() == State::Ready); CHECK(!s.take_insert());
    CHECK(s.record()); CHECK(s.finish(16000)); s.fail(); CHECK(!s.reply(s.id(), "bad"));
    // Request-local model or microphone errors can leave the worker loaded:
    // discard the failed session and record again without a new warmup state.
    s.cancel(); CHECK(s.state() == State::Ready);
    CHECK(s.record()); CHECK(s.finish(16000)); CHECK(s.reply(s.id(), "again"));
    CHECK(s.take_insert() == "again"); CHECK(s.record());
    s.fail(); s.cancel(); CHECK(s.record());
    delivery_checks();
    std::cout << "core checks passed\n";
}
