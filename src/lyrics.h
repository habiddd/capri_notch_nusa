#pragma once
#include <windows.h>
#include "net.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstdlib>

// Synced lyrics from LRCLIB: LRC parsing, line lookup by playback time, and the HTTP/JSON fetch.
// Memory-only; nothing is written to disk and no account or cookie is used.

struct LyricLine {
    double at;
    std::wstring text;
};

// "mm:ss", "mm:ss.xx" or "mm:ss:xx"
static bool ParseLrcStamp(const std::wstring& tag, double& t) {
    int m = 0, s = 0, n = 0;
    if (swscanf(tag.c_str(), L"%d:%d%n", &m, &s, &n) != 2 || m < 0 || s < 0 || s >= 60) return false;
    double frac = 0, scale = 0.1;
    if (n < (int)tag.size() && (tag[n] == L'.' || tag[n] == L':'))
        for (size_t k = n + 1; k < tag.size() && iswdigit(tag[k]); ++k, scale /= 10) frac += (tag[k] - L'0') * scale;
    t = m * 60 + s + frac;
    return true;
}

static std::vector<LyricLine> ParseLrc(const std::wstring& src) {
    std::vector<LyricLine> out;
    double offset = 0;
    size_t op = src.find(L"[offset:");
    if (op != std::wstring::npos) offset = _wtoi(src.c_str() + op + 8) / 1000.0;
    for (size_t pos = 0; pos < src.size() && out.size() < 10000;) {
        size_t eol = src.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = src.size();
        std::wstring line = src.substr(pos, eol - pos);
        pos = eol + 1;

        // A line may carry several stamps: "[00:12.00][01:30.50]chorus"
        std::vector<double> stamps;
        size_t i = 0;
        double t;
        while (i < line.size() && line[i] == L'[') {
            size_t close = line.find(L']', i);
            if (close == std::wstring::npos || !ParseLrcStamp(line.substr(i + 1, close - i - 1), t)) break;
            stamps.push_back(t + offset);
            i = close + 1;
        }
        if (stamps.empty()) continue; // metadata such as [ar:...] or untimed text
        std::wstring text = line.substr(i);
        while (!text.empty() && iswspace(text.back())) text.pop_back();
        while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
        for (double at : stamps) if (at >= 0) out.push_back({ at, text });
    }
    std::stable_sort(out.begin(), out.end(), [](const LyricLine& a, const LyricLine& b) { return a.at < b.at; });
    return out;
}

// Index of the line being sung at `seconds`, -1 before the first one.
static int LyricIndex(const std::vector<LyricLine>& lines, double seconds) {
    auto it = std::upper_bound(lines.begin(), lines.end(), seconds, [](double s, const LyricLine& l) { return s < l.at; });
    return (int)(it - lines.begin()) - 1;
}

// Blank stamps, note glyphs and [Instrumental]-style markers are breaks, not lines to show in the closed notch.
static bool IsVocalLine(std::wstring text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](wchar_t c) {
        return iswspace(c) || c == L'♪' || c == L'♫' || c == L'♬' || c == L'♩' || c == L'[' || c == L']' || c == L'(' || c == L')';
    }), text.end());
    for (auto& c : text) c = towlower(c);
    return !text.empty() && text != L"instrumental" && text != L"instrumentalbreak" && text != L"music" &&
           text != L"interlude" && text != L"intro" && text != L"outro" && text != L"solo";
}

// First non-empty string value of `field` in a JSON body. Enough for LRCLIB's flat objects and arrays of them.
static bool JsonString(const std::string& body, const char* field, std::wstring& out) {
    std::string needle = std::string("\"") + field + "\":";
    for (size_t p = body.find(needle); p != std::string::npos; p = body.find(needle, p + 1)) {
        size_t v = p + needle.size();
        while (v < body.size() && body[v] == ' ') v++;
        if (v >= body.size() || body[v] != '"') continue; // null: try the next result
        std::string utf8;
        size_t i = v + 1;
        for (; i < body.size() && body[i] != '"'; ++i) {
            if (body[i] != '\\' || i + 1 >= body.size()) { utf8 += body[i]; continue; }
            char e = body[++i];
            if (e == 'n') utf8 += '\n';
            else if (e == 't') utf8 += '\t';
            else if (e == 'u' && i + 4 < body.size()) {
                unsigned cp = strtoul(body.substr(i + 1, 4).c_str(), NULL, 16);
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < body.size() && body[i + 1] == '\\' && body[i + 2] == 'u') {
                    unsigned lo = strtoul(body.substr(i + 3, 4).c_str(), NULL, 16);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                }
                AppendUtf8(utf8, cp);
            } else if (e != 'r' && e != 'b' && e != 'f') utf8 += e; // \" \\ \/
        }
        if (i >= body.size() || utf8.empty()) continue;
        out = Wide(utf8);
        return true;
    }
    return false;
}

static bool LrclibGet(const std::wstring& path, std::string& body, DWORD& code) {
    return HttpsRequest(L"GET", L"lrclib.net", path, L"", "", body, code);
}

// Exact match first (title + artist + duration, like nabyscape); the search endpoint catches near-misses
// such as "- Remastered" titles or a duration a few seconds off. Returns the status line for the footer.
static std::wstring FetchSyncedLyrics(const std::wstring& title, const std::wstring& artist, int duration, std::vector<LyricLine>& lines) {
    std::wstring q = L"track_name=" + UrlEncode(title) + L"&artist_name=" + UrlEncode(artist);
    std::string body;
    DWORD code = 0;
    std::wstring lrc;
    // LRCLIB answers 503 every now and then and recovers within seconds; spaced retries hide it (runs off the UI thread)
    auto get = [&](const std::wstring& path) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            body.clear();
            if (LrclibGet(path, body, code) && code < 500) return true;
            if (attempt < 2) Sleep(1500);
        }
        return false;
    };
    bool ok = get(L"/api/get?" + q + (duration > 0 && duration <= 3600 ? L"&duration=" + std::to_wstring(duration) : L""));
    if (!(ok && code == 200 && JsonString(body, "syncedLyrics", lrc))) {
        if (ok && code == 200 && body.find("\"instrumental\":true") != std::string::npos) return L"Pista instrumental";
        if (!get(L"/api/search?" + q) || code != 200) return L"Letras no disponibles.";
        if (!JsonString(body, "syncedLyrics", lrc)) return L"Sin letra sincronizada para esta canción.";
    }
    lines = ParseLrc(lrc);
    return lines.empty() ? L"Sin letra sincronizada para esta canción." : L"Letras sincronizadas · LRCLIB";
}
