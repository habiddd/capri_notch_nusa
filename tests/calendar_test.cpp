// g++ -std=c++17 -Wall tests/calendar_test.cpp -o tests/calendar_test.exe -lshell32 && tests/calendar_test.exe
#include <windows.h>
#include <cassert>
#include "../src/events.h"

static bool Parse(const wchar_t* s, int base, CalEvent& e) { return ParseQuickEvent(s, base, 20261103, e); } // 2026-11-03 is a Tuesday

int main() {
    CalEvent e;
    assert(Parse(L"Dentista mañana 15:30 #salud", 20261103, e));
    assert(e.date == 20261104 && e.minute == 930 && e.end == -1 && e.color == 3 && e.repeat == 0 && e.title == L"Dentista");

    assert(Parse(L"Clase 9:00-10:30 cada semana", 20261110, e));
    assert(e.date == 20261110 && e.minute == 540 && e.end == 630 && e.repeat == 2 && e.title == L"Clase");

    assert(Parse(L"Cumple Ana 15/11 anual", 20261103, e));
    assert(e.date == 20261115 && e.minute == -1 && e.repeat == 4 && e.title == L"Cumple Ana");

    assert(Parse(L"lunes reunión 10h #trab", 20261103, e));
    assert(e.date == 20261109 && e.minute == 600 && e.color == 1 && e.title == L"reunión");

    assert(Parse(L"#xyz algo", 20261103, e) && e.color == 0 && e.title == L"#xyz algo");
    assert(!Parse(L"15:30", 20261103, e));
    assert(Parse(L"Pasado Mañana x", 20261103, e) && e.date == 20261105 && e.title == L"x");
    assert(Parse(L"x 1/10", 20261103, e) && e.date == 20271001); // already passed: next year
    assert(Parse(L"x 31/4 10:00-09:00", 20261103, e) && e.date == 20261103 && e.title == L"x 31/4" && e.end == -1);

    CalEvent t{ 20261110, 540, 600, 0, 0, L"Dentista mañana 15:30 #salud" };
    ApplyTitleTokens(t, 20261103);
    assert(t.date == 20261104 && t.minute == 930 && t.end == 990 && t.color == 3 && t.repeat == 0 && t.title == L"Dentista");
    t = { 20261110, 540, 600, 0, 0, L"Cena con Ana" };
    ApplyTitleTokens(t, 20261103);
    assert(t.date == 20261110 && t.minute == 540 && t.end == 600 && t.color == 0 && t.title == L"Cena con Ana");
    t = { 20261110, 540, 600, 0, 0, L"Clase 9:00-10:30" };
    ApplyTitleTokens(t, 20261103);
    assert(t.date == 20261110 && t.minute == 540 && t.end == 630 && t.title == L"Clase");

    CalEvent a{ 20261105, 930, 990, 0, 0, L"x" }; // alert 10
    assert(AlertDue(a, 920) == 1 && AlertDue(a, 930) == 2 && AlertDue(a, 925) == 0);
    a.alert = -1;
    assert(AlertDue(a, 920) == 0 && AlertDue(a, 930) == 0);
    a.alert = 0;
    assert(AlertDue(a, 930) == 2 && AlertDue(a, 920) == 0);
    a = { 20261105, -1, -1, 0, 0, L"x" };
    assert(AlertDue(a, 0) == 0);
    CalEvent weekly{ 20261125, 600, -1, 0, 2, L"w" }, monthly{ 20261103, -1, -1, 0, 3, L"m" };
    assert(Occurs(weekly, 20261125) && Occurs(weekly, 20261202) && !Occurs(weekly, 20261128) && !Occurs(weekly, 20261118));
    assert(Occurs(monthly, 20261203) && !Occurs(monthly, 20261204) && !Occurs(monthly, 20261003));

    // Old and new lines load side by side (storage is redirected: never touch the user's real file)
    EventManager m;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    m.storagePath = std::wstring(tmp) + L"capri_events_test.txt";
    WriteUtf8File(m.storagePath, L"20261103|600|Dentista\n20261104|540|630|1|2|Clase|con barra\nbasura\n20261105|5\n20261106|600|700|2|0|30|Con aviso\tmis notas\n20261107|600|700|2|0|7|Alerta rara\n");
    m.Load();
    assert(m.events.size() == 3); // the bad lines and the invalid alert are dropped
    assert(m.events[0].title == L"Dentista" && m.events[0].end == -1 && m.events[0].color == 0 && m.events[0].repeat == 0 && m.events[0].alert == 10 && m.events[0].notes.empty());
    assert(m.events[1].title == L"Clase|con barra" && m.events[1].end == 630 && m.events[1].color == 1 && m.events[1].repeat == 2 && m.events[1].alert == 10 && m.events[1].notes.empty());
    assert(m.events[2].title == L"Con aviso" && m.events[2].alert == 30 && m.events[2].notes == L"mis notas");
    size_t at = m.Add({ 20261103, -1, -1, 4, 0, L"Todo el dia" });
    assert(at == 0 && m.On(20261103).front() == 0);
    m.events.clear();
    m.Load(); // Save wrote the new format; reading it back keeps everything
    assert(m.events.size() == 4 && m.events[0].color == 4 && m.events[1].repeat == 0 && m.events[2].repeat == 2);
    assert(m.events[3].alert == 30 && m.events[3].notes == L"mis notas" && m.events[3].title == L"Con aviso");
    // Titles that look like numbers and bars survive a save/load once the file carries the header
    m.events.clear();
    m.Add({ 20261108, 600, 660, 0, 0, L"5|x" });
    m.Add({ 20261109, -1, -1, 0, 0, L"12|2|3|4|5" });
    m.events.clear();
    m.Load();
    assert(m.events.size() == 2 && m.events[0].title == L"5|x" && m.events[0].alert == 10 && m.events[1].title == L"12|2|3|4|5");

    // Remote events keep their source, id and pending bits; a pending delete stays in the file but is not shown
    m.events.clear();
    CalEvent g{ 20261110, 600, 660, 1, 0, L"Remoto", 10, L"nota" };
    g.src = 1;
    g.rid = L"gid1";
    m.Add(g); // a remote event made here goes up on the next sync
    m.events.clear();
    m.Load();
    assert(m.events.size() == 1 && m.events[0].src == 1 && m.events[0].rid == L"gid1" && m.events[0].dirty == 1 && m.events[0].notes == L"nota");
    WriteUtf8File(m.storagePath, L"20261110|600|660|1|0|10|Remoto\tnota\t1\t0\tgid1\n20261111|-1|-1|0|0|10|Local\t\t0\t0\t\n20261112|-1|-1|0|0|10|Vieja\n");
    m.Load();
    assert(m.events.size() == 3 && m.events[0].dirty == 0 && m.events[0].src == 1 && m.events[1].src == 0 && m.events[1].rid.empty());
    assert(m.events[2].title == L"Vieja" && m.events[2].src == 0 && m.events[2].dirty == 0);
    // editing a clean remote event marks it for update; a notes change marks the notes too
    CalEvent ed = m.events[0];
    ed.title = L"Editado";
    m.Update(0, ed);
    assert(m.events[0].dirty == 2 && m.events[0].rev == 1 && m.events[0].src == 1 && m.events[0].rid == L"gid1");
    ed = m.events[0];
    ed.notes = L"otra";
    m.Update(0, ed);
    assert(m.events[0].dirty == 10);
    // deleting a remote event hides it until the sync deletes it; a local one is removed at once
    m.Remove(0);
    assert(m.events.size() == 3 && (m.events[0].dirty & 4) && m.On(20261110).empty() && m.On(20261111).size() == 1);
    m.events.clear();
    m.Load();
    assert(m.events.size() == 3 && m.events[0].dirty == 14 && m.On(20261110).empty());
    m.Remove(1);
    assert(m.events.size() == 2 && m.events[1].title == L"Vieja");

    // Several alerts: the mask round-trips, the longest lead is the main one, and each chosen lead fires on its own
    for (int mask = 0; mask < 16; ++mask) {
        int al = 0, mo = 0;
        SetAlertMask(mask, al, mo);
        assert(AlertMask(al, mo) == mask && (mask ? al >= 0 : al == -1) && !(mo & AlertBit(al)));
    }
    int al = 0, mo = 0;
    SetAlertMask(5, al, mo); // 30 min and at start
    assert(al == 30 && mo == 1);
    CalEvent multi{ 20261105, 930, 990, 0, 0, L"x", 10 }; // 10 min and 1 h
    multi.moreAlerts = AlertBit(60);
    int lead = -1;
    assert(AlertDue(multi, 870, &lead) == 1 && lead == 60);
    assert(AlertDue(multi, 920, &lead) == 1 && lead == 10);
    assert(AlertDue(multi, 930, &lead) == 2 && lead == 0);
    assert(AlertDue(multi, 900) == 0);

    // Which tasks the Tareas tab lists: open ones always, done ones from their day on; events never; deleted ones never
    CalEvent open{ 20261101, -1, -1, 0, 0, L"x" }, done = open, doneToday = open, plain = open, gone = open;
    open.task = 1;
    done.task = 2;
    doneToday.task = 2;
    doneToday.date = 20261103;
    gone.task = 1;
    gone.dirty = 4;
    assert(TaskListed(open, 20261103) && !TaskListed(done, 20261103) && TaskListed(doneToday, 20261103));
    assert(!TaskListed(plain, 20261103) && !TaskListed(gone, 20261103));

    // Tasks and extra alerts survive save/load; a 7-field line with a task value out of range is skipped
    m.events.clear();
    CalEvent todo{ 20261114, -1, -1, 0, 0, L"Hacer algo" };
    todo.task = 2;
    todo.moreAlerts = 4;
    m.Add(todo);
    m.events.clear();
    m.Load();
    assert(m.events.size() == 1 && m.events[0].task == 2 && m.events[0].moreAlerts == 4 && m.events[0].alert == 10);
    WriteUtf8File(m.storagePath, L"20261112|600|660|1|0|10|Tarea\t\t0\t0\t\t1\t10\n20261113|600|660|1|0|10|Mala\t\t0\t0\t\t7\t0\n");
    m.Load();
    assert(m.events.size() == 1 && m.events[0].task == 1 && m.events[0].moreAlerts == 10 && m.events[0].title == L"Tarea");
    DeleteFileW(m.storagePath.c_str());
    return 0;
}
