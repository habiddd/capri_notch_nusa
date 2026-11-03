#define _WIN32_WINNT 0x0A00 // Windows 10+: exposes the per-monitor DPI APIs
#include <windows.h>
#include <roapi.h>
#include <propidl.h>
#include <gdiplus.h>
#include <cstdio>
#include "types.h"
#include "pomodoro.h"
#include "todo.h"
#include "events.h"
#include "media.h"
#include "ai_client.h"
#include "audio_wake.h"
#include "ui.h"

#define TIMER_ANIMATION 1
#define TIMER_SECOND 2
#define TIMER_MEDIA_REFRESH 3 // one-shot re-read shortly after a media button, so the new track shows fast
#define TIMER_LYRICS 4 // 100 ms lyric clock, only while lyrics are on; repaints only when the shown line changes
#define TIMER_HOVER 5  // one-shot: expand once the pointer has rested on the pill
#define TIMER_LEAVE 6  // one-shot: collapse once the pointer has stayed away
#define TIMER_TUCK 7   // 100 ms, only while tucked: untuck when the pointer leaves the zone around the notch
#define WM_NEAR_CLICK (WM_USER + 102) // posted by the mouse hook with the screen point of every left click

constexpr int HOVER_DELAY_MS = 180; // nabyscape's default hover delay
constexpr int LEAVE_DELAY_MS = 300; // grace period so a quick slip outside does not collapse
constexpr int TUCK_EDGE = 6;        // px of the pill left visible when tucked (nabyscape's edge height)
constexpr int NEAR_ZONE = 60;       // px around the pill where a click tucks it

// Pixel buffer sized for the largest notch (calendar tab, or the lyrics aside), in logical units
constexpr int BUF_W = (std::max)(LYRICS_PANEL_W, CAL_PANEL_W) + 2 * NOTCH_SHOULDER;
constexpr int BUF_H = CAL_POP_H;

class CapriNotchApp {
public:
    HWND hWnd = NULL;
    CapriUI ui;
    PomodoroManager pomo;
    TodoManager todo;
    EventManager events;
    MediaManager media;
    AICompanion ai;
    AudioWakeEngine wakeEngine;

    bool expanded = false;
    bool isTrackingMouse = false;
    bool tucked = false; // slid up out of the way after a click beside it
    int tuckY = 0;       // current slide-up offset, logical px
    std::wstring lyricSig; // last painted lyric state
    NotchTab activeTab = TAB_MEDIA;
    ULONGLONG livePaintAt = 0; // last repaint for the collapsed pill's animation
    int lastAc = -1;           // charger state last seen (0/1), -1 until known
    int reminderMinute = -1;   // minute of day the event reminders last ran for

    // Window dimensions & animation (widths include both notch shoulders)
    int currentWidth = 220 + 2 * NOTCH_SHOULDER;
    int targetWidth = 220 + 2 * NOTCH_SHOULDER;
    int currentHeight = 34;
    int targetHeight = 34;

    float scale = 1.0f; // monitor DPI / 96; sizes above are logical, the window is scale times bigger

    int screenWidth = 1920;
    int screenHeight = 1080;

    // Monitor configuration
    int monitorLeft = 0;
    int monitorTop = 0;
    int monitorWidth = 1920;

    // DIB buffer
    HDC hdcMem = NULL;
    HBITMAP hBitmap = NULL;
    void* pvBits = nullptr;
    int bufWidth = 0;
    int bufHeight = 0;

    void DetectMonitor() {
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (GetMonitorInfo(hMon, &mi)) {
            monitorLeft = mi.rcMonitor.left;
            monitorTop = mi.rcMonitor.top;
            monitorWidth = mi.rcMonitor.right - mi.rcMonitor.left;
        } else {
            monitorLeft = 0;
            monitorTop = 0;
            monitorWidth = GetSystemMetrics(SM_CXSCREEN);
        }
    }

