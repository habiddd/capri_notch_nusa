// g++ -std=c++17 -Wall tests/sync_test.cpp -o %TEMP%\sync_test.exe -lws2_32 -lbcrypt -lcrypt32 -lwinhttp -lshell32 && sync_test.exe
#define _WIN32_WINNT 0x0A00 // same prelude as main.cpp: the headers are not self-contained
#include <windows.h>
#include <cassert>
#include <cctype>
#include <cstdio>
#include "../src/net.h"
#include "../src/json.h"
#include "../src/oauth.h"
#include "../src/sync.h"

static long long Gm(int y, int mo, int d, int h, int mi) { // UTC epoch of these fields
    tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi;
    return _mkgmtime(&t);
}
static long long LocalEpoch(int y, int mo, int d, int h, int mi) { // epoch of these local wall-clock fields
    tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
    return mktime(&t);
}
static CalEvent Remote(const wchar_t* rid, int date, int src, int dirty, long long uid) {
    CalEvent e{ date, 540, -1, 0, 0, L"remoto" };
    e.src = src; e.rid = rid; e.dirty = dirty; e.uid = uid; e.rev = 1;
    return e;
}
static int CountRid(const std::vector<CalEvent>& v, const wchar_t* rid) {
    int n = 0;
    for (const CalEvent& e : v) n += e.rid == rid;
    return n;
}

