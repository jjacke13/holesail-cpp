// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace holesail {

// Mirrors holesail-logger: DEBUG 0, INFO 1, WARN 2, ERROR 3.
enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Emits `<ISO8601> [prefix] [LEVEL] message`. DEBUG and INFO go to stdout,
// WARN and ERROR to stderr, matching the JS console.log/warn/error split.
class Logger {
public:
    Logger(std::string prefix, bool enabled, Level min_level);

    void log(Level level, std::string_view msg) const;
    void debug(std::string_view msg) const { log(Level::Debug, msg); }
    void info(std::string_view msg) const  { log(Level::Info, msg); }
    void warn(std::string_view msg) const  { log(Level::Warn, msg); }
    void error(std::string_view msg) const { log(Level::Error, msg); }

    bool enabled() const { return enabled_; }

    // Test seams. The defaults write to stdout/stderr and read the wall clock.
    void set_sink(std::function<void(Level, std::string_view)> sink) {
        sink_ = std::move(sink);
    }
    void set_clock(std::function<std::string()> clock) { clock_ = std::move(clock); }

    // "2026-07-28T00:44:12.345Z" — same shape as JS Date#toISOString.
    static std::string iso8601_now();

private:
    std::string prefix_;
    bool enabled_;
    Level min_level_;
    std::function<void(Level, std::string_view)> sink_;
    std::function<std::string()> clock_;
};

}  // namespace holesail
