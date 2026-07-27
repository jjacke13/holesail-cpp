// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include <string>
#include <string_view>

// Port of the `barely-colours` module. Every helper wraps its argument in an
// SGR code and a full reset (SGR 0), exactly as the JS does — including the
// stacked resets that nesting produces. There is no TTY detection in the
// original and none here.
namespace holesail::colors {

inline std::string wrap(std::string_view code, std::string_view s) {
    std::string out;
    out.reserve(code.size() + s.size() + 4);
    out += code;
    out += s;
    out += "\x1b[0m";
    return out;
}

inline std::string red(std::string_view s)          { return wrap("\x1b[31m", s); }
inline std::string green(std::string_view s)        { return wrap("\x1b[32m", s); }
inline std::string yellow(std::string_view s)       { return wrap("\x1b[33m", s); }
inline std::string blue(std::string_view s)         { return wrap("\x1b[34m", s); }
inline std::string magenta(std::string_view s)      { return wrap("\x1b[35m", s); }
inline std::string cyan(std::string_view s)         { return wrap("\x1b[36m", s); }
inline std::string bright_black(std::string_view s) { return wrap("\x1b[90m", s); }
inline std::string bold(std::string_view s)         { return wrap("\x1b[1m", s); }
inline std::string dim(std::string_view s)          { return wrap("\x1b[2m", s); }
inline std::string underline(std::string_view s)    { return wrap("\x1b[4m", s); }

}  // namespace holesail::colors
