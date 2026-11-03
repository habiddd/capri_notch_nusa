#pragma once
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "calutil.h"
#include "events.h"
#include "ics.h"
#include "json.h"
#include "net.h"
#include "oauth.h"

// Google Calendar (through the user's Apps Script bridge), Microsoft Graph and a calendar link (ICS): time conversion, JSON mapping and
// merging results are pure. The worker at the end (RunSync, IcsBody) is the only code that talks to the network; it runs on its own
// thread and never touches UI state.

#define WM_SYNC_DONE (WM_USER + 103) // wParam: source (1 Google, 2 Outlook, 3 link); lParam: SyncResult*, deleted by the UI thread

// ---- time

// UTC from "2026-11-05T15:30:00Z", "...-06:00", "...15:30:00.0000000"; no zone means UTC.
inline bool ParseRfc3339(const std::wstring& s, long long& utc) {
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, sec = 0, pos = 0, oh = 0, om = 0, n = 0;
    if (swscanf(s.c_str(), L"%d-%d-%dT%d:%d:%d%n", &Y, &M, &D, &h, &mi, &sec, &pos) != 6) return false;
    tm t = {};
    t.tm_year = Y - 1900;
    t.tm_mon = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = sec;
    size_t p = pos;
    if (p < s.size() && s[p] == L'.') { p++; while (p < s.size() && iswdigit(s[p])) p++; }
    int sign = 1;
    if (p < s.size() && s[p] == L'Z') p++;
    else if (p < s.size() && (s[p] == L'+' || s[p] == L'-')) {
        sign = s[p] == L'+' ? 1 : -1;
        if (swscanf(s.c_str() + p + 1, L"%d:%d%n", &oh, &om, &n) != 2) return false;
        p += 1 + n;
    }
    if (p != s.size()) return false;
    utc = _mkgmtime(&t) - sign * (oh * 60 + om) * 60LL;
    return true;
}

// Wall time of a date key plus minutes; minutes past 1440 roll into the next day. mktime normalizes t in place.
inline tm LocalTm(int dateKey, int minute) {
    tm t = {};
    t.tm_year = dateKey / 10000 - 1900;
    t.tm_mon = dateKey / 100 % 100 - 1;
    t.tm_mday = dateKey % 100;
    t.tm_min = minute;
    t.tm_isdst = -1;
    mktime(&t);
    return t;
}

