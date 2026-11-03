// g++ -std=c++17 -Wall tests/ics_test.cpp -o %TEMP%\ics_test.exe -static -lgdiplus -lgdi32 -lwinhttp -lwinmm -luser32 -lshell32 -lruntimeobject -lole32 -loleaut32 -lshcore -lws2_32 -lbcrypt -lcrypt32 -ldwmapi -lpsapi
#define _WIN32_WINNT 0x0A00 // same prelude as main.cpp: the headers are not self-contained
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "../src/ics.h"

static long long Gm(int y, int mo, int d, int h, int mi) { // UTC epoch of these fields
    tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi;
    return _mkgmtime(&t);
}
static std::string Event(const char* uid, std::initializer_list<const char*> props) { // one VEVENT
    std::string s = "BEGIN:VEVENT\r\nUID:" + std::string(uid) + "\r\n";
    for (const char* p : props) s += std::string(p) + "\r\n";
    return s + "END:VEVENT\r\n";
}
static std::string Wrap(const std::string& body) { return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n" + body + "END:VCALENDAR\r\n"; }
static int CountDate(const std::vector<CalEvent>& v, int date) {
    int n = 0;
    for (const CalEvent& e : v) n += e.date == date;
    return n;
}

int main() {
    std::vector<CalEvent> out;
    const int from = 20261101, to = 20261130;

    // a plain event in UTC: the local wall time of that instant, a span within the day
    out.clear();
    assert(ParseIcs(Wrap(Event("z1", { "DTSTART:20261105T153000Z", "DTEND:20261105T163000Z", "SUMMARY:Reunion\\, equipo" })), from, to, out));
    int sd = 0, sm = 0, ed = 0, em = 0;
    UtcToLocal(Gm(2026, 11, 5, 15, 30), sd, sm);
    UtcToLocal(Gm(2026, 11, 5, 16, 30), ed, em);
    assert(out.size() == 1 && out[0].date == sd && out[0].minute == sm && out[0].src == 3 && out[0].repeat == 0);
    assert(out[0].end == (ed != sd ? 1440 : em > sm ? em : -1) && out[0].title == L"Reunion, equipo" && out[0].rid == L"z1");

    // all day: the start day only, the exclusive end is not a second day
    out.clear();
    assert(ParseIcs(Wrap(Event("d1", { "DTSTART;VALUE=DATE:20261110", "DTEND;VALUE=DATE:20261112", "SUMMARY:Feria" })), from, to, out));
    assert(out.size() == 1 && out[0].date == 20261110 && out[0].minute == -1 && out[0].end == -1 && out[0].title == L"Feria");

    // TZID that is this machine's zone: the wall time is already local
    out.clear();
    std::string win = Utf8(WindowsZone());
    assert(ParseIcs(Wrap(Event("t1", { ("DTSTART;TZID=" + win + ":20261105T093000").c_str(), ("DTEND;TZID=" + win + ":20261105T103000").c_str(), "SUMMARY:Local" })), from, to, out));
    assert(out.size() == 1 && out[0].date == 20261105 && out[0].minute == 570 && out[0].end == 630);

    // TZID of another zone: its STANDARD offset (+01:00) turns the wall time into UTC, then into local time
    out.clear();
    std::string other = "BEGIN:VTIMEZONE\r\nTZID:Zona Prueba\r\nBEGIN:STANDARD\r\nTZOFFSETFROM:+0200\r\nTZOFFSETTO:+0100\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n";
    assert(ParseIcs(Wrap(other + Event("t2", { "DTSTART;TZID=Zona Prueba:20261105T093000", "SUMMARY:Lejana" })), from, to, out));
    UtcToLocal(Gm(2026, 11, 5, 8, 30), sd, sm);
    assert(out.size() == 1 && out[0].date == sd && out[0].minute == sm);

    // DESCRIPTION: escapes, a folded line (its leading space is dropped), line breaks flattened to spaces
    out.clear();
    assert(ParseIcs(Wrap(Event("n1", { "DTSTART:20261105T100000", "SUMMARY:x", "DESCRIPTION:L\xC3\xADnea uno\\, dos\\; tres\\nfin", "  que sigue" })), from, to, out));
    assert(out.size() == 1 && out[0].notes == L"Línea uno, dos; tres fin que sigue");

    // a cancelled event is skipped
    out.clear();
    assert(ParseIcs(Wrap(Event("c1", { "DTSTART:20261105T100000", "STATUS:CANCELLED" })), from, to, out) && out.empty());

    // weekly on Monday and Wednesday, four times: 2, 4, 9 and 11 November
    out.clear();
    assert(ParseIcs(Wrap(Event("w1", { "DTSTART:20261102T100000", "RRULE:FREQ=WEEKLY;BYDAY=MO,WE;COUNT=4" })), from, to, out));
    assert(out.size() == 4 && out[0].date == 20261102 && out[1].date == 20261104 && out[2].date == 20261109 && out[3].date == 20261111);
    assert(out[0].minute == 600 && out[3].rid == L"w1|20261111" && out[3].repeat == 0);

    // daily until a UTC instant (the date part counts)
    out.clear();
    assert(ParseIcs(Wrap(Event("u1", { "DTSTART:20261101T080000", "RRULE:FREQ=DAILY;UNTIL=20261104T235959Z" })), from, to, out));
    assert(out.size() == 4 && out[3].date == 20261104);

    // EXDATE removes one occurrence
    out.clear();
    assert(ParseIcs(Wrap(Event("x1", { "DTSTART:20261101T080000", "RRULE:FREQ=DAILY;COUNT=5", "EXDATE:20261102T080000" })), from, to, out));
    assert(out.size() == 4 && CountDate(out, 20261102) == 0 && CountDate(out, 20261105) == 1);

    // RECURRENCE-ID: the moved occurrence replaces the one of its date
    out.clear();
    assert(ParseIcs(Wrap(Event("r1", { "DTSTART:20261101T080000", "RRULE:FREQ=DAILY;COUNT=3" }) +
                          Event("r1", { "RECURRENCE-ID:20261102T080000", "DTSTART:20261102T180000", "SUMMARY:Movida" })),
                    from, to, out));
    assert(out.size() == 3 && CountDate(out, 20261102) == 1);
    for (const CalEvent& e : out) if (e.date == 20261102) assert(e.title == L"Movida" && e.minute == 1080 && e.rid == L"r1|20261102");

    // monthly on the 31st: November and February have no 31st, so they are skipped
    out.clear();
    assert(ParseIcs(Wrap(Event("m1", { "DTSTART:20261031T090000", "RRULE:FREQ=MONTHLY" })), 20261001, 20270331, out));
    assert(out.size() == 4 && out[0].date == 20261031 && out[1].date == 20261231 && out[2].date == 20270131 && out[3].date == 20270331);

    // VALARM: -PT30M is 30 minutes before; no alarm is -1
    out.clear();
    assert(ParseIcs(Wrap("BEGIN:VEVENT\r\nUID:a1\r\nDTSTART:20261105T100000\r\nBEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT30M\r\nEND:VALARM\r\nEND:VEVENT\r\n" +
                         Event("a2", { "DTSTART:20261106T100000" })),
                    from, to, out));
    assert(out.size() == 2 && out[0].alert == 30 && out[1].alert == -1);

    // outside the window: nothing
    out.clear();
    assert(ParseIcs(Wrap(Event("o1", { "DTSTART:20250101T100000" })), from, to, out) && out.empty());

    // the cap: a daily series gives 1000 occurrences in the window, no more
    out.clear();
    assert(ParseIcs(Wrap(Event("cap", { "DTSTART:20260101T100000", "RRULE:FREQ=DAILY" })), 20260101, 20290101, out));
    assert(out.size() == 1000 && out[0].date == 20260101 && out[999].date > out[0].date);

    // a rule this does not read keeps only the first occurrence
    out.clear();
    assert(ParseIcs(Wrap(Event("s1", { "DTSTART:20261106T100000", "RRULE:FREQ=MONTHLY;BYDAY=FR;BYSETPOS=-1" })), from, to, out));
    assert(out.size() == 1 && out[0].date == 20261106);

    // a text that is not a calendar is refused, so nothing is merged from it
    out.clear();
    assert(!ParseIcs("<html>Inicia sesión</html>", from, to, out) && out.empty());

    // alarm triggers
    assert(IcsAlert("-PT30M") == 30 && IcsAlert("-PT1H") == 60 && IcsAlert("PT0M") == 0 && IcsAlert("PT10M") == -1 && IcsAlert("basura") == -1);

    // links: webcal becomes https; anything else that is not https with a host is refused
    std::wstring u = L"webcal://outlook.live.com/c.ics";
    assert(NormalizeIcsLink(u) && u == L"https://outlook.live.com/c.ics" && IcsHost(u) == L"outlook.live.com");
    u = L"https://calendar.google.com/x.ics";
    assert(NormalizeIcsLink(u) && u == L"https://calendar.google.com/x.ics");
    u = L"http://evil.com/x.ics";
    assert(!NormalizeIcsLink(u));
    u = L"https://";
    assert(!NormalizeIcsLink(u));
    u = L"";
    assert(!NormalizeIcsLink(u));

    // read-only link events in the calendar: no dirty flag, no removal, a colour change only
    EventManager m;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    m.storagePath = std::wstring(tmp) + L"capri_ics_test.txt";
    // the third line has src 4, which is not a source: it loads as an old-style Capri line, never as a link event
    WriteUtf8File(m.storagePath, L"20261110|600|660|0|0|10|Enlace\t\t3\t0\tuid1\n20261111|-1|-1|0|0|10|Local\t\t0\t0\t\n20261112|600|660|0|0|10|Mala\t\t4\t0\tuid2\n");
    m.Load();
    assert(m.events.size() == 3 && m.events[0].src == 3 && m.events[0].dirty == 0 && m.events[0].rid == L"uid1" && m.events[2].src == 0);
    CalEvent ed2 = m.events[0];
    ed2.title = L"Cambiado";
    ed2.color = 2;
    assert(m.Update(0, ed2) == 0 && m.events[0].color == 2 && m.events[0].title == L"Enlace" && m.events[0].dirty == 0);
    m.Load();
    assert(m.events[0].color == 2 && m.events[0].src == 3);
    m.Remove(0);
    assert(m.events.size() == 3 && m.On(20261110).size() == 1 && m.events[0].dirty == 0);
    m.Remove(1);
    assert(m.events.size() == 2 && m.events[0].title == L"Enlace");
    DeleteFileW(m.storagePath.c_str());

    std::printf("ics_test ok\n");
    return 0;
}
