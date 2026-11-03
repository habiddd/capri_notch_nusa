#pragma once
#include <windows.h>
#include <winhttp.h>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// Shared HTTPS (WinHTTP over TLS) and UTF-8 helpers for LRCLIB, Google and Microsoft Graph.

inline std::string Utf8(const std::wstring& s) {
    std::string out(WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], (int)out.size(), NULL, NULL);
    return out;
}

inline std::wstring Wide(const std::string& s) {
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], (int)out.size());
    return out;
}

inline void AppendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

// Percent-encodes UTF-8 bytes; RFC 3986 unreserved characters pass through.
inline std::string PercentEncode(const std::string& utf8) {
    std::string out;
    char hex[4];
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { snprintf(hex, sizeof hex, "%%%02X", c); out += hex; }
    }
    return out;
}

inline std::wstring UrlEncode(const std::wstring& s) { return Wide(PercentEncode(Utf8(s))); }

// a=b&c=d with both sides percent-encoded.
inline std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    for (size_t i = 0; i < kv.size(); ++i) {
        if (i) out += '&';
        out += PercentEncode(kv[i].first) + "=" + PercentEncode(kv[i].second);
    }
    return out;
}

// "https://host/path?q" -> host and "/path?q"; false for anything that is not https.
inline bool SplitUrl(const std::wstring& url, std::wstring& host, std::wstring& pathAndQuery) {
    const std::wstring prefix = L"https://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;
    size_t end = url.find_first_of(L"/?", prefix.size());
    host = url.substr(prefix.size(), end == std::wstring::npos ? std::wstring::npos : end - prefix.size());
    pathAndQuery = end == std::wstring::npos ? L"/" : (url[end] == L'?' ? L"/" + url.substr(end) : url.substr(end));
    return !host.empty();
}

// Blocking: call it off the UI thread. resp gets the raw body (capped at 8 MB), code the HTTP status.
// False on network failure or an oversized body; an error status still returns true with code set.
// headers: "Name: value\r\n" lines, body: raw bytes (JSON for POST/PATCH).
// location: when set, redirects are not followed and the Location header comes back in it (see HttpsPostFollow).
inline bool HttpsRequest(const wchar_t* method, const std::wstring& host, const std::wstring& pathAndQuery,
                         const std::wstring& headers, const std::string& body, std::string& resp, DWORD& code,
                         std::wstring* location = nullptr) {
    bool ok = false;
    code = 0;
    resp.clear();
    HINTERNET s = WinHttpOpen(L"CapriNotch/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 5000, 5000, 15000, 15000);
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, method, pathAndQuery.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (r && location) {
        DWORD off = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off);
    }
    if (r && WinHttpSendRequest(r, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), (DWORD)headers.size(),
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD size = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        if (location) {
            wchar_t loc[2048];
            DWORD n = sizeof loc;
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &n, WINHTTP_NO_HEADER_INDEX)) *location = loc;
        }
        ok = true;
        DWORD avail = 0;
        // 8 MB: a Graph calendarView page of 500 events with bodies is well above LRCLIB's 1 MB answers
        while (WinHttpQueryDataAvailable(r, &avail) && avail > 0) {
            if (resp.size() + avail > (8u << 20)) { ok = false; break; }
            size_t old = resp.size();
            resp.resize(old + avail);
            DWORD read = 0;
            if (!WinHttpReadData(r, &resp[old], avail, &read)) { ok = false; break; }
            resp.resize(old + read);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// Hosts a bridge redirect may go to: Apps Script, and the googleusercontent host that serves its answer.
inline bool BridgeRedirectHost(const std::wstring& h) {
    const std::wstring suffix = L".googleusercontent.com";
    return h == L"script.google.com" || (h.size() > suffix.size() && h.compare(h.size() - suffix.size(), suffix.size(), suffix) == 0);
}

// The web app URL of a bridge: https://script.google.com/macros/s/<id>/exec, nothing else. Checked before it is saved or called.
inline bool IsBridgeUrl(const std::wstring& url) {
    const std::wstring pre = L"https://script.google.com/macros/s/", post = L"/exec";
    if (url.size() <= pre.size() + post.size() || url.compare(0, pre.size(), pre) != 0 ||
        url.compare(url.size() - post.size(), post.size(), post) != 0) return false;
    for (size_t i = pre.size(); i < url.size() - post.size(); ++i) {
        wchar_t c = url[i];
        bool alnum = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        if (!alnum && c != L'-' && c != L'_') return false;
    }
    return true;
}

// POST to an Apps Script web app. It answers with a redirect to googleusercontent, which is read with GET.
// Redirects go nowhere else, at most 3 hops. False when the call failed or a redirect was refused.
inline bool HttpsPostFollow(const std::wstring& url, const std::string& body, std::string& resp, DWORD& code) {
    std::wstring host, pq, loc;
    if (!SplitUrl(url, host, pq) || !HttpsRequest(L"POST", host, pq, L"Content-Type: application/json\r\n", body, resp, code, &loc)) return false;
    for (int hop = 0; code == 301 || code == 302 || code == 303 || code == 307; ++hop) {
        std::wstring h, p;
        if (hop == 3 || !SplitUrl(loc, h, p) || !BridgeRedirectHost(h)) return false;
        if (!HttpsRequest(L"GET", h, p, L"", "", resp, code, &loc)) return false;
    }
    return true;
}
