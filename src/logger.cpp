// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/logger.hpp"

#include "holesail/colors.hpp"

#include <cstdio>
#include <ctime>
#include <sys/time.h>

namespace holesail {
namespace {

struct LevelStyle { const char* label; std::string (*color)(std::string_view); };

LevelStyle style_of(Level level) {
    switch (level) {
        case Level::Debug: return {"DEBUG", colors::bright_black};
        case Level::Info:  return {"INFO",  colors::green};
        case Level::Warn:  return {"WARN",  colors::yellow};
        case Level::Error: return {"ERROR", colors::red};
    }
    return {"INFO", colors::green};
}

void default_sink(Level level, std::string_view line) {
    std::FILE* out = (level == Level::Warn || level == Level::Error) ? stderr : stdout;
    std::fprintf(out, "%.*s\n", static_cast<int>(line.size()), line.data());
    std::fflush(out);
}

}  // namespace

Logger::Logger(std::string prefix, bool enabled, Level min_level)
    : prefix_(std::move(prefix)),
      enabled_(enabled),
      min_level_(min_level),
      sink_(default_sink),
      clock_(&Logger::iso8601_now) {}

std::string Logger::iso8601_now() {
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    struct tm utc{};
    const time_t secs = tv.tv_sec;
    gmtime_r(&secs, &utc);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(tv.tv_usec / 1000));
    return std::string(buf);
}

void Logger::log(Level level, std::string_view msg) const {
    if (!enabled_) return;
    if (static_cast<int>(level) < static_cast<int>(min_level_)) return;

    const LevelStyle style = style_of(level);
    std::string line = colors::bright_black(clock_());
    line += ' ';
    line += colors::blue("[" + prefix_ + "]");
    line += ' ';
    line += style.color(std::string("[") + style.label + "]");
    line += ' ';
    line += msg;
    sink_(level, line);
}

}  // namespace holesail
