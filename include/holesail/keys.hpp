// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace holesail {

// z-base-32, alphabet "ybndrfg8ejkmcpqxot1uwisza345h769".
// Port of the `z32` npm module used by holesail.
std::string z32_encode(const uint8_t* data, size_t len);
std::string z32_encode(const std::vector<uint8_t>& data);

// Returns nullopt if the input contains a character outside the alphabet.
std::optional<std::vector<uint8_t>> z32_decode(std::string_view s);

// SHA-256 of the string's bytes. holesail hashes the key STRING, so this
// takes a string_view rather than a byte span on purpose.
std::array<uint8_t, 32> sha256(std::string_view s);

std::string to_hex(const uint8_t* data, size_t len);

// Returns nullopt on odd length or a non-hex character.
std::optional<std::vector<uint8_t>> from_hex(std::string_view s);

// 32 random bytes as 64 lowercase hex characters — the auto-generated
// holesail key (JS: libKeys.randomBytes(32).toString('hex')).
std::string random_hex32();

struct ParsedUrl {
    std::string key;
    // nullopt means "the url did not determine it" — matching JS, which
    // leaves `secure` undefined rather than setting it to false.
    std::optional<bool> secure;
};

// Port of Holesail.urlParser. Strips an "hs://" prefix plus its 4-character
// mode field, and sniffs `secure` from character index 5.
ParsedUrl parse_url(std::string_view url);

}  // namespace holesail
