#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include "types.h"
#include "json.h"
#include "net.h"

// OAuth helpers: PKCE, random tokens, DPAPI-protected token files, the sign-in flow, and the ini with the user's client IDs.
// Callers must check for an empty result from RandomToken/PkceChallenge (a failed system RNG or hash).

inline std::string Base64Url(const unsigned char* p, size_t n) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (p[i] << 16) | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        if (i + 1 < n) out += tbl[(v >> 6) & 63];
        if (i + 2 < n) out += tbl[v & 63];
    }
    return out;
}

inline std::string RandomToken(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, buf.data(), (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return "";
    return Base64Url(buf.data(), bytes);
}

// S256 code_challenge: BASE64URL(SHA-256(verifier)).
inline std::string PkceChallenge(const std::string& verifier) {
    BCRYPT_ALG_HANDLE alg = NULL;
    unsigned char hash[32];
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) return "";
    NTSTATUS st = BCryptHash(alg, NULL, 0, (PUCHAR)verifier.data(), (ULONG)verifier.size(), hash, sizeof hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return BCRYPT_SUCCESS(st) ? Base64Url(hash, sizeof hash) : "";
}

// Current-user DPAPI; the file is unreadable by other accounts and by copying it to another PC.
inline bool ProtectToFile(const std::wstring& path, const std::string& plain) {
    DATA_BLOB in = { (DWORD)plain.size(), (BYTE*)plain.data() }, out = { 0, NULL };
    if (!CryptProtectData(&in, L"capri_sync", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    std::string bytes((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return WriteBytesFile(path, bytes);
}

inline bool UnprotectFromFile(const std::wstring& path, std::string& plain) {
    std::string bytes;
    if (!ReadBytesFile(path, bytes)) return false;
    DATA_BLOB in = { (DWORD)bytes.size(), (BYTE*)bytes.data() }, out = { 0, NULL };
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    plain.assign((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

// %XX and + decoded; bad escapes are kept as written.
inline std::string PercentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), NULL, 16);
            i += 2;
        } else out += s[i];
    }
    return out;
}

// "GET /?code=abc%2F&state=x HTTP/1.1" with name "code" -> "abc/"; "" when absent.
inline std::string QueryParam(const std::string& requestLine, const char* name) {
    size_t q = requestLine.find('?');
    if (q == std::string::npos) return "";
    size_t end = requestLine.find(' ', q);
    std::string query = requestLine.substr(q + 1, end == std::string::npos ? std::string::npos : end - q - 1);
    std::string key = std::string(name) + "=";
    for (size_t p = 0; p <= query.size();) {
        size_t amp = query.find('&', p);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(p, amp - p);
        if (pair.compare(0, key.size(), key) == 0) return PercentDecode(pair.substr(key.size()));
        p = amp + 1;
    }
    return "";
}

// ---- providers

struct OAuthProvider { std::wstring authHost, authPath, tokenHost, tokenPath, scope, clientId, clientSecret, redirectBase, extraAuth; };

// ---- ini: %LOCALAPPDATA%\capri_sync.ini, "key=value" per line; ';' and '#' start a comment line

struct SyncConfig { std::wstring microsoftId; }; // Google has no ID any more: it goes through the Apps Script bridge

// The user's own Microsoft app ID (see the README).
inline OAuthProvider OutlookProvider(const SyncConfig& c) {
    return { L"login.microsoftonline.com", L"/common/oauth2/v2.0/authorize", L"login.microsoftonline.com", L"/common/oauth2/v2.0/token",
             L"offline_access Calendars.ReadWrite User.Read", c.microsoftId, L"", L"http://localhost", L"prompt=select_account" };
}

inline std::wstring SyncIniPath() { return AppDataFile(L"capri_sync.ini"); }

inline std::wstring TrimWs(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r");
    return a == std::wstring::npos ? L"" : s.substr(a, s.find_last_not_of(L" \t\r") - a + 1);
}

inline std::wstring IniValue(const std::wstring& text, const wchar_t* key) {
    std::wistringstream in(text);
    for (std::wstring line; std::getline(in, line);) {
        std::wstring t = TrimWs(line);
        if (t.empty() || t[0] == L';' || t[0] == L'#') continue;
        size_t eq = t.find(L'=');
        if (eq != std::wstring::npos && TrimWs(t.substr(0, eq)) == key) return TrimWs(t.substr(eq + 1));
    }
    return L"";
}

// Replaces the value of `key` in place (comments and other lines stay), or appends it when the key is missing.
inline std::wstring SetIniValue(const std::wstring& text, const wchar_t* key, const std::wstring& value) {
    std::wstring out;
    bool done = false;
    std::wistringstream in(text);
    for (std::wstring raw; std::getline(in, raw);) {
        std::wstring t = TrimWs(raw);
        size_t eq = t.find(L'=');
        bool mine = !t.empty() && t[0] != L';' && t[0] != L'#' && eq != std::wstring::npos && TrimWs(t.substr(0, eq)) == key;
        if (mine) {
            if (!done) out += std::wstring(key) + L"=" + value + L"\r\n";
            done = true;
        } else {
            out += t + L"\r\n";
        }
    }
    if (!done) out += std::wstring(key) + L"=" + value + L"\r\n";
    return out;
}

inline SyncConfig ParseSyncConfig(const std::wstring& text) {
    return { IniValue(text, L"microsoft_client_id") };
}

// A missing file gives empty IDs, which Connect answers by creating the template.
inline SyncConfig LoadSyncConfig() {
    std::wstring text;
    ReadUtf8File(SyncIniPath(), text);
    return ParseSyncConfig(text);
}

// Writes the commented template only when the file does not exist, so the user's IDs are never overwritten.
inline bool CreateSyncIni(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    return WriteBytesFile(path, Utf8(
        L"; Capri Notch: sincronización de calendarios. Pon aquí el ID de TU app de Microsoft.\r\n"
        L"; La guía paso a paso está en el README, sección \"Sincronizar calendario\".\r\n"
        L"; Microsoft: entra.microsoft.com > Registros de aplicaciones > ID de aplicación (cliente).\r\n"
        L"microsoft_client_id=\r\n"));
}

// ---- token file: DPAPI-protected "refresh\naccount" (the account name in UTF-8)

inline bool SaveAccountToken(const std::wstring& path, const std::string& refresh, const std::wstring& account) {
    return ProtectToFile(path, refresh + "\n" + Utf8(account));
}

inline bool LoadAccountToken(const std::wstring& path, std::string& refresh, std::wstring& account) {
    std::string text;
    if (!UnprotectFromFile(path, text)) return false;
    size_t nl = text.find('\n');
    if (nl == std::string::npos || nl == 0) return false;
    refresh = text.substr(0, nl);
    account = Wide(text.substr(nl + 1));
    return true;
}

// ---- Google bridge file: DPAPI-protected "url\ntoken\naccount". The url is "" until a connection worked.

inline bool SaveBridgeFile(const std::wstring& path, const std::wstring& url, const std::string& token, const std::wstring& account) {
    if (!url.empty() && !IsBridgeUrl(url)) return false;
    return ProtectToFile(path, Utf8(url) + "\n" + token + "\n" + Utf8(account));
}

// False for a missing file, a file of another format (the old OAuth "refresh\naccount") or a url that does not pass IsBridgeUrl.
inline bool LoadBridgeFile(const std::wstring& path, std::wstring& url, std::string& token, std::wstring& account) {
    std::string text;
    if (!UnprotectFromFile(path, text)) return false;
    size_t a = text.find('\n'), b = a == std::string::npos ? a : text.find('\n', a + 1);
    if (b == std::string::npos) return false;
    std::wstring u = Wide(text.substr(0, a));
    std::string t = text.substr(a + 1, b - a - 1);
    if (t.empty() || !(u.empty() || IsBridgeUrl(u))) return false; // the outputs are set only when the file is good
    url = u;
    token = t;
    account = Wide(text.substr(b + 1));
    return true;
}

// ---- tokens over HTTPS (run on the worker thread)

// Set when the refresh token was revoked or expired: the user has to sign in again.
inline constexpr const wchar_t* kInvalidGrant = L"invalid_grant";

// GetTickCount64 ms when the access token stops being valid (expires_in, 1 h when missing)
inline long long ExpiresAt(const Json& j) {
    return (long long)GetTickCount64() + (long long)(j["expires_in"].n > 0 ? j["expires_in"].n : 3600) * 1000;
}

// Form POST to the token endpoint; false when the call failed or the answer is not JSON
inline bool TokenPost(const OAuthProvider& p, const std::vector<std::pair<std::string, std::string>>& form, Json& out) {
    std::string resp;
    DWORD code = 0;
    return HttpsRequest(L"POST", p.tokenHost, p.tokenPath, L"Content-Type: application/x-www-form-urlencoded\r\n", FormEncode(form),
                        resp, code) && JsonParse(resp, out);
}

// Renews the access token. A new refresh token from the answer replaces the old one.
inline bool OAuthRefresh(const OAuthProvider& p, std::string& refresh, std::string& access, long long& expiresAt, std::wstring& error) {
    std::vector<std::pair<std::string, std::string>> form = { { "grant_type", "refresh_token" }, { "refresh_token", refresh }, { "client_id", Utf8(p.clientId) } };
    if (!p.clientSecret.empty()) form.push_back({ "client_secret", Utf8(p.clientSecret) });
    Json j;
    if (!TokenPost(p, form, j)) { error = L"Sin conexión"; return false; }
    if (j["access_token"].str().empty()) {
        error = j["error"].str() == kInvalidGrant ? kInvalidGrant : L"No se pudo renovar la sesión";
        return false;
    }
    access = Utf8(j["access_token"].str());
    expiresAt = ExpiresAt(j);
    if (!j["refresh_token"].str().empty()) refresh = Utf8(j["refresh_token"].str());
    return true;
}

// The page the browser shows after the redirect
inline void ReplyPage(SOCKET c, const char* message) {
    std::string body = std::string("<!doctype html><meta charset=\"utf-8\"><title>Capri Notch</title>"
                                   "<body style=\"font-family:'Segoe UI',sans-serif;background:#08090c;color:#f2f2f5;text-align:center;padding-top:40px\">"
                                   "<h2>") + message + "</h2><p style=\"color:#8e9198\">Puedes cerrar esta pestaña.</p></body>";
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body;
    send(c, resp.c_str(), (int)resp.size(), 0);
}

// Browser sign-in with PKCE on a loopback port. Blocks for up to 3 minutes: call it from the worker thread only.
// On success refresh/access/expiresAt are set; on failure error says why (and nothing is changed).
inline bool OAuthLogin(const OAuthProvider& p, std::string& refresh, std::string& access, long long& expiresAt, std::wstring& error) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { error = L"No se pudo abrir el puerto local"; return false; }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only: nothing else on the network can reach the redirect
    int alen = sizeof addr;
    bool listening = ls != INVALID_SOCKET && bind(ls, (sockaddr*)&addr, sizeof addr) == 0 &&
                     getsockname(ls, (sockaddr*)&addr, &alen) == 0 && listen(ls, 1) == 0;
    std::string verifier = RandomToken(48), state = RandomToken(16), challenge = PkceChallenge(verifier);
    if (!listening || verifier.empty() || state.empty() || challenge.empty()) {
        error = L"No se pudo abrir el puerto local";
        if (ls != INVALID_SOCKET) closesocket(ls);
        WSACleanup();
        return false;
    }
    std::string redirect = Utf8(p.redirectBase) + ":" + std::to_string(ntohs(addr.sin_port));
    std::string query = FormEncode({ { "response_type", "code" }, { "client_id", Utf8(p.clientId) }, { "redirect_uri", redirect },
                                     { "scope", Utf8(p.scope) }, { "state", state }, { "code_challenge", challenge },
                                     { "code_challenge_method", "S256" } });
    std::wstring url = L"https://" + p.authHost + p.authPath + L"?" + Wide(query) + (p.extraAuth.empty() ? L"" : L"&" + p.extraAuth);
    bool opened = (INT_PTR)ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL) > 32;

    // Waits for the redirect. Stray connections (browser preconnects) are closed and ignored.
    SOCKET client = INVALID_SOCKET;
    std::string code, errParam;
    bool stateOk = false;
    ULONGLONG deadline = GetTickCount64() + 180000;
    while (opened && client == INVALID_SOCKET && GetTickCount64() < deadline) {
        timeval wait = { 1, 0 };
        fd_set set;
        FD_ZERO(&set);
        FD_SET(ls, &set);
        int ready = select(0, &set, NULL, NULL, &wait);
        if (ready < 0) break;
        if (ready == 0) continue;
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        DWORD timeoutMs = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof timeoutMs);
        std::string req;
        char buf[1024];
        int n;
        while (req.find('\n') == std::string::npos && req.size() < 8192 && (n = recv(c, buf, sizeof buf, 0)) > 0) req.append(buf, n);
        std::string line = req.substr(0, req.find('\n'));
        if (QueryParam(line, "state").empty() && QueryParam(line, "error").empty()) { closesocket(c); continue; }
        client = c;
        stateOk = QueryParam(line, "state") == state;
        code = QueryParam(line, "code");
        errParam = QueryParam(line, "error");
    }

    bool ok = false;
    if (client == INVALID_SOCKET) error = opened ? L"Tiempo agotado" : L"No se pudo abrir el navegador";
    else if (!stateOk) error = L"Respuesta no válida"; // a redirect that is not ours: no token request
    else if (!errParam.empty() || code.empty()) error = L"Cancelado";
    else {
        std::vector<std::pair<std::string, std::string>> form = { { "grant_type", "authorization_code" }, { "code", code },
                                                                  { "redirect_uri", redirect }, { "client_id", Utf8(p.clientId) },
                                                                  { "code_verifier", verifier } };
        if (!p.clientSecret.empty()) form.push_back({ "client_secret", Utf8(p.clientSecret) });
        Json j;
        if (TokenPost(p, form, j) && !j["refresh_token"].str().empty() && !j["access_token"].str().empty()) {
            refresh = Utf8(j["refresh_token"].str());
            access = Utf8(j["access_token"].str());
            expiresAt = ExpiresAt(j);
            ok = true;
        }
        if (!ok) error = L"No se pudo iniciar sesión";
    }
    if (client != INVALID_SOCKET) {
        ReplyPage(client, ok ? "Capri Notch ya está conectado." : "No se pudo conectar. Vuelve a Capri Notch e inténtalo de nuevo.");
        closesocket(client);
    }
    closesocket(ls);
    WSACleanup();
    return ok;
}
