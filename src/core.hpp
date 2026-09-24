#pragma once
#include <cstdint>
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
}
