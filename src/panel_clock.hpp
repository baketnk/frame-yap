#pragma once
#include "config.hpp"
#include <array>
#include <ctime>
#include <string>

namespace frameyap {
struct ClockLabel { std::string time, date; };
// Local wall time; caller refreshes on minute/day change, never by forcing
// a continuous redraw. No OpenVR or system-clock changes.
inline ClockLabel panel_clock(std::time_t now, bool clock_24h, DateFormat format) {
    std::tm local{};
    if (!localtime_r(&now, &local)) return {"--:--", {}};
    std::array<char, 48> out{};
    auto format_part = [&](const char* pattern) {
        return std::strftime(out.data(), out.size(), pattern, &local) ? std::string(out.data()) : std::string();
    };
    ClockLabel result{format_part(clock_24h ? "%H:%M" : "%I:%M %p"), {}};
    switch (format) {
    case DateFormat::Off: break;
    case DateFormat::MonthDayYear: result.date = format_part("%m/%d/%Y"); break;
    case DateFormat::DayMonthYear: result.date = format_part("%d/%m/%Y"); break;
    case DateFormat::Iso: result.date = format_part("%Y-%m-%d"); break;
    }
    return result;
}
} // namespace frameyap