    void SwitchMonitor() {
        struct MonData {
            std::vector<MONITORINFO> mons;
        } data;
        EnumDisplayMonitors(NULL, NULL, [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            MONITORINFO mi = { sizeof(MONITORINFO) };
            if (GetMonitorInfo(hMon, &mi)) {
                ((MonData*)lp)->mons.push_back(mi);
            }
            return TRUE;
        }, (LPARAM)&data);

        if (data.mons.size() > 1) {
            for (size_t i = 0; i < data.mons.size(); ++i) {
                if (data.mons[i].rcMonitor.left == monitorLeft && data.mons[i].rcMonitor.top == monitorTop) {
                    size_t nextIdx = (i + 1) % data.mons.size();
                    monitorLeft = data.mons[nextIdx].rcMonitor.left;
                    monitorTop = data.mons[nextIdx].rcMonitor.top;
                    monitorWidth = data.mons[nextIdx].rcMonitor.right - data.mons[nextIdx].rcMonitor.left;
                    UpdateDisplay();
                    break;
                }
            }
        }
    }

    void Init(HWND hwnd) {
        hWnd = hwnd;
        DetectMonitor();
        scale = GetDpiForWindow(hwnd) / 96.0f;
        AllocBuffer((int)(BUF_W * scale), (int)(BUF_H * scale));
        SYSTEM_POWER_STATUS ps;
        if (GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128) && ps.ACLineStatus != 255) lastAc = ps.ACLineStatus;