inline std::wstring LocalIso(int dateKey, int minute) {
    tm t = LocalTm(dateKey, minute);
    wchar_t buf[32];
    wsprintfW(buf, L"%04d-%02d-%02dT%02d:%02d:00", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    return buf;
}

inline std::wstring DateStr(int dateKey) { return LocalIso(dateKey, 0).substr(0, 10); }

// Offset of that wall time on that date (DST included): mktime gives the local instant, _mkgmtime the same fields as UTC.
inline std::wstring LocalIsoWithOffset(int dateKey, int minute) {
    tm t = LocalTm(dateKey, minute);
    long long local = mktime(&t);
    tm g = t;
    long long off = (_mkgmtime(&g) - local) / 60; // minutes east of UTC
    long long a = off < 0 ? -off : off;
    wchar_t buf[48];
    wsprintfW(buf, L"%04d-%02d-%02dT%02d:%02d:00%s%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
              off < 0 ? L"-" : L"+", (int)(a / 60), (int)(a % 60));
    return buf;
}

inline std::wstring UtcIso(long long utc) {
    time_t tt = (time_t)utc;
    tm g = {};
    gmtime_s(&g, &tt);
    wchar_t buf[32];
    wsprintfW(buf, L"%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return buf;
}

// UTC epoch of a local wall time; minute 1440 is midnight at the end of the date
inline long long LocalToUtc(int dateKey, int minute) {
    tm t = LocalTm(dateKey, minute);
    return mktime(&t);
}

// ---- shared mapping (UtcToLocal, zones, OneLine, AlertFromMinutes: calutil.h)

// Local start and end from two RFC 3339 instants. shortcut: a multi-day event shows only on its first day, upgrade if multi-day events matter
inline bool SetSpan(CalEvent& e, const std::wstring& start, const std::wstring& end) {
    long long s0 = 0, e0 = 0;
    if (!ParseRfc3339(start, s0)) return false;
    UtcToLocal(s0, e.date, e.minute);
    if (ParseRfc3339(end, e0)) {
        int ed = 0, em = 0;
        UtcToLocal(e0, ed, em);
        e.end = ed != e.date ? 1440 : (em > e.minute ? em : -1);
    }
    return true;
}

// ---- Google Calendar

// colorId -> category index; the default colour is category 0
inline int GoogleColorCat(const std::wstring& id) {
    static const std::pair<const wchar_t*, int> T[] = { { L"1", 1 }, { L"3", 1 }, { L"2", 2 }, { L"10", 2 },
                                                        { L"11", 3 }, { L"5", 4 }, { L"6", 4 }, { L"4", 5 } };
    for (const auto& x : T) if (id == x.first) return x.second;
    return 0;
}

inline std::wstring GoogleColorId(int cat) {
    static const wchar_t* ids[] = { L"", L"3", L"10", L"11", L"5", L"4" };
    return ids[cat];
}

inline bool GoogleToEvent(const Json& item, CalEvent& e) {
    if (item["status"].str() == L"cancelled" || item["id"].str().empty()) return false;
    e = CalEvent{ 0, -1, -1, 0, 0, L"" };
    std::wstring title = OneLine(item["summary"].str());
    e.title = title.empty() ? L"(Sin título)" : title;
    e.notes = OneLine(item["description"].str());
    e.color = GoogleColorCat(item["colorId"].str());
    e.src = 1;
    e.rid = item["id"].str();
    const Json& st = item["start"];
    if (!st["date"].str().empty()) {
        int Y = 0, M = 0, D = 0;
        if (swscanf(st["date"].str().c_str(), L"%d-%d-%d", &Y, &M, &D) != 3) return false;
        e.date = Y * 10000 + M * 100 + D;
    } else if (!SetSpan(e, st["dateTime"].str(), item["end"]["dateTime"].str())) {
        return false;
    }
    const Json& rem = item["reminders"];
    int first = -1; // the earliest override, mapped
    for (const Json& o : rem["overrides"].a) {
        int m = AlertFromMinutes((int)o["minutes"].n);
        if (first < 0 || m < first) first = m;
    }
    e.alert = rem["useDefault"].b ? 10 : first;
    return true;
}

// Create sends everything; update leaves out what the user did not change (includeNotes false keeps line breaks in the service's copy).
// shortcut: an update that clears the colour (back to General) does not reach Google, which keeps the old colour
inline std::string GoogleBody(const CalEvent& e, bool includeNotes, bool includeRecurrence) {
    static const char* FREQ[] = { "", "DAILY", "WEEKLY", "MONTHLY", "YEARLY" };
    std::string b = "{\"summary\":" + JsonQuote(e.title);
    if (includeNotes) b += ",\"description\":" + JsonQuote(e.notes);
    if (e.color) b += ",\"colorId\":" + JsonQuote(GoogleColorId(e.color));
    std::wstring zone = IanaZone();
    std::string tz = zone.empty() ? std::string() : ",\"timeZone\":" + JsonQuote(zone);
    if (e.minute < 0) { // all day: Google's end date is exclusive
        b += ",\"start\":{\"date\":" + JsonQuote(DateStr(e.date)) + "},\"end\":{\"date\":" + JsonQuote(DateStr(TmKey(LocalTm(e.date, 1440)))) + "}";
    } else {
        int endMin = e.end >= 0 ? e.end : e.minute + 60;
        b += ",\"start\":{\"dateTime\":" + JsonQuote(LocalIsoWithOffset(e.date, e.minute)) + tz +
             "},\"end\":{\"dateTime\":" + JsonQuote(LocalIsoWithOffset(e.date, endMin)) + tz + "}";
    }
    b += ",\"reminders\":{\"useDefault\":false,\"overrides\":[";
    if (e.alert >= 0) b += "{\"method\":\"popup\",\"minutes\":" + std::to_string(e.alert) + "}";
    b += "]}";
    if (includeRecurrence && e.repeat) b += std::string(",\"recurrence\":[\"RRULE:FREQ=") + FREQ[e.repeat] + "\"]";
    return b + "}";
}

// The script the user pastes into script.google.com. It runs as the user, so it reaches only their own calendar, and it refuses
// any caller without the key. Its answers are the Calendar API's own JSON, which is why the mapping above works unchanged.
inline constexpr const char* kBridgeScript = R"JS(// Capri Notch: puente con tu Google Calendar. No compartas esta URL ni este código.
const TOKEN = '%%TOKEN%%';
function doPost(e) {
  let req;
  try { req = JSON.parse(e.postData.contents); } catch (err) { return out({ ok: false, status: 400, error: 'json' }); }
  if (!req || req.token !== TOKEN) return out({ ok: false, status: 401, error: 'auth' });
  try {
    switch (req.action) {
      case 'list':   return out({ ok: true, status: 200, data: Calendar.Events.list('primary', req.params) });
      case 'insert': return out({ ok: true, status: 200, data: Calendar.Events.insert(req.body, 'primary') });
      case 'patch':  return out({ ok: true, status: 200, data: Calendar.Events.patch(req.body, 'primary', req.id) });
      case 'remove': Calendar.Events.remove('primary', req.id); return out({ ok: true, status: 204 });
    }
    return out({ ok: false, status: 400, error: 'action' });
  } catch (err) {
    const m = String((err && err.message) || err);
    const status = /not found|404/i.test(m) ? 404 : /deleted|410/i.test(m) ? 410 : /rate|quota|429/i.test(m) ? 429
                 : /backend|internal|500|503/i.test(m) ? 500 : 400;
    return out({ ok: false, status: status, error: m });
  }
}
function out(o) { return ContentService.createTextOutput(JSON.stringify(o)).setMimeType(ContentService.MimeType.JSON); }
)JS";

// The script with this key in it, CRLF line ends for the clipboard.
inline std::wstring BridgeScript(const std::string& token) {
    std::string s = kBridgeScript;
    s.replace(s.find("%%TOKEN%%"), 9, token);
    for (size_t p = 0; (p = s.find('\n', p)) != std::string::npos; p += 2) s.insert(p, 1, '\r');
    return Wide(s);
}

// ---- Microsoft Graph (Outlook)

inline bool GraphToEvent(const Json& item, CalEvent& e) {
    if (item["isCancelled"].b || item["id"].str().empty()) return false;
    e = CalEvent{ 0, -1, -1, 0, 0, L"" };
    std::wstring subject = OneLine(item["subject"].str());
    e.title = subject.empty() ? L"(Sin título)" : subject;
    e.notes = OneLine(item["body"]["content"].str());
    e.src = 2;
    e.rid = item["id"].str();
    e.alert = item["isReminderOn"].b ? AlertFromMinutes((int)item["reminderMinutesBeforeStart"].n) : -1;
    std::wstring start = item["start"]["dateTime"].str();
    if (item["isAllDay"].b) { // the date as Graph wrote it (we ask for UTC)
        int Y = 0, M = 0, D = 0;
        if (swscanf(start.c_str(), L"%d-%d-%d", &Y, &M, &D) != 3) return false;
        e.date = Y * 10000 + M * 100 + D;
    } else if (!SetSpan(e, start, item["end"]["dateTime"].str())) {
        return false;
    }
    return true;
}

// includeNotes is false on update unless the notes changed (see dirty bit 8): the service's HTML body is kept otherwise
inline std::string GraphBody(const CalEvent& e, bool includeNotes, bool includeRecurrence) {
    static const char* DAYS[] = { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday" };
    bool allDay = e.minute < 0;
    std::string tz = JsonQuote(WindowsZone());
    int startMin = allDay ? 0 : e.minute;
    int endMin = allDay ? 1440 : e.end >= 0 ? e.end : e.minute + 60;
    std::string b = "{\"subject\":" + JsonQuote(e.title);
    if (includeNotes) b += ",\"body\":{\"contentType\":\"text\",\"content\":" + JsonQuote(e.notes) + "}";
    b += ",\"start\":{\"dateTime\":" + JsonQuote(LocalIso(e.date, startMin)) + ",\"timeZone\":" + tz + "}";
    b += ",\"end\":{\"dateTime\":" + JsonQuote(LocalIso(e.date, endMin)) + ",\"timeZone\":" + tz + "}";
    b += std::string(",\"isAllDay\":") + (allDay ? "true" : "false");
    b += std::string(",\"isReminderOn\":") + (e.alert >= 0 ? "true" : "false");
    b += ",\"reminderMinutesBeforeStart\":" + std::to_string(e.alert < 0 ? 0 : e.alert);
    if (includeRecurrence && e.repeat) {
        int dom = e.date % 100, mon = e.date / 100 % 100, wd = LocalTm(e.date, 0).tm_wday;
        std::string pattern = e.repeat == 1 ? std::string("\"type\":\"daily\",\"interval\":1")
                            : e.repeat == 2 ? std::string("\"type\":\"weekly\",\"interval\":1,\"daysOfWeek\":[\"") + DAYS[wd] + "\"]"
                            : e.repeat == 3 ? "\"type\":\"absoluteMonthly\",\"interval\":1,\"dayOfMonth\":" + std::to_string(dom)
                            : "\"type\":\"absoluteYearly\",\"interval\":1,\"dayOfMonth\":" + std::to_string(dom) + ",\"month\":" + std::to_string(mon);
        b += ",\"recurrence\":{\"pattern\":{" + pattern + "},\"range\":{\"type\":\"noEnd\",\"startDate\":" + JsonQuote(DateStr(e.date)) + "}}";
    }
    return b + "}";
}

// ---- merging results (UI thread)

struct PushResult { long long uid; int rev; int op; bool ok; int httpCode; std::wstring newRid; }; // op: 1 create, 2 update, 4 delete

// Applies what the service answered for each pushed event. Network, sign-in and 5xx failures leave the event alone (next sync retries).
inline void ApplyPush(std::vector<CalEvent>& ev, const std::vector<PushResult>& results) {
    for (const PushResult& r : results) {
        auto it = std::find_if(ev.begin(), ev.end(), [&](const CalEvent& e) { return e.uid == r.uid; });
        if (it == ev.end()) continue;
        bool gone = r.httpCode == 404 || r.httpCode == 410;
        if (!r.ok && (r.httpCode == 0 || r.httpCode == 401 || r.httpCode == 429 || r.httpCode >= 500)) continue;
        if (!r.ok && r.op == 1) { // the service refused it: keep it as a Capri event, or the next pull would drop it
            it->src = 0;
            it->dirty = 0;
            continue;
        }
        if (r.op == 4 ? (r.ok || gone) : (gone && r.op == 2)) { ev.erase(it); continue; } // gone in the service too
        if (r.ok && r.op == 1) it->rid = r.newRid;
        if (!r.ok || r.rev == it->rev) it->dirty = 0; // rejected, or nothing edited since the send
        else if (r.op == 1) it->dirty = (it->dirty & ~1) | 2; // created, but edited meanwhile: it needs an update now
    }
}

// Replaces this service's clean copies inside [fromKey, toKey] with `fetched`. Local changes not yet uploaded win.
inline void MergePull(std::vector<CalEvent>& ev, int src, int fromKey, int toKey, std::vector<CalEvent> fetched) {
    std::map<std::wstring, const CalEvent*> local; // by remote id; the pointers die with the erase below
    for (const CalEvent& e : ev) if (e.src == src && !e.rid.empty()) local[e.rid] = &e;
    std::vector<CalEvent> add;
    std::set<std::wstring> got;
    for (CalEvent f : fetched) {
        auto it = local.find(f.rid);
        if (it != local.end()) {
            if (it->second->dirty) continue;
            if (src != 1) f.color = it->second->color; // Outlook categories and link events have no colour of ours: keep the local one
        }
        f.src = src;
        f.uid = ++g_eventUid;
        got.insert(f.rid);
        add.push_back(f);
    }
    // a clean copy that moved into the window from outside it is replaced too, so it is not shown twice
    ev.erase(std::remove_if(ev.begin(), ev.end(), [&](const CalEvent& e) {
        return e.src == src && e.dirty == 0 && ((e.date >= fromKey && e.date <= toKey) || got.count(e.rid));
    }), ev.end());
    ev.insert(ev.end(), add.begin(), add.end());
    std::stable_sort(ev.begin(), ev.end(), EventManager::Before);
}

// ---- one calendar service as the UI sees it (UI thread only)

struct SyncAccount {
    int src = 0;                  // 1 Google (Apps Script bridge), 2 Outlook, 3 calendar link (ICS, read-only)
    OAuthProvider prov;           // Outlook only
    std::wstring tokenFile;       // %LOCALAPPDATA%\capri_sync_google.bin or capri_sync_outlook.bin
    bool connected = false, syncing = false, again = false; // again: run one more sync when this one ends
    int gen = 0;                  // bumped on disconnect: results from an older connection are dropped
    std::wstring account, error;
    ULONGLONG lastSync = 0;       // GetTickCount64 of the last sync that got through; 0 = never
    std::string refresh, access;  // Outlook only; access is "" until refreshed
    long long expiresAt = 0;      // Outlook only; GetTickCount64 ms
    std::wstring bridgeUrl;       // Google: the web app URL; in the file only once a connection worked
    std::string bridgeToken;      // Google: the key the script checks, kept so a copied script still matches
    std::wstring link;            // link: the ICS address (a secret); in the file only once a connection worked
};

// The file keeps the bridge URL only while connected, so a failed attempt is not remembered after a restart
inline bool SaveBridgeAccount(const SyncAccount& a) {
    return SaveBridgeFile(a.tokenFile, a.connected ? a.bridgeUrl : std::wstring(), a.bridgeToken, a.account);
}

// capri_sync_ics.bin: DPAPI-protected address, nothing else. The address is checked again on load, as the bridge file's is.
inline bool SaveIcsFile(const std::wstring& path, const std::wstring& link) { return ProtectToFile(path, Utf8(link)); }
inline bool LoadIcsFile(const std::wstring& path, std::wstring& link) {
    std::string text;
    if (!UnprotectFromFile(path, text)) return false;
    std::wstring l = Wide(text);
    if (!NormalizeIcsLink(l)) return false;
    link = l;
    return true;
}

// What the worker gets: copies only, never the event list itself
struct SyncJob {
    HWND hwnd = NULL;
    int src = 0, gen = 0, fromKey = 0, toKey = 0;
    bool login = false;
    OAuthProvider prov;
    std::string refresh, access;
    long long expiresAt = 0;
    std::wstring account;
    std::wstring bridgeUrl;
    std::string bridgeToken;
    std::wstring link;
    std::vector<CalEvent> pending; // this service's local changes (dirty != 0)
    std::vector<CalEvent> series;  // this service's repeating events
};

struct SyncResult {
    int src = 0, gen = 0, fromKey = 0, toKey = 0;
    bool login = false, loggedIn = false, revoked = false, pulled = false;
    std::wstring error, account;
    std::string refresh, access;
    long long expiresAt = 0;
    std::vector<PushResult> pushes;
    std::vector<CalEvent> fetched;
};

// The call a pending event needs: 4 delete, 1 create, 2 update
inline int OpOf(const CalEvent& e) { return e.dirty & 4 ? 4 : e.dirty & 1 ? 1 : 2; }

// The service returns each occurrence of a series as its own event, so the local series stands in for them: the
// occurrences are skipped (`masters` gets the series' remote ids) and the series' clean copies are re-added (`copies`).
// A series created in this sync takes its id from its push result.
inline void SeriesCopies(const std::vector<CalEvent>& series, const std::vector<PushResult>& pushes,
                         std::set<std::wstring>& masters, std::vector<CalEvent>& copies) {
    for (CalEvent s : series) {
        for (const PushResult& p : pushes) if (p.op == 1 && p.ok && p.uid == s.uid) s.rid = p.newRid;
        if (s.rid.empty()) continue;
        masters.insert(s.rid);
        if (s.dirty & 4) continue; // deleted here: its occurrences go and nothing is re-added
        s.dirty = 0;
        s.rev = 0;
        copies.push_back(s);
    }
}

// ---- the worker (only on the sync thread: it blocks on the network)

// A failed call. 401 drops the access token so the next sync renews it.
inline std::wstring CallError(SyncJob& j, int code) {
    if (code == 401) { j.access.clear(); return j.src == 1 ? L"Clave del puente incorrecta" : L"Sesión caducada"; }
    if (code == 0) return L"Sin conexión";
    return L"Error " + std::to_wstring(code);
}

inline std::wstring AuthHeader(const SyncJob& j) {
    return L"Authorization: Bearer " + Wide(j.access) + L"\r\nContent-Type: application/json\r\n";
}

// The bridge's request: the key, the action, then its fields (JSON members without braces)
inline std::string BridgeBody(const std::string& token, const char* action, const std::string& fields) {
    return "{\"token\":" + JsonQuote(Wide(token)) + ",\"action\":\"" + action + "\"" + (fields.empty() ? std::string() : "," + fields) + "}";
}

// The bridge's answer {"ok","status","data","error"}: code is the Google status it reports, data its JSON. False when the answer is not
// JSON, e.g. the login page of a deployment that is not open to everyone: that must not read as a status (a 404 would drop events).
inline bool BridgeReply(const std::string& resp, Json& data, DWORD& code) {
    Json env;
    code = 0;
    if (!JsonParse(resp, env) || env.t != Json::Obj) return false;
    code = env["status"].n > 0 ? (DWORD)env["status"].n : 0;
    data = env["data"];
    return true;
}

// One call to the bridge (worker thread). False when it did not answer; otherwise code is the status it reports.
inline bool BridgeCall(const SyncJob& j, const char* action, const std::string& fields, Json& data, DWORD& code) {
    std::string resp;
    DWORD http = 0;
    code = 0;
    if (!IsBridgeUrl(j.bridgeUrl) || !HttpsPostFollow(j.bridgeUrl, BridgeBody(j.bridgeToken, action, fields), resp, http)) return false;
    return BridgeReply(resp, data, code);
}

// Sends one local change. False when the sync must stop (no network, expired sign-in, server error): the rest waits for the next sync.
inline bool SendOne(SyncJob& j, SyncResult& r, const CalEvent& e, int op) {
    PushResult pr{ e.uid, e.rev, op, false, 0, L"" };
    if (op == 4 && e.rid.empty()) {
        pr.ok = true; // never reached the service, so there is nothing to delete there
    } else {
        bool withNotes = op == 1 || (e.dirty & 8) != 0; // an update sends the notes only when they changed
        std::string resp;
        Json jr; // Google: the bridge's data; Outlook: the parsed answer
        DWORD code = 0;
        bool net;
        if (j.src == 1) {
            std::string fields = e.rid.empty() ? std::string() : "\"id\":" + JsonQuote(e.rid);
            if (op != 4) fields += (fields.empty() ? "" : ",") + std::string("\"body\":") + GoogleBody(e, withNotes, op == 1);
            net = BridgeCall(j, op == 1 ? "insert" : op == 2 ? "patch" : "remove", fields, jr, code);
        } else {
            const wchar_t* method = op == 1 ? L"POST" : op == 2 ? L"PATCH" : L"DELETE";
            std::wstring path = L"/v1.0/me/events";
            if (!e.rid.empty()) path += L"/" + UrlEncode(e.rid);
            std::string body = op == 4 ? std::string() : GraphBody(e, withNotes, op == 1);
            net = HttpsRequest(method, L"graph.microsoft.com", path, AuthHeader(j), body, resp, code);
        }
        pr.httpCode = net ? (int)code : 0;
        pr.ok = net && code / 100 == 2;
        if (pr.ok && op == 1) {
            if (j.src == 1) pr.newRid = jr["id"].str();
            else if (JsonParse(resp, jr)) pr.newRid = jr["id"].str();
        }
    }
    r.pushes.push_back(pr);
    bool transient = !pr.ok && (pr.httpCode == 0 || pr.httpCode == 401 || pr.httpCode == 429 || pr.httpCode >= 500);
    if (transient) r.error = CallError(j, pr.httpCode);
    return !transient; // other 4xx: the service refused it for good; ApplyPush clears it
}

// Every page of the window from this service. Occurrences of a series in `masters` are skipped (see SeriesCopies).
inline std::wstring PullWindow(SyncJob& j, const std::set<std::wstring>& masters, std::vector<CalEvent>& out) {
    const bool g = j.src == 1;
    const std::wstring host = L"graph.microsoft.com";
    const std::string parentKey = g ? "recurringEventId" : "seriesMasterId";
    const std::wstring from = UtcIso(LocalToUtc(j.fromKey, 0)), to = UtcIso(LocalToUtc(j.toKey, 1440));
    const std::wstring first = L"/v1.0/me/calendarView?startDateTime=" + UrlEncode(from) + L"&endDateTime=" + UrlEncode(to) +
                               L"&$top=500&$select=id,subject,body,start,end,isAllDay,isReminderOn,reminderMinutesBeforeStart,isCancelled,seriesMasterId";
    const std::wstring hdr = AuthHeader(j) + (g ? L"" : L"Prefer: outlook.timezone=\"UTC\", outlook.body-content-type=\"text\"\r\n");
    if (!g && j.account.empty()) { // Outlook has no account name in the calendar view, so it is asked once
        std::string me;
        DWORD code = 0;
        Json jm;
        if (HttpsRequest(L"GET", host, L"/v1.0/me?$select=mail,userPrincipalName", hdr, "", me, code) && code == 200 && JsonParse(me, jm))
            j.account = jm["mail"].str().empty() ? jm["userPrincipalName"].str() : jm["mail"].str();
    }
    std::wstring path = first, pageToken;
    for (int page = 0; page < 20; ++page) { // shortcut: 20 pages (50,000 Google events), then the sync fails instead of dropping the rest, upgrade if a calendar is bigger
        std::string resp;
        DWORD code = 0;
        Json jr;
        if (g) { // the bridge's data is the Calendar API's own answer; without "items" it is not a calendar, so nothing is merged
            std::string params = "\"params\":{\"timeMin\":" + JsonQuote(from) + ",\"timeMax\":" + JsonQuote(to) +
                                 ",\"singleEvents\":true,\"orderBy\":\"startTime\",\"maxResults\":2500" +
                                 (pageToken.empty() ? std::string() : ",\"pageToken\":" + JsonQuote(pageToken)) + "}";
            bool net = BridgeCall(j, "list", params, jr, code);
            if (!net || code != 200) return CallError(j, net ? (int)code : 0);
            if (jr["items"].t != Json::Arr) return L"Respuesta no válida";
        } else {
            bool net = HttpsRequest(L"GET", host, path, hdr, "", resp, code);
            if (!net || code != 200) return CallError(j, net ? (int)code : 0);
            if (!JsonParse(resp, jr)) return L"Respuesta no válida";
        }
        if (g && j.account.empty()) j.account = jr["summary"].str(); // on the primary calendar the name is the account
        for (const Json& item : jr[g ? "items" : "value"].a) {
            if (masters.count(item[parentKey.c_str()].str())) continue;
            CalEvent e;
            if (g ? GoogleToEvent(item, e) : GraphToEvent(item, e)) out.push_back(e);
        }
        std::wstring next = jr[g ? "nextPageToken" : "@odata.nextLink"].str();
        if (next.empty()) return L"";
        if (g) {
            pageToken = next;
        } else {
            std::wstring h;
            if (!SplitUrl(next, h, path) || h != host) return L"Respuesta no válida"; // a next page must stay on the Graph host
        }
    }
    return L"Demasiados eventos";
}

// The calendar link (src 3): one GET of the address, read as ICS. Nothing is pushed, and a failed read leaves the clean copies alone.
inline void IcsBody(SyncJob& j, SyncResult& r) {
    std::wstring host, path;
    if (!SplitUrl(j.link, host, path)) { r.error = L"Enlace no válido"; return; }
    std::string resp;
    DWORD code = 0;
    if (!HttpsRequest(L"GET", host, path, L"", "", resp, code)) { r.error = L"Sin conexión"; return; }
    if (code != 200) { r.error = L"Error " + std::to_wstring(code); return; }
    if (!ParseIcs(resp, j.fromKey, j.toKey, r.fetched)) { r.error = L"Respuesta no válida"; return; }
    j.account = host; // the host is all the account is called: the address itself is a secret
    r.pulled = true;
    if (j.login) r.loggedIn = true;
}

// Sign-in (when asked), token renewal, the pushes, then the window. Runs on the worker thread.
inline void SyncBody(SyncJob& j, SyncResult& r) {
    if (j.src == 3) return IcsBody(j, r);
    std::wstring err;
    if (j.src == 2) { // Outlook: browser sign-in and token renewal; Google's bridge has neither
        if (j.login) {
            if (!OAuthLogin(j.prov, j.refresh, j.access, j.expiresAt, err)) { r.error = err; return; }
            r.loggedIn = true;
        }
        if (j.access.empty() || j.expiresAt - (long long)GetTickCount64() < 60000) { // renewed when less than a minute is left
            if (!OAuthRefresh(j.prov, j.refresh, j.access, j.expiresAt, err)) {
                r.error = err;
                r.revoked = err == kInvalidGrant;
                return;
            }
        }
    }
    bool stop = false;
    for (int op : { 1, 2, 4 })
        for (size_t i = 0; !stop && i < j.pending.size(); ++i)
            if (OpOf(j.pending[i]) == op) stop = !SendOne(j, r, j.pending[i], op);
    if (stop) return;

    std::set<std::wstring> masters;
    std::vector<CalEvent> copies;
    SeriesCopies(j.series, r.pushes, masters, copies);
    std::wstring pullErr = PullWindow(j, masters, r.fetched);
    if (!pullErr.empty()) { r.error = pullErr; return; }
    r.fetched.insert(r.fetched.end(), copies.begin(), copies.end());
    r.pulled = true;
    if (j.login) r.loggedIn = true; // Google: the first answer from the bridge is the sign-in
}

inline SyncResult RunSync(SyncJob j) {
    SyncResult r;
    r.src = j.src; r.gen = j.gen; r.fromKey = j.fromKey; r.toKey = j.toKey; r.login = j.login;
    SyncBody(j, r);
    r.refresh = j.refresh; r.access = j.access; r.expiresAt = j.expiresAt; r.account = j.account;
    return r;
}

// Starts a sync (or a sign-in first, when login is set) on a worker thread. The result comes back as WM_SYNC_DONE.
inline void StartSync(HWND hwnd, SyncAccount& a, bool login, std::vector<CalEvent> pending, std::vector<CalEvent> series, int fromKey, int toKey) {
    SyncJob j;
    j.hwnd = hwnd; j.src = a.src; j.gen = a.gen; j.login = login; j.prov = a.prov;
    j.refresh = a.refresh; j.access = a.access; j.expiresAt = a.expiresAt; j.account = a.account;
    j.bridgeUrl = a.bridgeUrl; j.bridgeToken = a.bridgeToken; j.link = a.link;
    j.pending = std::move(pending); j.series = std::move(series);
    j.fromKey = fromKey; j.toKey = toKey;
    a.syncing = true;
    std::thread([j]() mutable {
        SyncResult* r = new SyncResult(RunSync(j));
        if (!PostMessageW(j.hwnd, WM_SYNC_DONE, (WPARAM)j.src, (LPARAM)r)) delete r; // window gone: nobody to hand it to
    }).detach();
}
