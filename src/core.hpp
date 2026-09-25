#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace frameyap {
// Throws on malformed UTF-8, controls, or >4096 bytes. Never interprets commands.
std::string literal_text(std::string_view input);
enum class State { Warming, Ready, Recording, Transcribing, Review, Queued, Error };
// Hardware-free delivery ledger. A request can be consumed only once, including
// ambiguous transport failures. Explicit Insert authorizes CURRENT seat focus.
class Session {
public:
    State state() const { return state_; }
    const std::string& text() const { return text_; }
    uint64_t id() const { return id_; }
    void ready();
    bool record();
    bool finish(std::size_t samples);
    bool reply(uint64_t id, std::string_view text);
    std::optional<std::string> take_insert();
    void cancel();
    void fail();
private:
    State state_ = State::Warming;
    uint64_t id_ = 0;
    std::string text_;
};
// Each lease is single-use. Constructing it must only check availability, not
// deliver input. After a possibly ambiguous text failure the review is consumed.
class DeliveryLease {
public:
    virtual ~DeliveryLease() = default;
    virtual void text(const std::string& literal) = 0;
    virtual void enter() = 0;
};
using DeliveryFactory = std::function<std::unique_ptr<DeliveryLease>()>;
enum class DeliveryResult {
    Ignored, TextQueued, EnterQueued, TextUncertain, EnterUncertain, TextQueuedEnterUnavailable
};
// Lease acquisition before consumption can throw while leaving review intact.
// A full 4096-byte transcript is sent intact without an additional space;
// never truncate transcript bytes merely to make room for the suffix.
DeliveryResult deliver_insert(Session& session, const DeliveryFactory& acquire);
// Review: insert text plus a trailing space, then explicitly send Enter only
// after successful text delivery. Ready/Queued: Enter alone.
DeliveryResult deliver_enter(Session& session, const DeliveryFactory& acquire);
// Explicit quick input: exact configured literal, then Enter (no trailing space).
DeliveryResult deliver_quick(std::string_view literal, const DeliveryFactory& acquire);
}
