#pragma once
#include <windows.h>
#include <ctime>
#include <string>
#include "events.h"

// Pieces the calendar readers share (Google/Outlook in sync.h, the ICS link in ics.h): local time, zone names, one-line text, alerts.

inline void UtcToLocal(long long utc, int& dateKey, int& minute) {
    time_t tt = (time_t)utc;
    tm lt = {};
    localtime_s(&lt, &tt);
    dateKey = TmKey(lt);
    minute = lt.tm_hour * 60 + lt.tm_min;
}

// Google wants an IANA name ("America/Mexico_City") from ICU; "" when icu.dll has no such export.
inline std::wstring IanaZone() {
    typedef int (*GetZone)(wchar_t*, int, int*);
    HMODULE icu = LoadLibraryExW(L"icu.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    GetZone get = icu ? (GetZone)GetProcAddress(icu, "ucal_getDefaultTimeZone") : NULL;
    wchar_t buf[128] = {};
    int err = 0, n = get ? get(buf, 128, &err) : 0;
    if (icu) FreeLibrary(icu);
    return n > 0 && err <= 0 ? std::wstring(buf, n) : L"";
}

// Microsoft wants its own zone name ("Central Standard Time")
inline std::wstring WindowsZone() {
    DYNAMIC_TIME_ZONE_INFORMATION d = {};
    GetDynamicTimeZoneInformation(&d);
    return d.TimeZoneKeyName;
}

// The file is line-based and tab-separated, so remote text is flattened to one line
inline std::wstring OneLine(std::wstring s) {
    for (auto& c : s) if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}

// Our alerts are -1 none, 0, 10, 30, 60 minutes before the start
inline int AlertFromMinutes(int m) { return m <= 0 ? 0 : m <= 10 ? 10 : m <= 30 ? 30 : 60; }
