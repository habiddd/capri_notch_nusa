#pragma once
#include "types.h"
#include <sstream>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <cstdlib>
#include <ctime>

struct CalEvent {
    int date;        // YYYYMMDD (start of the series when it repeats)
    int minute;      // start, minutes after midnight; -1 = all day
    int end = -1;    // end in minutes; -1 = none (drawn as 60 min)
    int color = 0;   // category index into CAL_CATS
    int repeat = 0;  // 0 no, 1 daily, 2 weekly, 3 monthly, 4 yearly
    std::wstring title;
    int alert = 10;  // minutes before the start: -1 none, 0 at start, 10, 30, 60
    std::wstring notes;
    int src = 0;          // 0 Capri (local), 1 Google, 2 Outlook, 3 calendar link (ICS: read-only, only the colour is ours)
    std::wstring rid;     // remote id ("" = not uploaded yet)
    int dirty = 0;        // bits: 1 create, 2 update, 4 delete, 8 notes changed (waiting to be uploaded)
    int rev = 0;          // local edit counter, not saved: a push result for an older rev does not clear newer edits
    long long uid = 0;    // in-memory id, not saved; assigned by EventManager and MergePull
    int moreAlerts = 0;   // extra alerts as bits (1 at start, 2 = 10 min, 4 = 30 min, 8 = 1 h); the main one is `alert`
    int task = 0;         // 0 event, 1 task, 2 task done (tasks are local: src 0)
};
inline long long g_eventUid = 0;

struct CalCategory { const wchar_t* name; BYTE r, g, b; };
inline constexpr CalCategory CAL_CATS[] = {
    { L"General", 96, 165, 250 }, { L"Trabajo", 167, 139, 250 }, { L"Personal", 74, 222, 128 },
    { L"Salud", 248, 113, 113 },  { L"Estudio", 251, 191, 36 },  { L"Social", 244, 114, 182 } };
constexpr int CAL_CAT_COUNT = 6;
inline constexpr const wchar_t* CAL_REPEAT[] = { L"No se repite", L"Diario", L"Semanal", L"Mensual", L"Anual" };

inline std::wstring EventTime(int minute) {
    wchar_t buf[8];
    wsprintfW(buf, L"%02d:%02d", minute / 60, minute % 60);
    return buf;
}

// The date `days` away from a YYYYMMDD key, normalized by mktime (midday: clear of DST jumps)
inline tm KeyTm(int key, int days = 0) {
    tm t = {};
    t.tm_year = key / 10000 - 1900;
    t.tm_mon = key / 100 % 100 - 1;
    t.tm_mday = key % 100 + days;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    mktime(&t);
    return t;
}
inline int TmKey(const tm& t) { return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday; }