        // Whistle engine starts dormant/unloaded (0 MB RAM) until explicitly loaded by user button
        SetTimer(hWnd, TIMER_ANIMATION, 16, NULL); // ~60fps smooth animation
        SetTimer(hWnd, TIMER_SECOND, 1000, NULL);  // 1s logic tick
        UpdateDisplay();
    }

    void AllocBuffer(int w, int h) {
        if (hdcMem) {
            DeleteDC(hdcMem);
            hdcMem = NULL;
        }
        if (hBitmap) {
            DeleteObject(hBitmap);
            hBitmap = NULL;
        }

        bufWidth = w;
        bufHeight = h;

        HDC hdcScreen = GetDC(NULL);
        hdcMem = CreateCompatibleDC(hdcScreen);

        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = bufWidth;
        bmi.bmiHeader.biHeight = -bufHeight; // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
        SelectObject(hdcMem, hBitmap);
        ReleaseDC(NULL, hdcScreen);
    }

    void UpdateTargetDimensions() {
        if (!expanded) {
            targetWidth = ui.pillWidth + 2 * NOTCH_SHOULDER; // wide enough for the text the pill last drew
            targetHeight = 34;
        } else {
            bool aside = activeTab == TAB_MEDIA && media.showLyrics, cal = activeTab == TAB_CALENDAR;
            targetWidth = (cal ? CAL_PANEL_W : aside ? LYRICS_PANEL_W : PANEL_W) + 2 * NOTCH_SHOULDER;
            targetHeight = cal ? (ui.calPop ? CAL_POP_H : CAL_PANEL_H) : 240;
        }
    }

    void SetExpanded(bool exp) {
        if (expanded == exp) return;
        expanded = exp;
        if (!exp) {
            ui.editing = false; // the event window, if open, stays with its draft
            ui.calSel = 0; // the calendar reopens on today (in the view it was left on)
            ui.calEvent = -1;
        }
        ui.CalHome();
        UpdateTargetDimensions();
    }

    void UpdateDisplay() {
        if (!pvBits || bufWidth <= 0 || bufHeight <= 0) return;

        int stride = bufWidth * 4;
        int pw = (int)(currentWidth * scale);
        int ph = (int)(currentHeight * scale);

        // Render UI with correct scanline stride
        ui.Render(pvBits, stride, pw, ph, scale, expanded, activeTab, pomo, todo, events, media, ai, wakeEngine);
        if (!expanded && targetWidth != ui.pillWidth + 2 * NOTCH_SHOULDER) UpdateTargetDimensions(); // new text: the animation tick resizes

        int posX = monitorLeft + (monitorWidth - pw) / 2;
        // shortcut: tucking slides the window above the monitor's top edge; on a monitor stacked above this one the
        // hidden part would show there. Upgrade to rendering the visible strip only if that layout matters.
        int posY = monitorTop - (int)(tuckY * scale);

        // Keep window bounds and hit-testing in sync with visual notch shape
        SetWindowPos(hWnd, HWND_TOPMOST, posX, posY, pw, ph, SWP_NOACTIVATE);

        // Update layered window
        HDC hdcScreen = GetDC(NULL);
        POINT ptSrc = {0, 0};
        SIZE sizeWnd = {pw, ph};
        POINT ptDst = {posX, posY};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

        UpdateLayeredWindow(hWnd, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
        ReleaseDC(NULL, hdcScreen);
    }

    void OnAnimTick() {
        bool changed = false;
        // Smooth easing towards target
        if (currentWidth != targetWidth) {
            int diff = targetWidth - currentWidth;
            int step = diff / 4;
            if (step == 0) step = (diff > 0) ? 1 : -1;
            currentWidth += step;
            changed = true;
        }
        if (currentHeight != targetHeight) {
            int diff = targetHeight - currentHeight;
            int step = diff / 4;
            if (step == 0) step = (diff > 0) ? 1 : -1;
            currentHeight += step;
            changed = true;
        }
        int tuckTarget = tucked ? currentHeight - TUCK_EDGE : 0;
        if (tuckY != tuckTarget) {
            int diff = tuckTarget - tuckY;
            int step = diff / 4;
            if (step == 0) step = (diff > 0) ? 1 : -1;
            tuckY += step;
            changed = true;
        }

        ULONGLONG now = GetTickCount64();
        bool live = ui.PillLive(expanded, media.isPlaying && media.currentTitle != media.idleTitle) && now - livePaintAt >= 50;
        if (live) livePaintAt = now;
        if (changed || live || ui.Animating()) {
            UpdateDisplay();
        }
    }

    // Charger plugged/unplugged: show a short banner on the pill. Desktops (no battery) are ignored.
    void OnPowerChange() {
        SYSTEM_POWER_STATUS ps;
        if (!GetSystemPowerStatus(&ps) || (ps.BatteryFlag & 128) || ps.ACLineStatus == 255) return;
        int prev = lastAc;
        lastAc = ps.ACLineStatus;
        if (prev < 0 || prev == lastAc) return;
        ui.notice = lastAc ? L"Cargando" : L"Sin cargador";
        ui.noticeRight = ps.BatteryLifePercent == 255 ? L"" : std::to_wstring(ps.BatteryLifePercent) + L"%";
        ui.noticeIcon = lastAc ? L"" : L"";
        ui.noticeColor = lastAc ? Gdiplus::Color(255, 74, 222, 128) : Gdiplus::Color(255, 242, 242, 245);
        ui.noticeUntil = GetTickCount64() + 3000;
        UpdateDisplay();
    }

    void OnLyricsTick() {
        MediaManager::LyricView v = media.Lyrics();
        std::wstring sig = v.status + L"\n" + std::to_wstring(v.index) + L"\n" + v.cur;
        if (sig != lyricSig) {
            lyricSig = sig;
            UpdateTargetDimensions();
            UpdateDisplay();
        }
    }

    // Once a minute: an event's early warning or its start takes over the pill for 8 s (categories switched off stay silent)
    void CheckReminders() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        int nowMin = st.wHour * 60 + st.wMinute;
        if (nowMin == reminderMinute) return;
        reminderMinute = nowMin;
        for (size_t i : events.On(CapriUI::TodayKey())) {
            const CalEvent& e = events.events[i];
            int due = AlertDue(e, nowMin);
            if (!due || ui.calHidden[e.color]) continue;
            const CalCategory& c = CAL_CATS[e.color];
            ui.notice = (due == 2 ? L"Ahora · " : e.alert == 60 ? L"En 1 h · " : L"En " + std::to_wstring(e.alert) + L" min · ") + e.title;
            ui.noticeRight = EventTime(e.minute);
            ui.noticeIcon = L"";
            ui.noticeColor = Gdiplus::Color(255, c.r, c.g, c.b);
            ui.noticeUntil = GetTickCount64() + 8000;
            MessageBeep(MB_ICONASTERISK);
            break; // shortcut: one reminder per minute; the later of two simultaneous events is skipped, upgrade to queueing them
        }
    }

    void OnSecondTick() {
        CheckReminders();
        pomo.Tick();
        media.DetectActiveTrack();
        UpdateTargetDimensions(); // playback pause/resume switches the collapsed notch between lyric and title width
        UpdateDisplay();
    }

    bool CursorOver() {
        POINT pt;
        GetCursorPos(&pt);
        return WindowFromPoint(pt) == hWnd; // transparent corners of the layered window count as outside
    }

    // Untucked pill bounds grown by NEAR_ZONE on the sides and below (screen px)
    RECT NearZone() {
        int pw = (int)(currentWidth * scale), ph = (int)(currentHeight * scale), m = (int)(NEAR_ZONE * scale);
        int x = monitorLeft + (monitorWidth - pw) / 2;
        return { x - m, monitorTop, x + pw + m, monitorTop + ph + m };
    }

    void SetTucked(bool t) {
        if (tucked == t) return;
        tucked = t;
        if (t) {
            KillTimer(hWnd, TIMER_HOVER);
            SetExpanded(false);
            SetTimer(hWnd, TIMER_TUCK, 100, NULL);
        } else {
            KillTimer(hWnd, TIMER_TUCK);
        }
    }

    // A click beside the notch (not on it) tucks it away so the spot underneath stays usable; not while lyrics are on.
    void OnNearClick(POINT pt) {
        if (tucked || media.showLyrics) return;
        RECT win, zone = NearZone();
        GetWindowRect(hWnd, &win);
        if (!PtInRect(&win, pt) && PtInRect(&zone, pt)) SetTucked(true);
    }

    void OnTuckTick() {
        POINT pt;
        GetCursorPos(&pt);
        RECT zone = NearZone();
        if (!PtInRect(&zone, pt)) SetTucked(false);
    }

    // Moves the task list by `rows` and keeps it in range (also after deletes)
    void ScrollTodo(int rows) {
        size_t n = todo.tasks.size(), maxTop = n > CapriUI::TODO_ROWS ? n - CapriUI::TODO_ROWS : 0;
        long long top = (long long)ui.todoTop + rows;
        ui.todoTop = (size_t)(std::max)(0LL, (std::min)((long long)maxTop, top));
        UpdateDisplay();
    }

    void CommitTodo() {
        std::wstring t = ui.todoDraft;
        t.erase(0, t.find_first_not_of(L' '));
        t.erase(t.find_last_not_of(L' ') + 1);
        ui.todoDraft.clear();
        size_t at = todo.Add(t, ui.todoParent); // ignores empty text; the parent stays set for the next subtask
        if (at == SIZE_MAX) return UpdateDisplay();
        // Scroll just enough to bring the new row into view
        long long top = (long long)ui.todoTop, a = (long long)at;
        ScrollTodo((int)(a < top ? a - top : a >= top + (long long)CapriUI::TODO_ROWS ? a - top - (long long)CapriUI::TODO_ROWS + 1 : 0));
    }

    // Saves the event window's draft (or rewrites the event it was opened on) and jumps to its day; a blank title does nothing
    void SavePopup() {
        CalEvent e = ui.popEv;
        ApplyTitleTokens(e, CapriUI::TodayKey()); // also trims the title
        if (e.title.empty()) return;
        ui.calEvent = (int)(ui.popEdit >= 0 ? events.Update(ui.popEdit, e) : events.Add(e));
        ui.CalSelect(e.date);
        ClosePopup();
    }

    void ClosePopup() {
        ui.calPop = false;
        ui.editing = false;
        UpdateTargetDimensions();
    }

    // Typing into the open tab's field: a task, or the focused field of the event window
    void OnChar(wchar_t ch) {
        bool cal = activeTab == TAB_CALENDAR;
        if (cal && !ui.calPop) return;
        std::wstring& d = cal ? (ui.popFocus ? ui.popEv.notes : ui.popEv.title) : ui.todoDraft;
        if (ch == L'\r') {
            if (!cal) return CommitTodo();
            SavePopup();
        } else if (ch == 27) { // Esc
            if (cal) {
                ClosePopup();
            } else {
                ui.editing = false;
                ui.todoParent = SIZE_MAX;
                d.clear();
            }
        } else if (ch == L'\t') {
            if (cal) ui.popFocus ^= 1;
        } else if (ch == L'\b') {
            if (!d.empty()) d.pop_back();
            if (!d.empty() && IS_HIGH_SURROGATE(d.back())) d.pop_back(); // whole emoji, not half of it
        } else if (ch >= 32 && ch != 127 && d.size() < 200) {
            d += ch;
        }
        UpdateDisplay();
    }

    void StartTyping() {
        ui.editing = true;
        SetForegroundWindow(hWnd);
        SetFocus(hWnd);
    }

    void OpenPopup(CalEvent e, int editIdx) {
        if (e.minute >= 0 && e.end <= e.minute) e.end = (std::min)(1440, e.minute + 60); // no end stored = drawn as an hour
        ui.popEv = e;
        ui.popEdit = editIdx;
        ui.popFocus = 0;
        ui.calPop = true;
        UpdateTargetDimensions();
        StartTyping();
    }

    // One of the event window's controls (see CapriUI::HIT_POP_*)
    void OnPopHit(const CapriUI::CalHit& h) {
        using C = CapriUI;
        CalEvent& e = ui.popEv;
        StartTyping(); // any click inside the window gives it the keyboard back
        switch (h.kind) {
        case C::HIT_POP_BACK: return ClosePopup();
        case C::HIT_POP_CANCEL: return ClosePopup();
        case C::HIT_POP_SAVE: return SavePopup();
        case C::HIT_POP_DELETE:
            events.Remove(ui.popEdit);
            ui.calEvent = -1;
            return ClosePopup();
        case C::HIT_POP_TITLE: ui.popFocus = 0; return;
        case C::HIT_POP_NOTES: ui.popFocus = 1; return;
        case C::HIT_POP_DATE: e.date = h.a; return;
        case C::HIT_POP_ALLDAY:
            e.minute = e.minute < 0 ? 540 : -1;
            e.end = e.minute < 0 ? -1 : 600;
            return;
        case C::HIT_POP_START: { // moves the whole block, keeping its length
            int len = e.end - e.minute;
            e.minute = (std::max)(0, (std::min)(1425, e.minute + h.a));
            e.end = (std::max)(e.minute + 15, (std::min)(1440, e.minute + len));
            return;
        }
        case C::HIT_POP_END: e.end = (std::max)((std::min)(e.minute + 15, 1440), (std::min)(1440, e.end + h.a)); return;
        case C::HIT_POP_REPEAT: e.repeat = h.a; return;
        case C::HIT_POP_COLOR: e.color = h.a; return;
        case C::HIT_POP_ALERT: e.alert = h.a; return;
        }
    }

    // A click on one of the areas the calendar registered while painting (see CapriUI::CalHit)
    void OnCalHit(const CapriUI::CalHit& h) {
        using C = CapriUI;
        if (ui.calPop) return OnPopHit(h); // the window covers the calendar: only its own areas are registered
        switch (h.kind) {
        case C::HIT_DAY:
            if (h.b && h.a == ui.SelKey()) { // a second click on a month cell opens that day
                ui.calView = C::CAL_DAY;
                ui.CalHome();
            } else {
                ui.CalSelect(h.a);
            }
            return;
        case C::HIT_PREV: return ui.CalPage(-1, h.a != 0);
        case C::HIT_NEXT: return ui.CalPage(1, h.a != 0);
        case C::HIT_TODAY: return ui.CalSelect(0);
        case C::HIT_VIEW:
            ui.calView = (C::CalView)h.a;
            ui.calAgendaTop = 0;
            return ui.CalHome();
        case C::HIT_CAT: ui.calHidden[h.a] = !ui.calHidden[h.a]; return;
        case C::HIT_EVENT:
            ui.calEvent = h.a;
            return;
        case C::HIT_SLOT: // empty hour: start a new event there
            ui.CalSelect(h.a);
            return OpenPopup({ h.a, h.b, (std::min)(1440, h.b + 60), 0, 0, L"" }, -1);
        case C::HIT_NEW: { // the selected day; today starts at the next full hour
            SYSTEMTIME st;
            GetLocalTime(&st);
            int day = ui.SelKey(), start = day == CapriUI::TodayKey() ? (std::min)(23, st.wHour + 1) * 60 : 540;
            return OpenPopup({ day, start, start + 60, 0, 0, L"" }, -1);
        }
        }

        // Detail strip actions on the selected event
        if (ui.calEvent < 0 || ui.calEvent >= (int)events.events.size()) return;
        CalEvent e = events.events[ui.calEvent];
        if (h.kind == C::HIT_EDIT) {
            ui.CalSelect(e.date);
            return OpenPopup(e, ui.calEvent);
        }
        if (h.kind == C::HIT_DELETE) {
            events.Remove(ui.calEvent);
            ui.calEvent = -1;
            return;
        }
        if (h.kind == C::HIT_COLOR) e.color = h.a;
        else if (h.kind == C::HIT_MOVE) e.date = TmKey(KeyTm(e.date, h.a));
        else if (h.kind == C::HIT_REPEAT) e.repeat = (e.repeat + 1) % 5;
        ui.calEvent = (int)events.Update(ui.calEvent, e);
    }

    void OnClick(int x, int y) {
        if (tucked) { // clicking the visible edge brings it straight back
            SetTucked(false);
            SetExpanded(true);
            return;
        }
        if (!expanded) {
            SetExpanded(true);
            return;
        }

        // Hit-test in content space, which starts after the left shoulder
        x -= NOTCH_SHOULDER;
        int w = currentWidth - 2 * NOTCH_SHOULDER;

        // Icon tab row (y: 8 - 38)
        if (y >= 8 && y <= 38) {
            for (int i = 0; i < TAB_COUNT; ++i) {
                float tabX = 16.0f + i * 46.0f;
                if (x >= tabX && x <= tabX + 40.0f) {
                    activeTab = (NotchTab)i;
                    ui.editing = false;
                    if (i == TAB_CALENDAR) ui.CalHome();
                    UpdateTargetDimensions();
                    UpdateDisplay();
                    return;
                }
            }
        }

        // Specific tab interactions
        if (activeTab == TAB_MEDIA) {
            int col = CapriUI::MediaColumn(w); // same column the renderer used
            float midX = CapriUI::TransportCenter(col);
            if (y >= 114 && y <= 156 && x >= col - 60 && x <= col - 20) {
                // Lyrics toggle, right end of the transport row
                media.ToggleLyrics();
                if (media.showLyrics) SetTimer(hWnd, TIMER_LYRICS, 100, NULL);
                else KillTimer(hWnd, TIMER_LYRICS);
                UpdateTargetDimensions();
                UpdateDisplay();
            } else if (y >= 114 && y <= 156) {
                // Media buttons: [|<] [>||] [>|]
                if (x >= midX - 80 && x <= midX - 34) media.Previous();
                else if (x >= midX - 26 && x <= midX + 26) media.PlayPause();
                else if (x >= midX + 34 && x <= midX + 80) media.Next();
                SetTimer(hWnd, TIMER_MEDIA_REFRESH, 400, NULL);
                UpdateDisplay();
            } else if (media.duration > 0 && y >= 176 && y <= 200 && x >= 20 && x <= col - 20) {
                // Timeline: jump to the clicked point
                media.Seek((x - 20) / (double)(col - 40) * media.duration);
                SetTimer(hWnd, TIMER_MEDIA_REFRESH, 400, NULL);
                UpdateDisplay();
            } else if (media.showLyrics && y >= 194 && y <= 216 && x >= w - 100 && media.Lyrics().retry) {
                media.RetryLyrics();
                UpdateDisplay();
            }
        } else if (activeTab == TAB_POMODORO) {
            // Round play/pause (210..254) and reset (266..310) discs, y 104..148
            if (y >= 104 && y <= 148) {
                if (x >= 210 && x <= 254) pomo.Toggle();
                else if (x >= 266 && x <= 310) pomo.Reset();
                UpdateDisplay();
            }
        } else if (activeTab == TAB_TASKS) {
            // Task rows (y from 82, step 26): X deletes, + (top-level only) starts a subtask, anywhere else toggles
            if (y >= 82 && y < 82 + (int)CapriUI::TODO_ROWS * 26) {
                size_t idx = ui.todoTop + (y - 82) / 26;
                if (idx >= todo.tasks.size()) return;
                if (x >= w - 44) {
                    todo.Remove(idx);
                    ui.todoParent = SIZE_MAX; // indices shifted
                } else if (x >= w - 68 && !todo.tasks[idx].sub) {
                    ui.todoParent = idx;
                    ui.editing = true;
                    SetForegroundWindow(hWnd);
                    SetFocus(hWnd);
                } else {
                    todo.Toggle(idx);
                }
                ScrollTodo(0);
            } else if (y >= 196 && y <= 226 && x >= 20 && x <= w - 20) {
                // Add field: the + adds what was typed, the rest of the field starts typing (a normal task unless
                // a subtask is already being typed)
                if (x >= w - 48 && !ui.todoDraft.empty()) CommitTodo();
                if (!ui.editing) ui.todoParent = SIZE_MAX;
                ui.editing = true;
                SetForegroundWindow(hWnd);
                SetFocus(hWnd);
                UpdateDisplay();
            }
        } else if (activeTab == TAB_CALENDAR) {
            // The top-most area the last paint registered under the pointer wins
            for (auto it = ui.calHits.rbegin(); it != ui.calHits.rend(); ++it)
                if (it->r.Contains((float)x, (float)y)) {
                    OnCalHit(*it);
                    break;
                }
            UpdateDisplay();
        } else if (activeTab == TAB_AI) {
            // Whistle on-demand load/unload button (y: 70..98)
            if (y >= 70 && y <= 98 && x >= 20 && x <= w - 20) {
                wakeEngine.Toggle([this](const std::wstring& transcript) {
                    PostMessage(hWnd, WM_USER + 100, 0, 0);
                });
                UpdateDisplay();
            } else if (y >= 192 && y <= 222) {
                // Quick prompts (y: 192..222)
                if (x >= 24 && x <= 194) {
                    ai.Ask(L"Resume brevemente el enfoque para hoy");
                } else if (x >= 204 && x <= 374) {
                    ai.Ask(L"Dame un consejo rapido para concentrarme mejor");
                }
                UpdateDisplay();
            }
        }
    }
};

