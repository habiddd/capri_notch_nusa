#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include "net.h"

// Minimal JSON reader for Google and Microsoft Graph replies. Keys stay UTF-8, values become wide strings.
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json& operator[](const std::string& key) const {
        static const Json none;
        for (const auto& kv : o) if (kv.first == key) return kv.second;
        return none;
    }
    const Json& operator[](size_t i) const {
        static const Json none;
        return (t == Arr && i < a.size()) ? a[i] : none;
    }
    std::wstring str() const { return t == Str ? s : L""; }
};

inline void JsonSkip(const std::string& s, size_t& p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
}

// Four hex digits at p; false if they are not there.
inline bool JsonHex4(const std::string& s, size_t p, unsigned& cp) {
    if (p + 4 > s.size()) return false;
    cp = 0;
    for (size_t i = 0; i < 4; ++i) {
        char c = s[p + i];
        unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        cp = cp * 16 + d;
    }
    return true;
}

// p is on the opening quote; the result is UTF-8. Surrogate pairs are joined here.
inline bool JsonParseString(const std::string& s, size_t& p, std::string& out) {
    for (p++; p < s.size();) {
        char c = s[p++];
        if (c == '"') return true;
        if ((unsigned char)c < 0x20) return false;
        if (c != '\\') { out += c; continue; }
        if (p >= s.size()) return false;
        char e = s[p++];
        switch (e) {
        case '"': case '\\': case '/': out += e; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            unsigned cp;
            if (!JsonHex4(s, p, cp)) return false;
            p += 4;
            unsigned lo;
            if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u' &&
                JsonHex4(s, p + 2, lo) && lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
            AppendUtf8(out, cp);
            break;
        }
        default: return false;
        }
    }
    return false; // unterminated
}

inline bool JsonParseAt(const std::string& s, size_t& p, Json& v, int depth) {
    if (depth > 64) return false;
    JsonSkip(s, p);
    if (p >= s.size()) return false;
    char c = s[p];
    if (c == '{') {
        v.t = Json::Obj;
        p++;
        JsonSkip(s, p);
        if (p < s.size() && s[p] == '}') { p++; return true; }
        for (;;) {
            JsonSkip(s, p);
            if (p >= s.size() || s[p] != '"') return false;
            std::string key;
            if (!JsonParseString(s, p, key)) return false;
            JsonSkip(s, p);
            if (p >= s.size() || s[p] != ':') return false;
            p++;
            v.o.emplace_back();
            v.o.back().first = key;
            if (!JsonParseAt(s, p, v.o.back().second, depth + 1)) return false;
            JsonSkip(s, p);
            if (p < s.size() && s[p] == ',') { p++; continue; }
            if (p < s.size() && s[p] == '}') { p++; return true; }
            return false;
        }
    }
    if (c == '[') {
        v.t = Json::Arr;
        p++;
        JsonSkip(s, p);
        if (p < s.size() && s[p] == ']') { p++; return true; }
        for (;;) {
            v.a.emplace_back();
            if (!JsonParseAt(s, p, v.a.back(), depth + 1)) return false;
            JsonSkip(s, p);
            if (p < s.size() && s[p] == ',') { p++; continue; }
            if (p < s.size() && s[p] == ']') { p++; return true; }
            return false;
        }
    }
    if (c == '"') {
        std::string u8;
        if (!JsonParseString(s, p, u8)) return false;
        v.t = Json::Str;
        v.s = Wide(u8);
        return true;
    }
    if (s.compare(p, 4, "true") == 0) { v.t = Json::Bool; v.b = true; p += 4; return true; }
    if (s.compare(p, 5, "false") == 0) { v.t = Json::Bool; v.b = false; p += 5; return true; }
    if (s.compare(p, 4, "null") == 0) { v.t = Json::Null; p += 4; return true; }
    if (c != '-' && !isdigit((unsigned char)c)) return false;
    // JSON number grammar checked by hand; strtod does the conversion
    size_t start = p;
    if (s[p] == '-') p++;
    size_t digits = p;
    while (p < s.size() && isdigit((unsigned char)s[p])) p++;
    if (p == digits) return false;
    if (p < s.size() && s[p] == '.') {
        p++;
        size_t frac = p;
        while (p < s.size() && isdigit((unsigned char)s[p])) p++;
        if (p == frac) return false;
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
        p++;
        if (p < s.size() && (s[p] == '+' || s[p] == '-')) p++;
        size_t exp = p;
        while (p < s.size() && isdigit((unsigned char)s[p])) p++;
        if (p == exp) return false;
    }
    v.t = Json::Num;
    v.n = strtod(s.substr(start, p - start).c_str(), NULL);
    return true;
}

// False for anything that is not one complete JSON value (trailing garbage included).
inline bool JsonParse(const std::string& utf8, Json& out) {
    size_t p = 0;
    out = Json();
    if (!JsonParseAt(utf8, p, out, 0)) return false;
    JsonSkip(utf8, p);
    return p == utf8.size();
}

// "text" in UTF-8 with JSON escapes.
inline std::string JsonQuote(const std::wstring& s) {
    std::string out = "\"";
    char buf[8];
    for (unsigned char c : Utf8(s)) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
            else out += (char)c;
        }
    }
    return out + "\"";
}
