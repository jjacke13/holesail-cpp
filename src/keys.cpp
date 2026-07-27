// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/keys.hpp"

#include <sodium.h>

namespace holesail {
namespace {

constexpr std::string_view kAlphabet = "ybndrfg8ejkmcpqxot1uwisza345h769";
constexpr char kHex[] = "0123456789abcdef";

int quintet_of(char c) {
    const auto pos = kAlphabet.find(c);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

int nibble_of(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string z32_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len * 8 + 4) / 5);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(kAlphabet[(acc >> bits) & 0x1f]);
        }
    }
    if (bits > 0) out.push_back(kAlphabet[(acc << (5 - bits)) & 0x1f]);
    return out;
}

std::string z32_encode(const std::vector<uint8_t>& data) {
    return z32_encode(data.data(), data.size());
}

std::optional<std::vector<uint8_t>> z32_decode(std::string_view s) {
    std::vector<uint8_t> out;
    out.reserve(s.size() * 5 / 8);
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : s) {
        const int v = quintet_of(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 5) | static_cast<uint32_t>(v);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
        }
    }
    return out;
}

std::array<uint8_t, 32> sha256(std::string_view s) {
    std::array<uint8_t, 32> out{};
    crypto_hash_sha256(out.data(),
                       reinterpret_cast<const uint8_t*>(s.data()), s.size());
    return out;
}

std::string to_hex(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0f]);
    }
    return out;
}

std::optional<std::vector<uint8_t>> from_hex(std::string_view s) {
    if (s.size() % 2 != 0) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int hi = nibble_of(s[i]);
        const int lo = nibble_of(s[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string random_hex32() {
    uint8_t buf[32];
    randombytes_buf(buf, sizeof(buf));
    return to_hex(buf, sizeof(buf));
}

ParsedUrl parse_url(std::string_view url) {
    ParsedUrl out;
    constexpr std::string_view kProtocol = "hs://";
    // JS: url.substring(0,5) === 'hs://' && url.substring(5,9).length === 4
    if (url.size() >= 9 && url.substr(0, kProtocol.size()) == kProtocol) {
        out.key = std::string(url.substr(9));
    } else {
        out.key = std::string(url);
    }
    // JS sets `secure = true` only when url[5] === 's'; it is otherwise left
    // undefined. Reproduced verbatim, including quirk Q2 (the sniff runs on
    // unprefixed strings too).
    if (url.size() > 5 && url[5] == 's') out.secure = true;
    return out;
}

}  // namespace holesail
