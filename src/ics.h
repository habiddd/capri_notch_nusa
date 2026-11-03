#pragma once
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "calutil.h"
#include "events.h"
#include "net.h"

// A calendar link (ICS, RFC 5545) as pure text: sync.h fetches it, this only reads it. Only VEVENT blocks are used. Repeats are
// expanded inside [fromKey, toKey]. A text without BEGIN:VCALENDAR is refused, so a login page never reads as an empty calendar.

struct IcsProp { std::string name, params, value; };
struct IcsZones { std::map<std::string, int> offset; std::string win, iana; }; // TZID -> minutes east of UTC (its STANDARD block)
struct IcsWhen { int date = 0, minute = -1; bool allDay = false; };

// "webcal://" becomes "https://"; false unless the result is an https URL with a host
inline bool NormalizeIcsLink(std::wstring& url) {
    const std::wstring webcal = L"webcal://";
    if (url.compare(0, webcal.size(), webcal) == 0) url = L"https://" + url.substr(webcal.size());
    std::wstring host, path;
    return SplitUrl(url, host, path);
}

inline std::wstring IcsHost(const std::wstring& link) {
    std::wstring host, path;
    SplitUrl(link, host, path);
    return host;
}

// "NAME;PARAM=x:VALUE"; a colon inside a quoted parameter does not end the name
inline bool IcsSplit(const std::string& line, IcsProp& p) {
    size_t c = 0;
    for (bool quoted = false; c < line.size() && (quoted || line[c] != ':'); ++c) if (line[c] == '"') quoted = !quoted;
    if (c >= line.size()) return false;
    std::string head = line.substr(0, c);
    size_t semi = head.find(';');
    p.name = head.substr(0, semi);
    for (char& ch : p.name) ch = (char)toupper((unsigned char)ch);
    p.params = semi == std::string::npos ? "" : head.substr(semi + 1);
    p.value = line.substr(c + 1);
    return true;
}

// One parameter's value ("TZID=Romance Standard Time"), quotes removed; "" when absent
inline std::string IcsParam(const std::string& params, const char* key) {
    std::string k = std::string(key) + "=";
    for (size_t p = 0; p <= params.size();) {
        size_t semi = std::min(params.find(';', p), params.size());
        if (params.compare(p, k.size(), k) == 0) {
            std::string v = params.substr(p + k.size(), semi - p - k.size());
            v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
            return v;
        }
        p = semi + 1;
    }
    return "";
}

inline const IcsProp* IcsFind(const std::vector<IcsProp>& ps, const char* name) {
    for (const IcsProp& p : ps) if (p.name == name) return &p;
    return nullptr;
}

// TEXT values: \n is a line break (flattened to a space like the other services), \, \; \\ are the characters themselves
inline std::wstring IcsText(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char c = v[++i];
            out += (c == 'n' || c == 'N') ? '\n' : c;
        } else {
            out += v[i];
        }
    }
    return OneLine(Wide(out));
}

// TZOFFSETTO "+0100" -> 60, "-0330" -> -210
inline int IcsOffset(const std::string& v) {
    int hh = 0, mm = 0;
    if (v.size() < 5 || sscanf(v.c_str() + 1, "%2d%2d", &hh, &mm) != 2) return 0;
    return (v[0] == '-' ? -1 : 1) * (hh * 60 + mm);
}

// A DATE or DATE-TIME as this machine's local date and minute. TZID is kept when it names this zone (already local) or has no
// STANDARD offset in the file (shortcut below). False for any other text.
inline bool IcsWhenOf(const IcsProp& p, const IcsZones& z, IcsWhen& w) {
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, s = 0;
    const bool utc = !p.value.empty() && p.value.back() == 'Z';
    const int n = sscanf(p.value.c_str(), "%4d%2d%2dT%2d%2d%2d", &Y, &M, &D, &h, &mi, &s);
    if (n == 3 && p.value.size() == 8) { // a bare date: all day
        w = { Y * 10000 + M * 100 + D, -1, true };
        return true;
    }
    if (n != 6 || p.value.size() != (utc ? 16u : 15u)) return false;
    tm t = {};
    t.tm_year = Y - 1900;
    t.tm_mon = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    const long long wall = _mkgmtime(&t); // the fields read as UTC
    if (utc) {
        UtcToLocal(wall, w.date, w.minute);
        return true;
    }
    const std::string tzid = IcsParam(p.params, "TZID");
    auto it = z.offset.find(tzid);
    if (it != z.offset.end() && tzid != z.win && tzid != z.iana) {
        // shortcut: a foreign TZID uses its STANDARD offset only, so its summer-time events are an hour off; upgrade with the DAYLIGHT rules
        UtcToLocal(wall - it->second * 60LL, w.date, w.minute);
        return true;
    }
    // shortcut: a TZID with no STANDARD offset in the file, and floating times, are read as this machine's time; upgrade with a zone table
    w = { Y * 10000 + M * 100 + D, h * 60 + mi, false };
    return true;
}

