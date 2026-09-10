#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// Minimal JSON reader/writer for the messages exchanged with the WebView UI.
namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    const Value& operator[](const std::string& key) const {
        static const Value nil;
        auto it = o.find(key);
        return it == o.end() ? nil : it->second;
    }
    bool has(const std::string& key) const { return o.find(key) != o.end(); }
    std::string str(const std::string& def = "") const { return type == String ? s : def; }
    long long i64(long long def = 0) const {
        if (type == Number) return (long long)n;
        if (type == String) return _atoi64(s.c_str());
        return def;
    }
    bool boolean(bool def = false) const { return type == Bool ? b : def; }
};

namespace detail {

struct Parser {
    const std::string& s;
    size_t p = 0;

    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p;
    }

    bool hex4(unsigned& out) {
        if (p + 4 > s.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[p++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= c - '0';
            else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }

    bool str(std::string& out) {
        if (p >= s.size() || s[p] != '"') return false;
        ++p;
        while (p < s.size()) {
            char c = s[p++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) return false;
            char e = s[p++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                    p += 2;
                    unsigned lo = 0;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool parse(Value& v, int depth) {
        if (depth > 64) return false;
        ws();
        if (p >= s.size()) return false;
        char c = s[p];
        if (c == '{') {
            ++p;
            v.type = Value::Object;
            ws();
            if (p < s.size() && s[p] == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!str(key)) return false;
                ws();
                if (p >= s.size() || s[p] != ':') return false;
                ++p;
                Value child;
                if (!parse(child, depth + 1)) return false;
                v.o[key] = std::move(child);
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p;
            v.type = Value::Array;
            ws();
            if (p < s.size() && s[p] == ']') { ++p; return true; }
            for (;;) {
                Value child;
                if (!parse(child, depth + 1)) return false;
                v.a.push_back(std::move(child));
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Value::String; return str(v.s); }
        if (s.compare(p, 4, "true") == 0) { v.type = Value::Bool; v.b = true; p += 4; return true; }
        if (s.compare(p, 5, "false") == 0) { v.type = Value::Bool; v.b = false; p += 5; return true; }
        if (s.compare(p, 4, "null") == 0) { v.type = Value::Null; p += 4; return true; }
        const char* start = s.c_str() + p;
        char* end = nullptr;
        double d = strtod(start, &end);
        if (end == start) return false;
        v.type = Value::Number;
        v.n = d;
        p += (size_t)(end - start);
        return true;
    }
};

}

inline bool Parse(const std::string& text, Value& out) {
    detail::Parser parser{ text };
    if (!parser.parse(out, 0)) return false;
    parser.ws();
    return parser.p == text.size();
}

inline std::string Quote(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    o += '"';
    return o;
}

}
