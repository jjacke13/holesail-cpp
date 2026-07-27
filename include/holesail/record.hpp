// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace holesail {

// The mutable DHT record holesail publishes at its public key:
//   {"host":"127.0.0.1","udp":true,"port":8080}
// `udp` is absent when the server was not started with --udp, because
// JS JSON.stringify drops undefined values. Field order is host, udp, port.
struct Record {
    std::string host;
    std::optional<bool> udp;
    std::optional<uint16_t> port;
};

// Byte-for-byte identical to the JS JSON.stringify output. `udp` is omitted
// entirely when nullopt.
std::string encode_record(std::string_view host, std::optional<bool> udp, uint16_t port);

// Minimal reader for a flat JSON object with string / bool / integer values.
// Unknown keys are ignored. Returns nullopt if the input is not a
// well-formed flat object. A `port` outside 1..65535 is reported as absent
// rather than failing the whole record.
std::optional<Record> decode_record(std::string_view json);

}  // namespace holesail
