// host_test/tinyjson.hpp
//
// Tiny self-contained JSON reader/writer used by the host-side test
// driver. We keep it minimal: only the subset the BlockOutlineConfig
// JSON needs (bool, int, float, hex-string-as-int, string). No
// nlohmann::json because that header isn't available on this build
// host and we want the test to stand on its own.
//
// Format produced / consumed is loose (no whitespace). Strings and
// numbers round-trip flatly. The reader is forgiving: top-level
// braces required, keys come back as strings, values come back as
// one of the Value variants.

#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace tinyjson {

struct Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

struct Value {
    enum class Kind { Null, Bool, Int, Float, String, Array_, Object_ };
    Kind kind = Kind::Null;

    bool        boolean = false;
    long long   integer = 0;
    double      number  = 0.0;
    std::string str;
    Array       arr;
    Object      obj;

    [[nodiscard]] bool isBool()   const { return kind == Kind::Bool; }
    [[nodiscard]] bool isInt()    const { return kind == Kind::Int || kind == Kind::Float; }
    [[nodiscard]] bool isFloat()  const { return kind == Kind::Float || kind == Kind::Int; }
    [[nodiscard]] bool isString() const { return kind == Kind::String; }

    [[nodiscard]] bool        asBool(bool def = false) const {
        return isBool() ? boolean : def;
    }
    [[nodiscard]] long long   asInt(long long def = 0) const {
        if (kind == Kind::Int)   return integer;
        if (kind == Kind::Float) return static_cast<long long>(number);
        return def;
    }
    [[nodiscard]] double      asFloat(double def = 0.0) const {
        if (kind == Kind::Float) return number;
        if (kind == Kind::Int)   return static_cast<double>(integer);
        return def;
    }
    [[nodiscard]] std::string asString(const std::string& def = {}) const {
        return isString() ? str : def;
    }
    [[nodiscard]] bool contains(const std::string& k) const {
        return kind == Kind::Object_ && obj.count(k) != 0;
    }
};

namespace detail {

inline void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

inline Value parse(const std::string& s, size_t& i);

inline std::string parseString(const std::string& s, size_t& i) {
    // s[i] == '"'
    ++i;
    std::string out;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            switch (c) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                default:   out += c;    break;
            }
            i += 2;
        } else {
            out += s[i++];
        }
    }
    if (i < s.size()) ++i; // consume closing quote
    return out;
}

inline Value parseNumber(const std::string& s, size_t& i) {
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                            s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                            s[i] == '+' || s[i] == '-')) ++i;
    std::string num = s.substr(start, i - start);
    Value v;
    if (num.find('.') != std::string::npos ||
        num.find('e') != std::string::npos ||
        num.find('E') != std::string::npos) {
        v.kind   = Value::Kind::Float;
        v.number = std::strtod(num.c_str(), nullptr);
    } else {
        v.kind   = Value::Kind::Int;
        v.integer = std::strtoll(num.c_str(), nullptr, 10);
    }
    return v;
}

inline Value parseLiteral(const std::string& s, size_t& i, const char* lit, Value v) {
    size_t start = i;
    ++i;
    while (*lit && i < s.size() && s[i] == *lit) { ++lit; ++i; }
    (void)start;
    return v;
}

inline Value parseArray(const std::string& s, size_t& i) {
    Value v;
    v.kind = Value::Kind::Array_;
    ++i; // '['
    skipWs(s, i);
    while (i < s.size() && s[i] != ']') {
        v.arr.push_back(parse(s, i));
        skipWs(s, i);
        if (i < s.size() && s[i] == ',') { ++i; skipWs(s, i); }
    }
    if (i < s.size()) ++i; // ']'
    return v;
}

inline Value parseObject(const std::string& s, size_t& i) {
    Value v;
    v.kind = Value::Kind::Object_;
    ++i; // '{'
    skipWs(s, i);
    while (i < s.size() && s[i] != '}') {
        std::string k = parseString(s, i);
        skipWs(s, i);
        if (i < s.size() && s[i] == ':') { ++i; skipWs(s, i); }
        Value child = parse(s, i);
        v.obj.emplace(std::move(k), std::move(child));
        skipWs(s, i);
        if (i < s.size() && s[i] == ',') { ++i; skipWs(s, i); }
    }
    if (i < s.size()) ++i; // '}'
    return v;
}

inline Value parse(const std::string& s, size_t& i) {
    skipWs(s, i);
    if (i >= s.size()) return {};
    char c = s[i];
    if (c == '{') return parseObject(s, i);
    if (c == '[') return parseArray(s, i);
    if (c == '"') {
        Value v; v.kind = Value::Kind::String; v.str = parseString(s, i); return v;
    }
    if (c == 't' || c == 'f') {
        return parseLiteral(s, i, c == 't' ? "rue" : "alse",
                            [&]{ Value v; v.kind = Value::Kind::Bool; v.boolean = (c == 't'); return v; }());
    }
    if (c == 'n') {
        Value v; v.kind = Value::Kind::Null; i += 4; return v;
    }
    return parseNumber(s, i);
}

inline void writeString(std::string& s, const std::string& v) {
    s += '"';
    for (char c : v) {
        switch (c) {
            case '"':  s += "\\\""; break;
            case '\\': s += "\\\\"; break;
            case '\n': s += "\\n"; break;
            case '\t': s += "\\t"; break;
            case '\r': s += "\\r"; break;
            default:   s += c;       break;
        }
    }
    s += '"';
}

inline void write(std::string& s, const Value& v) {
    switch (v.kind) {
        case Value::Kind::Null:   s += "null"; break;
        case Value::Kind::Bool:   s += v.boolean ? "true" : "false"; break;
        case Value::Kind::Int:    s += std::to_string(v.integer); break;
        case Value::Kind::Float: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6f", v.number);
            s += buf;
            break;
        }
        case Value::Kind::String: writeString(s, v.str); break;
        case Value::Kind::Array_: {
            s += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) s += ',';
                write(s, v.arr[i]);
            }
            s += ']';
            break;
        }
        case Value::Kind::Object_: {
            s += '{';
            size_t i = 0;
            for (const auto& [k, child] : v.obj) {
                if (i++) s += ',';
                writeString(s, k);
                s += ':';
                write(s, child);
            }
            s += '}';
            break;
        }
    }
}

} // namespace detail

inline Value fromString(const std::string& s) {
    size_t i = 0;
    return detail::parse(s, i);
}

inline std::string toString(const Value& v) {
    std::string out;
    detail::write(out, v);
    return out;
}

} // namespace tinyjson
