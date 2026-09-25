#include "core.hpp"
#include <stdexcept>

namespace frameyap {
std::string literal_text(std::string_view input) {
    if (input.size() > 4096) throw std::runtime_error("Transcript exceeds 4096 bytes");
    std::string out;
    for (std::size_t i = 0; i < input.size();) {
        const auto start = i;
        const auto first = static_cast<unsigned char>(input[i++]);
        uint32_t cp = first;
        int continuation = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { cp = first & 31; continuation = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { cp = first & 15; continuation = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { cp = first & 7; continuation = 3; minimum = 0x10000; }
        else if (first >= 0x80) throw std::runtime_error("Invalid UTF-8 transcript");
        while (continuation--) {
            if (i == input.size()) throw std::runtime_error("Truncated UTF-8 transcript");
            auto byte = static_cast<unsigned char>(input[i++]);
            if ((byte & 0xc0) != 0x80) throw std::runtime_error("Invalid UTF-8 transcript");
            cp = (cp << 6) | (byte & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            throw std::runtime_error("Invalid UTF-8 codepoint");
        if (cp == '\n' || cp == '\r' || cp == '\t' || cp == 0x2028 || cp == 0x2029) {
            out += ' ';
        } else {
            if (cp < 32 || (cp >= 0x7f && cp <= 0x9f))
                throw std::runtime_error("Control characters rejected");
            out.append(input.substr(start, i - start));
        }
    }
    if (out.find_first_not_of(' ') == std::string::npos) out.clear();
    return out;
}
void Session::ready() { if (state_ == State::Warming) state_ = State::Ready; }
bool Session::record() {
    if (state_ != State::Ready && state_ != State::Queued) return false;
    ++id_; text_.clear(); state_ = State::Recording; return true;
}
bool Session::finish(std::size_t samples) {
    if (state_ != State::Recording) return false;
    if (samples < 3200 || samples > 320000) { cancel(); return false; }
    state_ = State::Transcribing; return true;
}
bool Session::reply(uint64_t id, std::string_view text) {
    if (id != id_ || state_ != State::Transcribing) return false;
    text_ = literal_text(text);
    state_ = text_.empty() ? State::Ready : State::Review;
    return true;
}
std::optional<std::string> Session::take_insert() {
    if (state_ != State::Review) return std::nullopt;
    auto text = std::move(text_); text_.clear(); state_ = State::Queued;
    return text;
}
void Session::cancel() { ++id_; text_.clear(); state_ = State::Ready; }
void Session::fail() { ++id_; text_.clear(); state_ = State::Error; }
namespace {
std::unique_ptr<DeliveryLease> lease(const DeliveryFactory& acquire) {
    auto result = acquire();
    if (!result) throw std::runtime_error("Input lease unavailable");
    return result;
}
DeliveryResult send_review(Session& session, const DeliveryFactory& acquire, bool submit) {
    // literal_text already validated the transcript at reply time. A suffix
    // cannot fit beside a full 4096-byte transcript: prefer the entire literal
    // with no trailing space over rejecting it or dropping transcript bytes.
    auto spaced = session.text();
    if (spaced.back() != ' ' && spaced.size() < 4096) spaced += ' ';
    auto input = lease(acquire); // Failed acquisition keeps the review available.
    session.take_insert();      // Consume before any potentially ambiguous send.
    try { input->text(spaced); }
    catch (...) { return DeliveryResult::TextUncertain; }
    if (!submit) return DeliveryResult::TextQueued;
    // The Gamescope IME is exclusive and each lease is single-use. Release
    // the text lease BEFORE acquiring another one for Enter. Never send Enter
    // unless text returned successfully; do not replay text on lease failure.
    input.reset();
    try { input = lease(acquire); }
    catch (...) { return DeliveryResult::TextQueuedEnterUnavailable; }
    try { input->enter(); }
    catch (...) { return DeliveryResult::EnterUncertain; }
    return DeliveryResult::EnterQueued;
}
}
DeliveryResult deliver_insert(Session& session, const DeliveryFactory& acquire) {
    if (session.state() != State::Review) return DeliveryResult::Ignored;
    return send_review(session, acquire, false);
}
DeliveryResult deliver_enter(Session& session, const DeliveryFactory& acquire) {
    if (session.state() == State::Review) return send_review(session, acquire, true);
    if (session.state() != State::Ready && session.state() != State::Queued) return DeliveryResult::Ignored;
    auto input = lease(acquire);
    try { input->enter(); }
    catch (...) { return DeliveryResult::EnterUncertain; }
    return DeliveryResult::EnterQueued;
}
DeliveryResult deliver_quick(std::string_view literal, const DeliveryFactory& acquire) {
    const auto safe = literal_text(literal);
    if (safe.empty() || safe != literal || safe.size() > 64)
        throw std::runtime_error("Invalid quick input");
    auto input = lease(acquire);
    try { input->text(safe); }
    catch (...) { return DeliveryResult::TextUncertain; }
    input.reset(); // Gamescope leases are single-use; never retry an uncertain text send.
    try { input = lease(acquire); }
    catch (...) { return DeliveryResult::TextQueuedEnterUnavailable; }
    try { input->enter(); }
    catch (...) { return DeliveryResult::EnterUncertain; }
    return DeliveryResult::EnterQueued;
}
}