// Days since 1970-01-01 (civil-calendar arithmetic: no mktime, so no DST or pre-1970 trouble)
inline long long DayNumber(int key) {
    long long y = key / 10000, m = key / 100 % 100, d = key % 100;
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

// shortcut: a monthly event on the 29th-31st skips shorter months; upgrade to clamping to the last day if it matters
inline bool Occurs(const CalEvent& e, int date) {
    if (date == e.date) return true;
    if (!e.repeat || date < e.date) return false;
    switch (e.repeat) {
    case 1: return true;
    case 2: return (DayNumber(date) - DayNumber(e.date)) % 7 == 0;
    case 3: return date % 100 == e.date % 100;
    default: return date % 10000 == e.date % 10000;
    }
}

// Lowercase without accents ("Mañana" -> "manana"), so typed words match with or without them
inline std::wstring FoldAccents(std::wstring s) {
    static const wchar_t from[] = L"áéíóúüñÁÉÍÓÚÜÑ", to[] = L"aeiouunaeiouun";
    for (auto& c : s) {
        if (const wchar_t* p = wcschr(from, c)) c = to[p - from];
        c = (wchar_t)towlower(c);
    }
    return s;
}

// One H:MM at t[p..]; advances p
inline bool ClockAt(const std::wstring& t, size_t& p, int& minute) {
    size_t q = p;
    int h = 0, digits = 0;
    while (q < t.size() && iswdigit(t[q]) && digits < 2) { h = h * 10 + (t[q++] - L'0'); ++digits; }
    if (!digits || q + 3 > t.size() || t[q] != L':' || !iswdigit(t[q + 1]) || !iswdigit(t[q + 2])) return false;
    int m = (t[q + 1] - L'0') * 10 + (t[q + 2] - L'0');
    if (h > 23 || m > 59) return false;
    minute = h * 60 + m;
    p = q + 3;
    return true;
}

// "15:30", "15:30-16:30" or "15h"; an end at or before the start is dropped
inline bool QuickClock(const std::wstring& t, int& minute, int& end) {
    size_t p = 0, n = t.size();
    end = -1;
    if (ClockAt(t, p, minute)) {
        if (p == n) return true;
        int b;
        if (t[p] != L'-') return false;
        ++p;
        if (!ClockAt(t, p, b) || p != n) return false;
        if (b > minute) end = b;
        return true;
    }
    if ((n == 2 || n == 3) && t[n - 1] == L'h' && iswdigit(t[0]) && (n == 2 || iswdigit(t[1]))) {
        int h = std::stoi(t.substr(0, n - 1));
        if (h > 23) return false;
        minute = h * 60;
        return true;
    }
    return false;
}

// "Dentista mañana 15:30-16:30 #salud cada semana" -> event. False when no title is left.
// base = the selected day (used when no date is typed), today = YYYYMMDD. Recognized words are removed from the title.
inline bool ParseQuickEvent(const std::wstring& s, int base, int today, CalEvent& out) {
    static const wchar_t* const WEEKDAYS[] = { L"domingo", L"lunes", L"martes", L"miercoles", L"jueves", L"viernes", L"sabado" };
    std::vector<std::wstring> tok;
    std::wistringstream in(s);
    for (std::wstring t; in >> t;) tok.push_back(t);

    out = CalEvent{ base, -1, -1, 0, 0, L"" };
    bool gotTime = false, gotDate = false;
    std::wstring title;
    for (size_t i = 0; i < tok.size(); ++i) {
        std::wstring f = FoldAccents(tok[i]), next = i + 1 < tok.size() ? FoldAccents(tok[i + 1]) : L"";
        int m, e;
        if (!gotTime && QuickClock(f, m, e)) { out.minute = m; out.end = e; gotTime = true; continue; }
        if (!gotDate) {
            int add = -1; // days from today
            if (f == L"hoy") add = 0;
            else if (f == L"manana") add = 1;
            else if (f == L"pasado" && next == L"manana") { add = 2; ++i; }
            else for (int w = 0; w < 7; ++w) if (f == WEEKDAYS[w]) add = (w - KeyTm(today).tm_wday + 7) % 7;
            if (add >= 0) { out.date = TmKey(KeyTm(today, add)); gotDate = true; continue; }
            wchar_t *p, *q = nullptr;
            long d = wcstol(f.c_str(), &p, 10), mo = p != f.c_str() && *p == L'/' ? wcstol(p + 1, &q, 10) : 0;
            if (mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && !*q) {
                int key = (int)(today / 10000 * 10000 + mo * 100 + d);
                if (key < today) key += 10000;
                if (TmKey(KeyTm(key)) == key) { out.date = key; gotDate = true; continue; } // rejects 31/4 and the like
            }
        }
        if (f.size() > 3 && f[0] == L'#') {
            int c = 0;
            while (c < CAL_CAT_COUNT && FoldAccents(CAL_CATS[c].name).compare(0, f.size() - 1, f, 1, std::wstring::npos) != 0) ++c;
            if (c < CAL_CAT_COUNT) { out.color = c; continue; }
        }
        int rep = f == L"diario" ? 1 : f == L"semanal" ? 2 : f == L"mensual" ? 3 : f == L"anual" ? 4 : 0;
        if (!rep && f == L"cada") {
            rep = next == L"dia" ? 1 : next == L"semana" ? 2 : next == L"mes" ? 3 : next == L"ano" ? 4 : 0;
            if (rep) ++i;
        }
        if (rep) { out.repeat = rep; continue; }
        title += (title.empty() ? L"" : L" ") + tok[i];
    }
    out.title = title;
    return !title.empty();
}

// A title typed with quick words ("Dentista mañana 15:30 #salud") moves only what was recognized into the event
inline void ApplyTitleTokens(CalEvent& e, int today) {
    e.title.erase(0, e.title.find_first_not_of(L' '));
    e.title.erase(e.title.find_last_not_of(L' ') + 1);
    CalEvent p;
    if (!ParseQuickEvent(e.title, e.date, today, p) || p.title == e.title) return;
    if (p.minute >= 0) {
        e.minute = p.minute;
        e.end = p.end >= 0 ? p.end : (std::min)(1440, p.minute + 60);
    }
    e.date = p.date;
    if (p.color) e.color = p.color;
    if (p.repeat) e.repeat = p.repeat;
    e.title = p.title;
}

// Alert bits: 1 at start, 2 = 10 min, 4 = 30 min, 8 = 1 h. Each bit's lead in minutes:
inline constexpr int ALERT_MINUTES[] = { 0, 10, 30, 60 };
inline int AlertBit(int alert) {
    for (int i = 0; i < 4; ++i) if (alert == ALERT_MINUTES[i]) return 1 << i;
    return 0; // -1 (none) and anything else
}
// Every chosen alert as bits (1 at start, 2 = 10 min, 4 = 30 min, 8 = 1 h); 0 = none
inline int AlertMask(int alert, int more) { return more | AlertBit(alert); }
// The opposite: the longest lead becomes the main alert (the one calendar services get), the rest go to `more`
inline void SetAlertMask(int mask, int& alert, int& more) {
    alert = -1;
    for (int i = 0; i < 4; ++i) if (mask & (1 << i)) alert = ALERT_MINUTES[i]; // the highest bit is the longest lead
    more = mask & ~AlertBit(alert);
}

// 0 nothing, 1 early warning (lead is the minutes before it fired), 2 it starts now; timed events only.
// Due when any chosen alert matches; "starts now" fires for any event with an alert.
inline int AlertDue(const CalEvent& e, int nowMinute, int* lead = nullptr) {
    int mask = AlertMask(e.alert, e.moreAlerts);
    if (e.minute < 0 || !mask) return 0;
    if (e.minute == nowMinute) { if (lead) *lead = 0; return 2; }
    for (int i = 1; i < 4; ++i)
        if ((mask & (1 << i)) && e.minute - ALERT_MINUTES[i] == nowMinute) { if (lead) *lead = ALERT_MINUTES[i]; return 1; }
    return 0;
}

// The task list's rule: an open task always shows; a done one only on its day or later
inline bool TaskListed(const CalEvent& e, int today) {
    return e.task != 0 && !(e.dirty & 4) && (e.task == 1 || e.date >= today);
}
class EventManager {
public:
    std::vector<CalEvent> events; // sorted by date, then start (all-day first)
    std::wstring storagePath;

    EventManager() {
        storagePath = AppDataFile(L"capri_events.txt");
        Load();
    }

    static bool Before(const CalEvent& a, const CalEvent& b) { return a.date != b.date ? a.date < b.date : a.minute < b.minute; }

    // Returns the index the event ended up at
    size_t Add(const CalEvent& e) {
        CalEvent n = e;
        if (n.src == 1 || n.src == 2) n.dirty = 1; // a remote event made here is uploaded by the next sync; a link event never is
        size_t at = Insert(n);
        Save();
        return at;
    }

    // shortcut: a repeating event is edited, moved and deleted as a whole series; upgrade to per-occurrence exceptions if needed
    size_t Update(size_t index, const CalEvent& e) {
        if (index < events.size() && events[index].src == 3) { // calendar link: read-only, only the colour is kept
            events[index].color = e.color;
            Save();
            return index;
        }
        CalEvent n = e;
        if (index < events.size()) {
            const CalEvent& old = events[index];
            n.uid = old.uid;
            if (old.src) { // remote: a pending create stays a create; notes go up only when they changed
                n.src = old.src;
                n.rid = old.rid;
                n.rev = old.rev + 1;
                n.dirty = old.dirty | ((old.dirty & 1) ? 0 : 2) | (n.notes != old.notes ? 8 : 0);
            }
            events.erase(events.begin() + index);
        }
        size_t at = Insert(n);
        Save();
        return at;
    }

    void Remove(size_t index) {
        if (index < events.size() && events[index].src != 3) { // calendar link events are read-only
            CalEvent& e = events[index];
            if (e.src && !e.rid.empty()) { e.dirty |= 4; e.rev++; } // hidden now, deleted in the service on the next sync
            else events.erase(events.begin() + index);
            Save();
        }
    }

    // Indices of the events on `date` (repeats included): all-day first, then by start
    std::vector<size_t> On(int date) const {
        std::vector<size_t> out;
        for (size_t i = 0; i < events.size(); ++i) if (!(events[i].dirty & 4) && Occurs(events[i], date)) out.push_back(i);
        std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) { return events[a].minute < events[b].minute; });
        return out;
    }

    // Lines are date|minute|end|color|repeat|alert|title, then title \t notes \t src \t dirty \t rid \t task \t moreAlerts
    void Save() {
        std::wstring all;
        for (const auto& e : events)
            all += std::to_wstring(e.date) + L"|" + std::to_wstring(e.minute) + L"|" + std::to_wstring(e.end) + L"|" +
                   std::to_wstring(e.color) + L"|" + std::to_wstring(e.repeat) + L"|" + std::to_wstring(e.alert) + L"|" + e.title +
                   L"\t" + e.notes + L"\t" + std::to_wstring(e.src) + L"\t" + std::to_wstring(e.dirty) + L"\t" + e.rid + L"\t" +
                   std::to_wstring(e.task) + L"\t" + std::to_wstring(e.moreAlerts) + L"\n";
        WriteUtf8File(storagePath, all);
    }

    // Also reads the 5-number lines (alert 10), the old date|minute|title lines and the tab-notes lines; malformed ones are skipped.
    // Only the first 6 fields are split, so a title like "5|x" in the current format is never read as a number.
    void Load() {
        events.clear();
        std::wstring all;
        if (!ReadUtf8File(storagePath, all)) return;
        std::wistringstream lines(all);
        std::wstring line;
        auto num = [](const std::wstring& s, long& v) {
            wchar_t* end;
            v = wcstol(s.c_str(), &end, 10);
            return !s.empty() && !*end;
        };
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            std::vector<std::wstring> f;     // up to the first 6 fields
            std::vector<size_t> start = { 0 }; // start[k]: offset just after the k-th bar
            for (size_t q; f.size() < 6 && (q = line.find(L'|', start.back())) != std::wstring::npos;) {
                f.push_back(line.substr(start.back(), q - start.back()));
                start.push_back(q + 1);
            }
            long v[6] = {};
            size_t n = 0; // leading numeric fields
            while (n < f.size() && num(f[n], v[n])) ++n;
            CalEvent e{ 0, 0, -1, 0, 0, L"" };
            if (n >= 5) {
                // shortcut: a pre-alert 5-number line whose title starts "N|" is misread; such files only came from one build on 2026-11-03
                bool withAlert = n == 6;
                e = {(int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4], line.substr(start[withAlert ? 6 : 5]), withAlert ? (int)v[5] : 10 };
            } else if (n >= 2) {
                e = { (int)v[0], (int)v[1], -1, 0, 0, line.substr(start[2]) };
            } else {
                continue;
            }
            // title \t notes \t src \t dirty \t rid [\t task \t moreAlerts]; older lines have no such tail, so their notes are everything after the first tab
            std::vector<std::wstring> tf;
            for (size_t s = 0;;) {
                size_t q = e.title.find(L'\t', s);
                tf.push_back(e.title.substr(s, q == std::wstring::npos ? q : q - s));
                if (q == std::wstring::npos) break;
                s = q + 1;
            }
            long sv = 0, dv = 0, tk = 0, mo = 0;
            const bool v7 = tf.size() == 7;
            if (v7 && !(num(tf[5], tk) && tk >= 0 && tk <= 2 && num(tf[6], mo) && mo >= 0 && mo <= 15)) continue;
            if ((tf.size() == 5 || v7) && num(tf[2], sv) && num(tf[3], dv) && sv >= 0 && sv <= 3 && dv >= 0 && dv <= 15) {
                e.title = tf[0];
                e.notes = tf[1];
                e.src = (int)sv;
                e.dirty = (int)dv;
                e.rid = tf[4];
                e.task = v7 ? (int)tk : 0;
                e.moreAlerts = v7 ? (int)mo : 0;
            } else {
                size_t tab = e.title.find(L'\t');
                if (tab != std::wstring::npos) {
                    e.notes = e.title.substr(tab + 1);
                    e.title.erase(tab);
                }
            }
            if (e.title.empty() || e.date < 10000101 || e.minute < -1 || e.minute >= 1440 || e.end < -1 || e.end > 1440 ||
                e.color < 0 || e.color >= CAL_CAT_COUNT || e.repeat < 0 || e.repeat > 4 ||
                (e.alert != -1 && e.alert != 0 && e.alert != 10 && e.alert != 30 && e.alert != 60)) continue;
            e.uid = ++g_eventUid;
            events.push_back(e);
        }
        std::stable_sort(events.begin(), events.end(), Before); // a hand-edited file may be out of order
    }
private:
    size_t Insert(CalEvent e) {
        if (!e.uid) e.uid = ++g_eventUid;
        size_t at = std::upper_bound(events.begin(), events.end(), e, Before) - events.begin(); // index: insert invalidates iterators
        events.insert(events.begin() + at, e);
        return at;
    }
};
