#pragma once
#include <string>
namespace frameyap {
struct Options {
    std::string assets, font, python = "python3", worker, model, socket;
    int threads = 2;
    bool hand = true;
};
int run(const Options& options);
}