int main() {
    Json j;
    assert(JsonParse(R"({"a":{"b":[1,{"c":"x"}]},"t":true,"f":false,"z":null})", j));
    assert(j["a"]["b"][0].n == 1 && j["a"]["b"][1]["c"].str() == L"x");
    assert(j["t"].t == Json::Bool && j["t"].b && j["f"].t == Json::Bool && !j["f"].b && j["z"].t == Json::Null);
    assert(j["missing"].t == Json::Null && j["a"]["b"][9].t == Json::Null);

    assert(JsonParse(R"("a\"b\\c\nd")", j) && j.str() == L"a\"b\\c\nd");
    assert(JsonParse(R"("é")", j) && j.str() == L"é");
    assert(JsonParse(R"("😀")", j) && j.str().size() == 2 && j.str()[0] == 0xD83D && j.str()[1] == 0xDE00);
    assert(JsonParse("-1.5e2", j) && j.t == Json::Num && j.n == -150);

    assert(!JsonParse("{", j));
    assert(!JsonParse(R"({"a":})", j));
    assert(!JsonParse("[1,]", j));
    assert(!JsonParse("{} x", j));
    assert(!JsonParse(std::string(70, '[') + std::string(70, ']'), j)); // depth limit 64

    std::wstring hola = L"Hola \"é\"\n\tcafé \U0001F600 \\";
    assert(JsonParse(JsonQuote(hola), j) && j.str() == hola);
    assert(JsonQuote(L"a\x01") == "\"a\\u0001\"");

    assert(PkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    std::string r1 = RandomToken(32), r2 = RandomToken(32);
    assert(r1.size() == 43 && r1 != r2);
    for (char c : r1) assert(isalnum((unsigned char)c) || c == '-' || c == '_');

    assert(FormEncode({ { "a b", "ñ&=" } }) == "a%20b=%C3%B1%26%3D");
    assert(UrlEncode(L"a b") == L"a%20b");

    assert(QueryParam("GET /?code=abc%2F&state=x HTTP/1.1", "code") == "abc/");
    assert(QueryParam("GET /?code=abc%2F&state=x HTTP/1.1", "state") == "x");
    assert(QueryParam("GET /?code=abc%2F&state=x HTTP/1.1", "missing") == "");
    assert(QueryParam("GET /?error=access_denied&state=x HTTP/1.1", "error") == "access_denied");
    assert(QueryParam("GET /?xcode=1&code=a+b HTTP/1.1", "code") == "a b");

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"capri_sync_test.bin";
    std::string secret = "refresh_token_secreto_ñ";
    assert(ProtectToFile(path, secret));
    std::string raw, back;
    assert(ReadBytesFile(path, raw) && raw.find("secreto") == std::string::npos);
    assert(UnprotectFromFile(path, back) && back == secret);
    DeleteFileW(path.c_str());

    std::wstring host, pq;
    assert(SplitUrl(L"https://graph.microsoft.com/v1.0/me/calendarView?$skip=2", host, pq));
    assert(host == L"graph.microsoft.com" && pq == L"/v1.0/me/calendarView?$skip=2");
    assert(SplitUrl(L"https://www.googleapis.com?x=1", host, pq) && host == L"www.googleapis.com" && pq == L"/?x=1");
    assert(!SplitUrl(L"http://x/y", host, pq));

    // RFC 3339 forms; the offset forms are checked against _mkgmtime, so the machine's zone does not matter
    long long u = 0;
    assert(ParseRfc3339(L"2026-11-05T15:30:00Z", u) && u == Gm(2026, 11, 5, 15, 30));
    assert(ParseRfc3339(L"2026-11-05T15:30:00-06:00", u) && u == Gm(2026, 11, 5, 21, 30));
    assert(ParseRfc3339(L"2026-11-05T15:30:00.0000000", u) && u == Gm(2026, 11, 5, 15, 30));
    assert(ParseRfc3339(L"2026-11-05T15:30:00.5+01:00", u) && u == Gm(2026, 11, 5, 14, 30));
    assert(!ParseRfc3339(L"2026-11-05 15:30", u) && !ParseRfc3339(L"2026-11-05T15:30:00+0600", u));
    int dk = 0, mn = 0;
    UtcToLocal(Gm(2026, 11, 5, 21, 30), dk, mn);
    time_t tt = (time_t)Gm(2026, 11, 5, 21, 30);
    tm lt = {};
    localtime_s(&lt, &tt);
    assert(dk == TmKey(lt) && mn == lt.tm_hour * 60 + lt.tm_min);
    assert(LocalIso(20261105, 930) == L"2026-11-05T15:30:00" && LocalIso(20261105, 1440) == L"2026-11-06T00:00:00");
    assert(UtcIso(Gm(2026, 11, 5, 21, 30)) == L"2026-11-05T21:30:00Z");
    assert(ParseRfc3339(LocalIsoWithOffset(20261105, 930), u) && u == LocalEpoch(2026, 11, 5, 15, 30));
    assert(LocalIsoWithOffset(20261105, 1440).substr(0, 19) == L"2026-11-06T00:00:00");
    assert(!WindowsZone().empty());

    // Google -> event

    CalEvent e;
    assert(JsonParse(R"({"id":"g1","summary":"Reunión","status":"confirmed","start":{"dateTime":"2026-11-05T21:30:00Z"},"end":{"dateTime":"2026-11-05T22:15:00Z"},"colorId":"11","reminders":{"useDefault":false,"overrides":[{"method":"popup","minutes":45},{"method":"popup","minutes":5}]}})", j));
    assert(GoogleToEvent(j, e));
    int d = 0, m = 0, ed = 0, em = 0;
    UtcToLocal(Gm(2026, 11, 5, 21, 30), d, m);
    UtcToLocal(Gm(2026, 11, 5, 22, 15), ed, em);
    assert(e.date == d && e.minute == m && e.end == (ed != d ? 1440 : em) && e.title == L"Reunión");
    assert(e.color == 3 && e.alert == 10 && e.src == 1 && e.rid == L"g1" && e.dirty == 0 && e.repeat == 0);
    assert(JsonParse(R"({"id":"g2","summary":"","start":{"date":"2026-11-05"},"end":{"date":"2026-11-06"},"colorId":"5"})", j) && GoogleToEvent(j, e));
    assert(e.date == 20261105 && e.minute == -1 && e.end == -1 && e.title == L"(Sin título)" && e.color == 4 && e.alert == -1);
    assert(JsonParse(R"({"id":"g3","status":"cancelled","start":{"date":"2026-11-05"}})", j) && !GoogleToEvent(j, e));

    // Outlook -> event
    assert(JsonParse(R"({"id":"o1","subject":"Café","isCancelled":false,"isAllDay":false,"isReminderOn":true,"reminderMinutesBeforeStart":30,"body":{"content":"linea1\nlinea2"},"start":{"dateTime":"2026-11-05T15:30:00.0000000","timeZone":"UTC"},"end":{"dateTime":"2026-11-05T16:00:00.0000000","timeZone":"UTC"}})", j));
    assert(GraphToEvent(j, e));
    UtcToLocal(Gm(2026, 11, 5, 15, 30), d, m);
    assert(e.date == d && e.minute == m && e.src == 2 && e.alert == 30 && e.notes == L"linea1 linea2" && e.title == L"Café" && e.color == 0);
    assert(JsonParse(R"({"id":"o2","subject":"Festivo","isCancelled":false,"isAllDay":true,"isReminderOn":false,"start":{"dateTime":"2026-11-05T00:00:00.0000000"},"end":{"dateTime":"2026-11-06T00:00:00.0000000"}})", j) && GraphToEvent(j, e));
    assert(e.date == 20261105 && e.minute == -1 && e.alert == -1);
    assert(JsonParse(R"({"id":"o3","isCancelled":true,"start":{"dateTime":"2026-11-05T00:00:00"}})", j) && !GraphToEvent(j, e));

    // event -> Google body
    CalEvent g{ 20261105, 930, 990, 3, 2, L"Reunión", 30, L"notas" };
    assert(JsonParse(GoogleBody(g, true, true), j));
    assert(j["summary"].str() == L"Reunión" && j["description"].str() == L"notas" && j["colorId"].str() == L"11");
    assert(j["start"]["dateTime"].str() == LocalIsoWithOffset(20261105, 930) && j["end"]["dateTime"].str() == LocalIsoWithOffset(20261105, 990));
    assert(j["reminders"]["overrides"][0]["minutes"].n == 30 && j["recurrence"][0].str() == L"RRULE:FREQ=WEEKLY");
    std::wstring zone = IanaZone();
    assert(zone.empty() ? j["start"]["timeZone"].t == Json::Null : j["start"]["timeZone"].str() == zone);
    assert(JsonParse(GoogleBody(g, false, false), j) && j["description"].t == Json::Null && j["recurrence"].t == Json::Null);
    CalEvent ad{ 20261105, -1, -1, 0, 0, L"Todo", -1, L"" };
    assert(JsonParse(GoogleBody(ad, true, false), j));
    assert(j["start"]["date"].str() == L"2026-11-05" && j["end"]["date"].str() == L"2026-11-06");
    assert(j["reminders"]["overrides"].a.empty() && j["colorId"].t == Json::Null);

    // event -> Graph body
    CalEvent wc{ 20261103, 540, 600, 0, 2, L"Clase", 10, L"x" };
    assert(JsonParse(GraphBody(wc, true, true), j));
    assert(j["subject"].str() == L"Clase" && j["body"]["content"].str() == L"x" && j["body"]["contentType"].str() == L"text");
    assert(j["start"]["dateTime"].str() == L"2026-11-03T09:00:00" && j["end"]["dateTime"].str() == L"2026-11-03T10:00:00");
    assert(j["start"]["timeZone"].str() == WindowsZone() && j["isAllDay"].b == false && j["isReminderOn"].b && j["reminderMinutesBeforeStart"].n == 10);
    assert(j["recurrence"]["pattern"]["type"].str() == L"weekly" && j["recurrence"]["pattern"]["daysOfWeek"][0].str() == L"tuesday");
    assert(j["recurrence"]["range"]["startDate"].str() == L"2026-11-03" && j["recurrence"]["range"]["type"].str() == L"noEnd");
    CalEvent y{ 20261105, -1, -1, 0, 4, L"Aniv", -1, L"" };
    assert(JsonParse(GraphBody(y, false, true), j));
    assert(j["isAllDay"].b && j["start"]["dateTime"].str() == L"2026-11-05T00:00:00" && j["end"]["dateTime"].str() == L"2026-11-06T00:00:00");
    assert(j["recurrence"]["pattern"]["type"].str() == L"absoluteYearly" && j["recurrence"]["pattern"]["month"].n == 11 && j["recurrence"]["pattern"]["dayOfMonth"].n == 5);
    assert(j["body"].t == Json::Null);
    assert(JsonParse(GraphBody(wc, false, false), j) && j["recurrence"].t == Json::Null && j["body"].t == Json::Null);

    // push results
    std::vector<CalEvent> ev = { Remote(L"", 20261105, 1, 1, 1) };
    ApplyPush(ev, { PushResult{ 1, 1, 1, true, 200, L"new1" } }); // create ok
    assert(ev.size() == 1 && ev[0].rid == L"new1" && ev[0].dirty == 0);
    ev = { Remote(L"", 20261105, 1, 1, 2) };
    ev[0].rev = 2;
    ApplyPush(ev, { PushResult{ 2, 1, 1, true, 200, L"new2" } }); // edited while the create was in flight
    assert(ev[0].rid == L"new2" && ev[0].dirty == 2);
    ev = { Remote(L"k", 20261105, 1, 2, 3) };
    ApplyPush(ev, { PushResult{ 3, 1, 2, false, 500, L"" } }); // server error: retry next sync
    assert(ev.size() == 1 && ev[0].dirty == 2);
    ApplyPush(ev, { PushResult{ 3, 1, 2, false, 0, L"" } }); // no network
    assert(ev.size() == 1 && ev[0].dirty == 2);
    ev = { Remote(L"k", 20261105, 1, 2, 4) };
    ApplyPush(ev, { PushResult{ 4, 1, 2, false, 404, L"" } }); // deleted in the service too
    assert(ev.empty());
    ev = { Remote(L"k", 20261105, 1, 4, 5) };
    ApplyPush(ev, { PushResult{ 5, 1, 4, true, 204, L"" } }); // delete ok
    assert(ev.empty());
    ev = { Remote(L"k", 20261105, 1, 2, 6) };
    ApplyPush(ev, { PushResult{ 6, 1, 2, false, 403, L"" } }); // rejected: clear the bits
    assert(ev.size() == 1 && ev[0].dirty == 0);
    ev = { Remote(L"k", 20261105, 1, 2, 7) };
    ev[0].rev = 3;
    ApplyPush(ev, { PushResult{ 7, 2, 2, true, 200, L"" } }); // an older send: newer edit still pending
    assert(ev[0].dirty == 2);
    ev = { Remote(L"", 20261105, 1, 1, 8) };
    ApplyPush(ev, { PushResult{ 8, 1, 1, false, 400, L"" } }); // create refused: becomes a Capri event, not lost
    assert(ev.size() == 1 && ev[0].src == 0 && ev[0].dirty == 0);
    MergePull(ev, 1, 20261101, 20261130, {});
    assert(ev.size() == 1); // and a later pull does not drop it
    ev = { Remote(L"k", 20261105, 1, 2, 9) };
    ApplyPush(ev, { PushResult{ 9, 1, 2, false, 429, L"" } }); // rate limited: retry next sync
    assert(ev[0].dirty == 2);

    // merge a pull
    std::vector<CalEvent> pm = {
        Remote(L"a", 20261105, 1, 0, 1),  // clean, inside the window: replaced
        Remote(L"b", 20261105, 1, 2, 2),  // local change pending: kept
        Remote(L"c", 20270301, 1, 0, 3),  // clean, outside the window: kept
        Remote(L"x", 20261106, 1, 0, 4),  // gone from the service: removed
        Remote(L"m", 20270301, 1, 0, 5),  // moved into the window: not shown twice
        Remote(L"", 20261105, 0, 0, 6),   // local Capri event: kept
    };
    MergePull(pm, 1, 20261101, 20261231, { Remote(L"a", 20261107, 1, 0, 0), Remote(L"b", 20261108, 1, 0, 0),
                                           Remote(L"d", 20261109, 1, 0, 0), Remote(L"m", 20261106, 1, 0, 0) });
    assert(pm.size() == 6 && std::is_sorted(pm.begin(), pm.end(), EventManager::Before));
    assert(CountRid(pm, L"a") == 1 && CountRid(pm, L"x") == 0 && CountRid(pm, L"m") == 1 && CountRid(pm, L"d") == 1);
    for (const CalEvent& x : pm) {
        if (x.rid == L"a") assert(x.date == 20261107 && x.uid != 0);
        if (x.rid == L"b") assert(x.date == 20261105 && x.dirty == 2);
        if (x.rid == L"m") assert(x.date == 20261106);
    }
    std::vector<CalEvent> om = { Remote(L"o1", 20261105, 2, 0, 7) };
    om[0].color = 4;
    MergePull(om, 2, 20261101, 20261231, { Remote(L"o1", 20261105, 2, 0, 0) });
    assert(om.size() == 1 && om[0].color == 4 && om[0].src == 2);

    // ini: comments, spaces and CRLF; a missing key is empty
    SyncConfig cfg = ParseSyncConfig(L"; microsoft_client_id=nope\r\n# microsoft_client_id=nope\r\nmicrosoft_client_id = abc123 \r\n");
    assert(cfg.microsoftId == L"abc123");
    assert(ParseSyncConfig(L"").microsoftId.empty());

    // the template is written to %TEMP% once and never overwrites the user's IDs
    std::wstring ini = std::wstring(tmp) + L"capri_sync_test.ini";
    DeleteFileW(ini.c_str());
    assert(CreateSyncIni(ini));
    std::wstring text;
    assert(ReadUtf8File(ini, text) && text.find(L"microsoft_client_id=") != std::wstring::npos && text.find(L"google") == std::wstring::npos);
    assert(text.find(L"README") != std::wstring::npos && ParseSyncConfig(text).microsoftId.empty());
    assert(WriteBytesFile(ini, Utf8(L"microsoft_client_id=mine\n")) && CreateSyncIni(ini));
    assert(ReadUtf8File(ini, text) && ParseSyncConfig(text).microsoftId == L"mine");
    DeleteFileW(ini.c_str());

    // token file: "refresh\naccount" under DPAPI; the account is written as UTF-8
    std::wstring tokenPath = std::wstring(tmp) + L"capri_sync_test_account.bin";
    assert(SaveAccountToken(tokenPath, "1//refresh-ñ", L"ana@ejemplo.com"));
    std::string rawToken, rt;
    std::wstring acct;
    assert(ReadBytesFile(tokenPath, rawToken) && rawToken.find("ana@") == std::string::npos);
    assert(LoadAccountToken(tokenPath, rt, acct) && rt == "1//refresh-ñ" && acct == L"ana@ejemplo.com");
    assert(ProtectToFile(tokenPath, "solo-token") && LoadAccountToken(tokenPath, rt, acct) == false); // no account line: refused
    DeleteFileW(tokenPath.c_str());
    assert(!LoadAccountToken(tokenPath, rt, acct));

    // Google bridge: URL rules, redirect hosts, the script, the request and answer shapes (no network here)
    assert(IsBridgeUrl(L"https://script.google.com/macros/s/AKfycbx-Q_9/exec"));
    assert(!IsBridgeUrl(L"http://script.google.com/macros/s/AKfy/exec") && !IsBridgeUrl(L"https://script.google.com/macros/s/AKfy/dev"));
    assert(!IsBridgeUrl(L"https://script.google.com.evil.com/macros/s/AKfy/exec") && !IsBridgeUrl(L"https://evil.com/macros/s/AKfy/exec"));
    assert(!IsBridgeUrl(L"") && !IsBridgeUrl(L"https://script.google.com/macros/s//exec") && !IsBridgeUrl(L"https://script.google.com/macros/s/a b/exec"));
    assert(BridgeRedirectHost(L"script.google.com") && BridgeRedirectHost(L"script.googleusercontent.com"));
    assert(!BridgeRedirectHost(L"xgoogleusercontent.com") && !BridgeRedirectHost(L"evil.com") && !BridgeRedirectHost(L"googleusercontent.com.evil.com"));
    std::wstring sc = BridgeScript("tok_ABC-123");
    assert(sc.find(L"tok_ABC-123") != std::wstring::npos && sc.find(L"%%TOKEN%%") == std::wstring::npos && sc.find(L"\r\n") != std::wstring::npos);
    assert(JsonParse(BridgeBody("tok", "patch", "\"id\":\"ev1\",\"body\":{\"summary\":\"x\"}"), j));
    assert(j["token"].str() == L"tok" && j["action"].str() == L"patch" && j["id"].str() == L"ev1" && j["body"]["summary"].str() == L"x");
    assert(JsonParse(BridgeBody("t", "list", "\"params\":{\"maxResults\":5}"), j) && j["action"].str() == L"list" && j["params"]["maxResults"].n == 5);
    assert(JsonParse(BridgeBody("t", "remove", ""), j) && j["action"].str() == L"remove" && j["id"].t == Json::Null);
    DWORD bcode = 99;
    Json bdata;
    assert(BridgeReply(R"({"ok":true,"status":200,"data":{"items":[1]}})", bdata, bcode) && bcode == 200 && bdata["items"][0].n == 1);
    assert(BridgeReply(R"({"ok":false,"status":404,"error":"x"})", bdata, bcode) && bcode == 404);
    assert(BridgeReply(R"({"ok":false,"status":401})", bdata, bcode) && bcode == 401);
    assert(BridgeReply(R"({"ok":false,"status":-5})", bdata, bcode) && bcode == 0);
    assert(!BridgeReply("<html>Inicia sesión</html>", bdata, bcode) && bcode == 0); // a web page is never a status
    assert(!BridgeReply("", bdata, bcode) && bcode == 0);

    // bridge file: the URL is refused unless it passes IsBridgeUrl; the old OAuth format is refused
    std::wstring bridgePath = std::wstring(tmp) + L"capri_sync_test_bridge.bin";
    std::wstring bu, bacct = L"x";
    std::string btok;
    assert(SaveBridgeFile(bridgePath, L"https://script.google.com/macros/s/AKfy/exec", "tok-1", L"ana@ejemplo.com"));
    assert(ReadBytesFile(bridgePath, rawToken) && rawToken.find("ana@") == std::string::npos && rawToken.find("AKfy") == std::string::npos);
    assert(LoadBridgeFile(bridgePath, bu, btok, bacct) && bu == L"https://script.google.com/macros/s/AKfy/exec" && btok == "tok-1" && bacct == L"ana@ejemplo.com");
    assert(!SaveBridgeFile(bridgePath, L"http://evil.com/exec", "tok-2", L"") && LoadBridgeFile(bridgePath, bu, btok, bacct) && btok == "tok-1");
    assert(SaveBridgeFile(bridgePath, L"", "tok-2", L"") && LoadBridgeFile(bridgePath, bu, btok, bacct) && bu.empty() && btok == "tok-2");
    assert(ProtectToFile(bridgePath, "1//refresh-old\nana@ejemplo.com") && !LoadBridgeFile(bridgePath, bu, btok, bacct));
    assert(ProtectToFile(bridgePath, "https://evil.com/macros/s/x/exec\ntok\n") && !LoadBridgeFile(bridgePath, bu, btok, bacct) && bu.empty());
    SyncAccount sa;
    sa.tokenFile = bridgePath; sa.bridgeUrl = L"https://script.google.com/macros/s/AKfy/exec"; sa.bridgeToken = "tok-3"; sa.account = L"ana@ejemplo.com";
    assert(SaveBridgeAccount(sa) && LoadBridgeFile(bridgePath, bu, btok, bacct) && bu.empty()); // not connected yet: no URL on disk
    sa.connected = true;
    assert(SaveBridgeAccount(sa) && LoadBridgeFile(bridgePath, bu, btok, bacct) && bu == sa.bridgeUrl && btok == "tok-3");
    DeleteFileW(bridgePath.c_str());

    // calendar link file: DPAPI-protected, checked on load (https only; webcal is converted)
    std::wstring icsPath = std::wstring(tmp) + L"capri_sync_test_ics.bin", link;
    assert(SaveIcsFile(icsPath, L"https://outlook.live.com/calendar/x.ics") && ReadBytesFile(icsPath, rawToken));
    assert(rawToken.find("outlook") == std::string::npos && LoadIcsFile(icsPath, link) && link == L"https://outlook.live.com/calendar/x.ics");
    assert(SaveIcsFile(icsPath, L"webcal://calendar.google.com/a.ics") && LoadIcsFile(icsPath, link) && link == L"https://calendar.google.com/a.ics");
    assert(SaveIcsFile(icsPath, L"http://evil.com/x.ics") && !LoadIcsFile(icsPath, link));
    assert(ProtectToFile(icsPath, "1//refresh-old\nana@ejemplo.com") && !LoadIcsFile(icsPath, link));
    DeleteFileW(icsPath.c_str());

    // a link event keeps its local colour when the link is read again (its own copy comes back with the new title)
    std::vector<CalEvent> lk = { Remote(L"u1", 20261110, 3, 0, 900) };
    lk[0].color = 4;
    std::vector<CalEvent> lkNew = { Remote(L"u1", 20261110, 3, 0, 0) };
    lkNew[0].title = L"Feria v2";
    MergePull(lk, 3, 20261101, 20261130, lkNew);
    assert(lk.size() == 1 && lk[0].src == 3 && lk[0].color == 4 && lk[0].title == L"Feria v2");

    // series: a created one takes its id from its push; a deleted one still hides its occurrences but is not re-added
    std::vector<CalEvent> series = { Remote(L"", 20261105, 1, 1, 10), Remote(L"keep", 20261110, 1, 2, 11),
                                     Remote(L"gone", 20261111, 1, 4, 12), Remote(L"", 20261112, 1, 1, 13) };
    series[0].repeat = 2; series[1].repeat = 1; series[2].repeat = 3; series[3].repeat = 1;
    std::set<std::wstring> masters;
    std::vector<CalEvent> copies;
    SeriesCopies(series, { PushResult{ 10, 1, 1, true, 200, L"M10" }, PushResult{ 13, 1, 1, false, 400, L"" } }, masters, copies);
    assert(masters.size() == 3 && masters.count(L"M10") && masters.count(L"keep") && masters.count(L"gone"));
    assert(copies.size() == 2 && copies[0].rid == L"M10" && copies[1].rid == L"keep");
    assert(copies[0].dirty == 0 && copies[1].dirty == 0 && copies[0].rev == 0);
    assert(OpOf(series[0]) == 1 && OpOf(series[1]) == 2 && OpOf(series[2]) == 4);
    assert(LocalToUtc(20261105, 930) == LocalEpoch(2026, 11, 5, 15, 30));

    std::printf("sync_test ok\n");
    return 0;
}