static CapriNotchApp g_App;

// Sees every left click on the desktop so a click beside the notch can tuck it. It only forwards the point;
// the decision runs on the UI thread. Never blocks or swallows the click.
static LRESULT CALLBACK NearClickHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_LBUTTONDOWN) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lParam;
        PostMessageW(g_App.hWnd, WM_NEAR_CLICK, 0, MAKELPARAM(m->pt.x, m->pt.y));
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_App.Init(hWnd);
        break;

    case WM_TIMER:
        if (wParam == TIMER_ANIMATION) {
            g_App.OnAnimTick();
        } else if (wParam == TIMER_SECOND) {
            g_App.OnSecondTick();
        } else if (wParam == TIMER_LYRICS) {
            g_App.OnLyricsTick();
        } else if (wParam == TIMER_MEDIA_REFRESH) {
            KillTimer(hWnd, TIMER_MEDIA_REFRESH);
            g_App.media.DetectActiveTrack();
            g_App.UpdateDisplay();
        } else if (wParam == TIMER_HOVER) {
            KillTimer(hWnd, TIMER_HOVER);
            if (!g_App.tucked && g_App.CursorOver()) g_App.SetExpanded(true);
        } else if (wParam == TIMER_LEAVE) {
            KillTimer(hWnd, TIMER_LEAVE);
            if (!g_App.CursorOver() && !g_App.ui.editing) g_App.SetExpanded(false); // typing keeps it open
        } else if (wParam == TIMER_TUCK) {
            g_App.OnTuckTick();
        }
        break;

    case WM_MOUSEMOVE:
        if (!g_App.isTrackingMouse) {
            TRACKMOUSEEVENT tme = {0};
            tme.cbSize = sizeof(TRACKMOUSEEVENT);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hWnd;
            TrackMouseEvent(&tme);
            g_App.isTrackingMouse = true;
            if (!g_App.expanded && !g_App.tucked) SetTimer(hWnd, TIMER_HOVER, HOVER_DELAY_MS, NULL);
        }
        KillTimer(hWnd, TIMER_LEAVE); // back inside before the grace period ran out
        break;

    case WM_MOUSELEAVE:
        g_App.isTrackingMouse = false;
        KillTimer(hWnd, TIMER_HOVER);
        if (g_App.expanded) SetTimer(hWnd, TIMER_LEAVE, LEAVE_DELAY_MS, NULL);
        break;

    case WM_NEAR_CLICK: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) }; // signed: monitors left of/above the primary
        g_App.OnNearClick(pt);
        break;
    }

    case WM_LBUTTONDOWN: {
        int x = (int)(LOWORD(lParam) / g_App.scale);
        int y = (int)(HIWORD(lParam) / g_App.scale);
        g_App.OnClick(x, y);
        break;
    }

    case WM_CHAR:
        if (g_App.ui.editing) g_App.OnChar((wchar_t)wParam);
        break;

    case WM_MOUSEWHEEL:
        if (g_App.expanded && g_App.activeTab == TAB_TASKS) g_App.ScrollTodo(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
        if (g_App.expanded && g_App.activeTab == TAB_CALENDAR && !g_App.ui.calPop) {
            g_App.ui.CalWheel(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
            g_App.UpdateDisplay();
        }
        break;

    case WM_ACTIVATE: // clicked away while typing: stop, and close if the pointer is gone too
        if (LOWORD(wParam) == WA_INACTIVE && g_App.ui.editing) {
            g_App.ui.editing = false;
            if (!g_App.CursorOver()) g_App.SetExpanded(false);
            g_App.UpdateDisplay();
        }
        break;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMPOWERSTATUSCHANGE) g_App.OnPowerChange();
        return TRUE;

    case WM_DPICHANGED: // moved to a monitor with different scaling: redraw crisp at the new density
        g_App.scale = HIWORD(wParam) / 96.0f;
        g_App.AllocBuffer((int)(BUF_W * g_App.scale), (int)(BUF_H * g_App.scale));
        g_App.UpdateDisplay();
        break;

    case WM_RBUTTONUP: {
        POINT pt;
        GetCursorPos(&pt);
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, 1001, g_App.expanded ? L"Colapsar Notch" : L"Expandir Notch");
        AppendMenuW(hMenu, MF_STRING, 1002, L"Mover al otro monitor");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, 1003, L"Salir de Capri Notch");

        SetForegroundWindow(hWnd);
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
        DestroyMenu(hMenu);

        if (cmd == 1001) {
            g_App.SetExpanded(!g_App.expanded);
        } else if (cmd == 1002) {
            g_App.SwitchMonitor();
        } else if (cmd == 1003) {
            DestroyWindow(hWnd);
        }
        break;
    }

    case WM_USER + 100: // Audio wake trigger
        g_App.SetExpanded(true);
        g_App.activeTab = TAB_AI;
        g_App.UpdateDisplay();
        break;

    case WM_USER + 101: // Re-open or expand request from duplicate launch
        g_App.SetTucked(false);
        g_App.SetExpanded(true);
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        g_App.UpdateDisplay();
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Quick debug log
    FILE* dbg = fopen("C:\\capri_debug.txt", "w");
    if (dbg) { fprintf(dbg, "START\n"); fflush(dbg); }

    // WinRT init for GSMTC media detection
    HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);
    if (dbg) { fprintf(dbg, "RoInit: 0x%08X\n", (unsigned)roHr); fflush(dbg); }
    if (FAILED(roHr)) {
        // Non-fatal: media detection won't work but app should still run
    }

    // Initialize GDI+ in main thread before window creation
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gsi, NULL);
    if (dbg) { fprintf(dbg, "GDI+ OK\n"); fflush(dbg); }

    // Single instance check
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"CapriNotch_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (dbg) { fprintf(dbg, "MUTEX EXISTS - exiting\n"); fclose(dbg); }
        HWND hExisting = FindWindowW(L"CapriNotchWindowClass", NULL);
        if (hExisting) {
            PostMessageW(hExisting, WM_USER + 101, 0, 0);
            SetForegroundWindow(hExisting);
        }
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 0;
    }

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"CapriNotchWindowClass";

    if (!RegisterClassExW(&wc)) {
        if (dbg) { fprintf(dbg, "RegisterClass FAIL: %lu\n", GetLastError()); fclose(dbg); }
        return 0;
    }
    if (dbg) { fprintf(dbg, "RegisterClass OK\n"); fflush(dbg); }

    // Without this Windows bitmap-stretches the notch on scaled displays (125%, 150%...) and it looks blurry
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_App.DetectMonitor();
    int initW = 220 + 2 * NOTCH_SHOULDER;
    int initH = 34;
    int posX = g_App.monitorLeft + (g_App.monitorWidth - initW) / 2;
    int posY = g_App.monitorTop;

    HWND hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"Capri Notch",
        WS_POPUP | WS_VISIBLE,
        posX, posY, initW, initH,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) {
        if (dbg) { fprintf(dbg, "CreateWindow FAIL: %lu\n", GetLastError()); fclose(dbg); }
        return 0;
    }
    if (dbg) { fprintf(dbg, "CreateWindow OK hwnd=%p\n", (void*)hWnd); fflush(dbg); }

    // Low-level hooks run on the installing thread's message loop; a thread of its own keeps system-wide mouse
    // input from waiting on the UI thread, which can block briefly on media calls.
    std::thread([] {
        HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, NearClickHook, GetModuleHandleW(NULL), 0);
        MSG m;
        while (GetMessageW(&m, NULL, 0, 0)) {}
        if (hook) UnhookWindowsHookEx(hook);
    }).detach();

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    // Minimize working set to keep RAM footprint minimal
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hMutex) CloseHandle(hMutex);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    RoUninitialize();
    return (int)msg.wParam;
}