// TRIGGER as minutes before the start through AlertFromMinutes: "-PT30M", "-PT1H", "PT0M". -1 for anything else (after the start, absolute)
inline int IcsAlert(const std::string& v) {
    size_t p = 0;
    const bool before = p < v.size() && v[p] == '-';
    if (before) ++p;
    if (p >= v.size() || v[p] != 'P') return -1;
    long total = 0, num = -1;
    bool time = false;
    for (++p; p < v.size(); ++p) {
        const char c = v[p];
        if (isdigit((unsigned char)c)) { num = (num < 0 ? 0 : num * 10) + (c - '0'); continue; }
        if (c == 'T') time = true;
        else if (num < 0) return -1;
        else if (c == 'D') total += num * 1440;
        else if (c == 'H' && time) total += num * 60;
        else if (c == 'M' && time) total += num;
        else if (c == 'S' && time) total += num / 60;
        else return -1;
        num = -1;
    }
    if (num >= 0 || (!before && total > 0)) return -1;
    return AlertFromMinutes((int)total);
}

// BYDAY "MO,WE" as weekdays (0 = Sunday). False for a day with a number ("2TH") or anything unknown.
inline bool IcsWeekdays(const std::string& v, std::set<int>& days) {
    static const char* names[] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };
    for (size_t p = 0; p <= v.size();) {
        size_t c = std::min(v.find(',', p), v.size());
        std::string t = v.substr(p, c - p);
        int d = -1;
        for (int i = 0; i < 7; ++i) if (t == names[i]) d = i;
        if (d < 0) return false;
        days.insert(d);
        p = c + 1;
    }
    return true;
}

// A whole number in [lo, hi]
inline bool IcsInt(const std::string& v, int lo, int hi, int& n) {
    char* end = nullptr;
    long x = strtol(v.c_str(), &end, 10);
    n = (int)x;
    return !v.empty() && *end == '\0' && x >= lo && x <= hi;
}

inline int IcsWeekdayOf(int key) { return (int)(((DayNumber(key) + 4) % 7 + 7) % 7); } // 1970-01-01 was a Thursday
inline int IcsDaysIn(int y, int m) {
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : d[m - 1];
}

