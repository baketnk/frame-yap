#pragma once
#include "cli.hpp"
namespace frameyap {
// Local inventory only; runs the shared Python verifier with argv (no shell).
int model_status(const CliOptions& options, const char* executable);
}
