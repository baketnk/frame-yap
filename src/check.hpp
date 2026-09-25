#pragma once
#include <string>
#include <optional>
#ifdef FRAMEYAP_NATIVE
#include "mount.hpp"
namespace frameyap {
int check_input(const std::string& socket);
int check_overlay(const std::string& assets, const std::string& font,
                  std::optional<Mount> mount, bool controls);
}
#endif