// The dates of one repeat rule inside [fromKey, toKey], in order. `skip` takes out single dates (EXDATE, replaced occurrences).
// COUNT counts from the start, the window does not. A series stops at 1000 dates in the window.
inline void IcsRepeat(const IcsWhen& start, const std::string& rule, const std::set<int>& skip, int fromKey, int toKey, std::vector<int>& dates) {
    std::map<std::string, std::string> r;
    for (size_t p = 0; p < rule.size();) {
        size_t semi = std::min(rule.find(';', p), rule.size());
        size_t eq = rule.find('=', p);
        if (eq != std::string::npos && eq < semi) r[rule.substr(p, eq - p)] = rule.substr(eq + 1, semi - eq - 1);
        p = semi + 1;
    }
    const std::string freq = r["FREQ"];
    const bool daily = freq == "DAILY", weekly = freq == "WEEKLY", monthly = freq == "MONTHLY", yearly = freq == "YEARLY";
    int interval = 1, count = 0, untilKey = 0, bymd = 0, bym = 0;
    std::set<int> days;
    bool ok = daily || weekly || monthly || yearly;
    for (const auto& kv : r) {
        const std::string& k = kv.first;
        const std::string& v = kv.second;
        if (k == "FREQ" || k == "WKST") continue; // shortcut: WKST is ignored (weeks start on Monday); it only matters with INTERVAL > 1 and BYDAY
        if (k == "INTERVAL") ok = ok && IcsInt(v, 1, 9999, interval);
        else if (k == "COUNT") ok = ok && IcsInt(v, 1, 1000000000, count);
        else if (k == "UNTIL") ok = ok && sscanf(v.c_str(), "%8d", &untilKey) == 1;
        else if (k == "BYDAY") ok = ok && (weekly || monthly) && IcsWeekdays(v, days);
        else if (k == "BYMONTHDAY") ok = ok && (monthly || yearly) && IcsInt(v, 1, 31, bymd);
        else if (k == "BYMONTH") ok = ok && yearly && IcsInt(v, 1, 12, bym);
        else ok = false;
    }
    if (bymd && !days.empty()) ok = false;
    if (!ok) { // shortcut: BYSETPOS, BYWEEKNO, numbered BYDAY, several BYMONTHDAY and other rules keep only the first occurrence; upgrade to a full RRULE reader if calendars need them
        if (start.date >= fromKey && start.date <= toKey && !skip.count(start.date)) dates.push_back(start.date);
        return;
    }

    int n = 0;
    auto take = [&](int key) { // false: the series is over (past the window, UNTIL, COUNT, or the 1000 cap)
        if (key > toKey || (untilKey && key > untilKey) || (count && n >= count) || dates.size() >= 1000) return false;
        ++n;
        if (key >= fromKey && !skip.count(key)) dates.push_back(key);
        return true;
    };
    const int y0 = start.date / 10000, m0 = start.date / 100 % 100, d0 = start.date % 100;
    if (daily) {
        for (int k = 0;; ++k) if (!take(TmKey(KeyTm(start.date, k * interval)))) return;
    } else if (weekly) {
        if (days.empty()) days.insert(IcsWeekdayOf(start.date));
        const int monday = TmKey(KeyTm(start.date, -((IcsWeekdayOf(start.date) + 6) % 7)));
        for (int w = 0;; ++w) {
            const int base = TmKey(KeyTm(monday, 7 * interval * w));
            if (base > toKey) return;
            for (int off = 0; off < 7; ++off) { // Monday first
                const int key = TmKey(KeyTm(base, off));
                if (days.count((off + 1) % 7) && key >= start.date && !take(key)) return;
            }
        }
    } else if (monthly) {
        const int dom = bymd ? bymd : d0;
        for (int k = 0;; ++k) {
            const int mm = m0 - 1 + interval * k, y = y0 + mm / 12, m = mm % 12 + 1;
            if (y * 10000 + m * 100 + 1 > toKey) return;
            for (int d = 1; d <= IcsDaysIn(y, m); ++d) { // a month without that day is skipped
                const int key = y * 10000 + m * 100 + d;
                const bool hit = days.empty() ? d == dom : days.count(IcsWeekdayOf(key)) > 0;
                if (hit && key >= start.date && !take(key)) return;
            }
        }
    } else {
        const int mon = bym ? bym : m0, dom = bymd ? bymd : d0;
        for (int k = 0;; ++k) {
            const int y = y0 + interval * k;
            if (y * 10000 + 101 > toKey) return;
            if (dom > IcsDaysIn(y, mon)) continue;
            const int key = y * 10000 + mon * 100 + dom;
            if (key >= start.date && !take(key)) return;
        }
    }
}

