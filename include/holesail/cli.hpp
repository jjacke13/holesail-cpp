// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include "holesail/holesail.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Port of `src/bin/holesail.mjs` plus the three modules it prints through:
// `lib/validateInput.js`, `lib/help.js` and `lib/stdout.js`. Everything except
// `main()` lives here so the parsing, validation and rendering stay testable
// without spawning a process.
namespace holesail::cli {

// A minimist-compatible argv view: --flag, --key=value, --key value,
// negative-number-aware, with positionals in `rest`.
//
// Only the subset holesail actually uses is reproduced. A token starting with
// `-` followed by a digit is a positional (so `--live -1` keeps its value),
// and a `--flag` only swallows the next token when that token is not itself a
// long option.
struct Args {
    std::map<std::string, std::string> values;   // --key value / --key=value
    std::set<std::string> flags;                 // --key with no value
    std::vector<std::string> rest;               // positionals
    bool has(std::string_view name) const;
    std::optional<std::string> str(std::string_view name) const;
    std::optional<long> num(std::string_view name) const;
    bool boolean(std::string_view name) const;   // present as a bare flag
};
Args parse_argv(int argc, char** argv);

// Port of `validateInput`. `message` holds the lines to print — it is set even
// when `ok` is true, because the UDP-host check is a warning that lets the run
// continue. `exit_code` is 2 for every failure except the empty `--key`, which
// upstream exits 0 on (quirk Q3, reproduced).
struct Validation { bool ok; std::string message; int exit_code; };
Validation validate(const Args& args);

// Port of `printHelp`. The returned string is what JS hands `console.log`, so
// the caller adds the trailing newline. `filemanager` — like any other unknown
// topic — renders the general help: the file manager is not implemented
// (divergence D1) and must not be advertised.
std::string render_help(std::string_view topic);   // "", "live", "connect", "filemanager"

// Port of `stdout.js` without the `cli-box` frame and the QR code (D2). Same
// wording, same colours, including the stacked resets that nesting produces.
std::string render_started(const Info& info);

// The `--lookup` report, or the red miss line when no record was found.
std::string render_lookup(const std::optional<LookupResult>& result);

int run(int argc, char** argv);   // called by main()

}  // namespace holesail::cli
