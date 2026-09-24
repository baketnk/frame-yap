#include "core.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace frameyap;
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)
void rejects(std::string text) { try { literal_text(text); } catch (const std::runtime_error&) { return; } CHECK(false); }
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
    std::cout << "core checks passed\n";
}
