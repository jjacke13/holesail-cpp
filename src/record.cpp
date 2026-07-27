// SPDX-License-Identifier: AGPL-3.0-or-later
// Part of holesail-cpp — a C++ port of holesail (https://github.com/holesail/holesail)
#include "holesail/record.hpp"

#include <cctype>
#include <cstdlib>

namespace holesail {
namespace {

void append_escaped(std::string& out, std::string_view s) {
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0x0f]);
                    out.push_back(kHex[c & 0x0f]);
                } else {
                    out.push_back(c);
                }
        }
    }
}

void skip_ws(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
}

// Reads a JSON string starting at the opening quote. Returns nullopt on an
// unterminated string or a trailing escape.
std::optional<std::string> read_string(std::string_view s, size_t& i) {
    if (i >= s.size() || s[i] != '"') return std::nullopt;
    i++;
    std::string out;
    while (i < s.size()) {
        const char c = s[i++];
        if (c == '"') return out;
        if (c != '\\') { out.push_back(c); continue; }
        if (i >= s.size()) return std::nullopt;
        const char e = s[i++];
        switch (e) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'u': {
                if (i + 4 > s.size()) return std::nullopt;
                // Only the ASCII subset holesail ever emits is decoded;
                // anything above 0x7f is passed through as a literal '?'.
                unsigned code = 0;
                for (int k = 0; k < 4; k++) {
                    const char h = s[i + k];
                    unsigned v;
                    if (h >= '0' && h <= '9') v = static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') v = static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v = static_cast<unsigned>(h - 'A' + 10);
                    else return std::nullopt;
                    code = (code << 4) | v;
                }
                i += 4;
                out.push_back(code < 0x80 ? static_cast<char>(code) : '?');
                break;
            }
            default: return std::nullopt;
        }
    }
    return std::nullopt;  // unterminated
}

// Skips one value of any scalar type, capturing it as raw text.
std::optional<std::string_view> read_scalar(std::string_view s, size_t& i) {
    const size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}') i++;
    if (i > s.size()) return std::nullopt;
    size_t end = i;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' ||
                           s[end - 1] == '\n' || s[end - 1] == '\r')) end--;
    if (end == start) return std::nullopt;
    return s.substr(start, end - start);
}

}  // namespace

std::string encode_record(std::string_view host, std::optional<bool> udp, uint16_t port) {
    std::string out = R"({"host":")";
    append_escaped(out, host);
    out += '"';
    if (udp.has_value()) {
        out += R"(,"udp":)";
        out += *udp ? "true" : "false";
    }
    out += R"(,"port":)";
    out += std::to_string(port);
    out += '}';
    return out;
}

std::optional<Record> decode_record(std::string_view json) {
    size_t i = 0;
    skip_ws(json, i);
    if (i >= json.size() || json[i] != '{') return std::nullopt;
    i++;

    Record rec;
    skip_ws(json, i);
    if (i < json.size() && json[i] == '}') return rec;  // empty object

    while (i < json.size()) {
        skip_ws(json, i);
        const auto key = read_string(json, i);
        if (!key.has_value()) return std::nullopt;

        skip_ws(json, i);
        if (i >= json.size() || json[i] != ':') return std::nullopt;
        i++;
        skip_ws(json, i);
        if (i >= json.size()) return std::nullopt;

        if (json[i] == '"') {
            const auto value = read_string(json, i);
            if (!value.has_value()) return std::nullopt;
            if (*key == "host") rec.host = *value;
        } else {
            const auto raw = read_scalar(json, i);
            if (!raw.has_value()) return std::nullopt;
            if (*key == "udp") {
                if (*raw == "true") rec.udp = true;
                else if (*raw == "false") rec.udp = false;
                // any other shape leaves udp unset
            } else if (*key == "port") {
                const std::string text(*raw);
                char* end = nullptr;
                const long v = std::strtol(text.c_str(), &end, 10);
                if (end != nullptr && *end == '\0' && v >= 1 && v <= 65535) {
                    rec.port = static_cast<uint16_t>(v);
                }
            }
        }

        skip_ws(json, i);
        if (i >= json.size()) return std::nullopt;
        if (json[i] == ',') { i++; continue; }
        if (json[i] == '}') return rec;
        return std::nullopt;
    }
    return std::nullopt;  // never saw the closing brace
}

}  // namespace holesail