// The events of an ICS text inside [fromKey, toKey] (YYYYMMDD), in the form the calendar uses: src 3, no local repeat (each
// occurrence is its own event). rid is the UID, or "UID|YYYYMMDD" for an occurrence. False when the text is not a calendar.
inline bool ParseIcs(const std::string& text, int fromKey, int toKey, std::vector<CalEvent>& out) {
    if (text.find("BEGIN:VCALENDAR") == std::string::npos) return false;

    // Unfolding: a line that starts with a space or tab goes on the line before it
    std::vector<std::string> lines;
    for (size_t p = 0; p < text.size();) {
        size_t nl = std::min(text.find('\n', p), text.size());
        std::string line = text.substr(p, nl - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        p = nl + 1;
        if (line.empty()) continue;
        if ((line[0] == ' ' || line[0] == '\t') && !lines.empty()) lines.back() += line.substr(1);
        else lines.push_back(line);
    }

    // The VEVENT blocks (with their VALARM trigger as "TRIGGER-ALARM") and the zone offsets of the VTIMEZONE blocks
    IcsZones z;
    z.win = Utf8(WindowsZone());
    z.iana = Utf8(IanaZone());
    std::vector<std::vector<IcsProp>> events;
    std::vector<IcsProp> cur;
    std::string tzid;
    bool inEvent = false, inAlarm = false, inZone = false, inStd = false;
    for (const std::string& line : lines) {
        IcsProp p;
        if (!IcsSplit(line, p)) continue;
        if (p.name == "BEGIN") {
            if (p.value == "VEVENT") { cur.clear(); inEvent = true; }
            else if (p.value == "VALARM") inAlarm = true;
            else if (p.value == "VTIMEZONE") { inZone = true; tzid.clear(); }
            else if (p.value == "STANDARD") inStd = true;
        } else if (p.name == "END") {
            if (p.value == "VEVENT") { if (inEvent) events.push_back(cur); inEvent = false; }
            else if (p.value == "VALARM") inAlarm = false;
            else if (p.value == "VTIMEZONE") inZone = false;
            else if (p.value == "STANDARD") inStd = false;
        } else if (inZone && p.name == "TZID") {
            tzid = p.value;
        } else if (inZone && inStd && p.name == "TZOFFSETTO") {
            z.offset[tzid] = IcsOffset(p.value);
        } else if (inEvent && inAlarm) {
            if (p.name == "TRIGGER" && !IcsFind(cur, "TRIGGER-ALARM")) cur.push_back({ "TRIGGER-ALARM", "", p.value });
        } else if (inEvent) {
            cur.push_back(p);
        }
    }

    // A RECURRENCE-ID replaces one occurrence of its series, EXDATE removes dates from it: both are keyed by UID
    std::map<std::string, std::set<int>> skip;
    for (const auto& ps : events) {
        const IcsProp* uid = IcsFind(ps, "UID");
        if (!uid) continue;
        IcsWhen rw;
        if (const IcsProp* rec = IcsFind(ps, "RECURRENCE-ID")) if (IcsWhenOf(*rec, z, rw)) skip[uid->value].insert(rw.date);
        for (const IcsProp& p : ps) {
            if (p.name != "EXDATE") continue;
            for (size_t q = 0; q <= p.value.size();) {
                size_t c = std::min(p.value.find(',', q), p.value.size());
                IcsWhen w;
                if (IcsWhenOf({ "", p.params, p.value.substr(q, c - q) }, z, w)) skip[uid->value].insert(w.date);
                q = c + 1;
            }
        }
    }

    static const std::set<int> none;
    for (const auto& ps : events) {
        const IcsProp* uidP = IcsFind(ps, "UID");
        const IcsProp* st = IcsFind(ps, "DTSTART");
        IcsWhen start;
        if (!uidP || uidP->value.empty() || !st || !IcsWhenOf(*st, z, start)) continue;
        const IcsProp* status = IcsFind(ps, "STATUS");
        if (status && status->value == "CANCELLED") continue;

        CalEvent t{ start.date, start.minute, -1, 0, 0, L"" };
        t.src = 3;
        const IcsProp* sum = IcsFind(ps, "SUMMARY");
        std::wstring title = sum ? IcsText(sum->value) : L"";
        t.title = title.empty() ? L"(Sin título)" : title;
        if (const IcsProp* d = IcsFind(ps, "DESCRIPTION")) t.notes = IcsText(d->value);
        t.alert = -1; // no VALARM: no reminder (a CalEvent defaults to 10)
        if (const IcsProp* a = IcsFind(ps, "TRIGGER-ALARM")) t.alert = IcsAlert(a->value);

        // The length: same-day events keep their span, events that end on another day run to the end of the day
        int span = -1;
        bool past = false;
        IcsWhen endW;
        if (!start.allDay)
            if (const IcsProp* en = IcsFind(ps, "DTEND"))
                if (IcsWhenOf(*en, z, endW) && !endW.allDay) {
                    if (endW.date != start.date) past = true;
                    else if (endW.minute > start.minute) span = endW.minute - start.minute;
                }
        auto endOf = [&](int minute) { return minute < 0 ? -1 : past ? 1440 : span > 0 ? (std::min)(1440, minute + span) : -1; };

        IcsWhen rw;
        const IcsProp* rec = IcsFind(ps, "RECURRENCE-ID");
        const bool replaces = rec && IcsWhenOf(*rec, z, rw);
        const IcsProp* rr = IcsFind(ps, "RRULE");
        const std::string uid = uidP->value;
        std::vector<int> dates;
        if (rr && !replaces) {
            auto it = skip.find(uid);
            IcsRepeat(start, rr->value, it == skip.end() ? none : it->second, fromKey, toKey, dates);
        } else if (start.date >= fromKey && start.date <= toKey) {
            dates.push_back(start.date);
        }
        for (int key : dates) {
            CalEvent e = t;
            e.date = key;
            e.end = endOf(start.minute);
            e.rid = Wide(replaces ? uid + "|" + std::to_string(rw.date) : rr ? uid + "|" + std::to_string(key) : uid);
            out.push_back(e);
        }
    }
    return true;
}
