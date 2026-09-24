#pragma once
#include <memory>
#include <string>
namespace frameyap {
// Short-lived, exclusive IME lease acquired only for an explicit user action.
// Construction checks protocol readiness without delivering input.
class TextInput {
public:
    explicit TextInput(const std::string& socket);
    ~TextInput();
    TextInput(const TextInput&) = delete;
    TextInput& operator=(const TextInput&) = delete;
    void text(const std::string& literal);
    void enter();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
