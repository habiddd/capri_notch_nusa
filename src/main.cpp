#define _WIN32_WINNT 0x0A00 // Windows 10+: exposes the per-monitor DPI APIs
#include <windows.h>
#include <roapi.h>
#include <propidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <objbase.h>
#include <ole2.h>
#include <locationapi.h>
#include "types.h"
#include "tabs.h"
#include "pomodoro.h"
#include "todo.h"
#include "shelf.h"
#include "events.h"
#include "media.h"
#include "ai_client.h"
#include "whistle.h"
#include "ui.h"
#include "sync.h"
#include "settings.h"
#include "settings_ui.h"

#define TIMER_ANIMATION 1
#define TIMER_SECOND 2
#define TIMER_MEDIA_REFRESH 3 // one-shot re-read shortly after a media button, so the new track shows fast
#define TIMER_LYRICS 4 // 100 ms lyric clock, only while lyrics are on; repaints only when the shown line changes
#define TIMER_HOVER 5  // one-shot: expand once the pointer has rested on the pill
#define TIMER_LEAVE 6  // one-shot: collapse once the pointer has stayed away
#define TIMER_TUCK 7   // 100 ms, only while tucked: untuck when the pointer leaves the zone around the notch
#define TIMER_SYNC 8      // every 5 min: pulls and pushes each connected calendar
#define TIMER_SYNC_SOON 9 // one-shot, 2 s after a change to a remote event: uploads it without waiting for the timer
#define WM_NEAR_CLICK (WM_USER + 102) // posted by the mouse hook with the screen point of every left click
#define WM_TRAY (WM_USER + 111)       // the tray icon's callback (NOTIFYICON_VERSION_4: LOWORD(lParam) is the event)
#define WM_WEATHER (WM_USER + 104)    // the forecast thread's answer: lParam a WeatherReply* that the UI thread owns (WeatherWork)
#define MENU_EXPAND 1001
#define MENU_MOVE 1002
#define MENU_QUIT 1003
#define MENU_CONFIG 1004
#define MENU_HIDE 1005

static UINT g_taskbarCreated = 0; // sent when Explorer restarts: the tray icon is added again

constexpr int TUCK_EDGE = 6;        // px of the pill left visible when tucked (nabyscape's edge height)
constexpr int NEAR_ZONE = 60;       // px around the pill where a click tucks it

// Pixel buffer sized for the largest notch (calendar tab, or the lyrics aside), in logical units
constexpr int BUF_W = (std::max)(LYRICS_PANEL_W, CAL_PANEL_W) + 2 * NOTCH_SHOULDER;
constexpr int BUF_H = CAL_POP_H;

// The forecast worker's answer, carried to the UI thread (WM_WEATHER) as a heap object
struct WeatherReply { bool located = false; bool net = false; std::wstring place; double lat = 0, lon = 0; bool ok = false; std::string body; };

// Windows Location API (needs location services on). Blocking: worker thread only. Waits up to about 3 s for a first fix.
static bool WindowsLocation(double& lat, double& lon) {
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool got = false;
    ILocation* loc = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_Location, NULL, CLSCTX_INPROC_SERVER, IID_ILocation, (void**)&loc))) {
        LOCATION_REPORT_STATUS status = REPORT_NOT_SUPPORTED;
        for (int i = 0; i < 12; ++i) {
            loc->GetReportStatus(IID_ILatLongReport, &status);
            if (status == REPORT_RUNNING || status == REPORT_ACCESS_DENIED || status == REPORT_NOT_SUPPORTED) break;
            Sleep(250);
        }
        ILocationReport* rep = NULL;
        ILatLongReport* ll = NULL;
        if (status == REPORT_RUNNING && SUCCEEDED(loc->GetReport(IID_ILatLongReport, &rep)) &&
            SUCCEEDED(rep->QueryInterface(IID_ILatLongReport, (void**)&ll))) {
            got = SUCCEEDED(ll->GetLatitude(&lat)) && SUCCEEDED(ll->GetLongitude(&lon));
            ll->Release();
        }
        if (rep) rep->Release();
        loc->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return got;
}

// The forecast worker: where the user is, when that is not known yet, then the forecast for it. Blocking, so it runs on its
// own thread with copies only; the answer goes back as one WeatherReply.
static void WeatherWork(HWND hwnd, WeatherReply r, bool autoOn, std::wstring city) {
    std::string resp;
    DWORD code = 0;
    Json j;
    if (!r.located && !city.empty()) { // a city typed by the user: when it is not found there is no automatic fallback
        if (HttpsRequest(L"GET", L"geocoding-api.open-meteo.com", L"/v1/search?name=" + UrlEncode(city) + L"&count=1&language=es", L"", "", resp, code) &&
            (r.net = true) && code == 200 && JsonParse(resp, j) && j["results"][0]["latitude"].t == Json::Num) {
            r.located = true;
            r.lat = j["results"][0]["latitude"].n;
            r.lon = j["results"][0]["longitude"].n;
            r.place = j["results"][0]["name"].str();
        }
    } else if (!r.located && autoOn) {
        if (WindowsLocation(r.lat, r.lon)) {
            r.located = true;
            r.place = L"Tu ubicación";
        } else if (HttpsRequest(L"GET", L"ipwho.is", L"/", L"", "", resp, code) && (r.net = true) && code == 200 && JsonParse(resp, j) &&
                   j["success"].t == Json::Bool && j["success"].b && j["latitude"].t == Json::Num && j["longitude"].t == Json::Num) {
            r.located = true;
            r.lat = j["latitude"].n;
            r.lon = j["longitude"].n;
            r.place = j["city"].str().empty() ? std::wstring(L"Tu ubicación") : j["city"].str();
        }
    } else if (!r.located) r.net = true; // nothing to try: the place is unknown by choice, not for lack of network
    if (r.located) {
        wchar_t path[320];
        swprintf(path, 320, L"/v1/forecast?latitude=%.2f&longitude=%.2f&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m&daily=temperature_2m_max,temperature_2m_min&forecast_days=1&timezone=auto",
                 r.lat, r.lon);
        r.ok = HttpsRequest(L"GET", L"api.open-meteo.com", path, L"", "", r.body, code) && code == 200;
    }
    WeatherReply* out = new WeatherReply(std::move(r));
    if (!PostMessageW(hwnd, WM_WEATHER, 0, (LPARAM)out)) delete out; // the window is gone
}

class CapriNotchApp {
public:
    HWND hWnd = NULL;
    CapriUI ui;
    PomodoroManager pomo;
    TodoManager todo;
    ShelfManager shelf;
    EventManager events;
    MediaManager media;
    AICompanion ai;
    WhistleEngine whistle;

    bool expanded = false;
    bool isTrackingMouse = false;
    bool tucked = false; // slid up out of the way after a click beside it
    bool hiddenByUser = false, hiddenByFullscreen = false; // the tray's "Ocultar notch", and a full-screen app
    HICON trayIcon = NULL;
    int tuckY = 0;       // current slide-up offset, logical px
    std::wstring lyricSig; // last painted lyric state
    NotchTab activeTab = TAB_MEDIA;
    ULONGLONG livePaintAt = 0; // last repaint for the collapsed pill's animation
    int lastAc = -1;           // charger state last seen (0/1), -1 until known
    int reminderMinute = -1;   // minute of day the event reminders last ran for
    int sensorTick = 0;        // seconds since the Sistema tab was opened: perfSensorSec picks which ones read the sensors

    // Window dimensions & animation (widths include both notch shoulders)
    int currentWidth = 220 + 2 * NOTCH_SHOULDER;
    int targetWidth = 220 + 2 * NOTCH_SHOULDER;
    int currentHeight = 34;
    int targetHeight = 34;
    float animW = (float)currentWidth, animH = (float)currentHeight, velW = 0, velH = 0; // spring state of the size
    ULONGLONG animAt = 0;                                                               // time of the last spring step

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
        g_onBattery = lastAc == 0;

        SetTimer(hWnd, TIMER_SECOND, 1000, NULL);  // 1s logic tick
        LoadSettings(SettingsIniPath(), g_settings);
        g_settings.startWithWindows = RunAtLogin(); // the Run key is the truth, not the ini
        if (g_settings.startWithWindows) SetRunAtLogin(true); // the exe may have moved: point the entry at this copy
        std::string key;
        if (UnprotectFromFile(AppDataFile(L"capri_ai_key.bin"), key)) ai.SetKey(Wide(key));
        ui.calView = (CapriUI::CalView)g_settings.calDefaultView;
        ApplySettings();
        InitSync();
        AddTray();
        UpdateDisplay();
    }

    // ---- settings (settings.h): applied at start and after every change from the settings window
    void ApplySettings() {
        ai.Configure(AiPreset(g_settings));
        LoadProviders();
        SetTimer(hWnd, TIMER_SYNC, (UINT)g_settings.syncMinutes * 60000, NULL);
        pomo.Retime();
        if (RunAtLogin() != g_settings.startWithWindows) SetRunAtLogin(g_settings.startWithWindows);
        if (!g_settings.hideFullscreen) hiddenByFullscreen = false;
        // A new place in Settings: the place is found again; the forecast follows on the next tick while its tab is open
        if (g_settings.weatherAuto != ui.weather.autoAsked || g_settings.weatherCityName != ui.weather.cityAsked) {
            ui.weather.located = false;
            ui.weather.at = 0;
        }
        ApplyFrameRate();
        ApplyVisibility();
        UpdateTargetDimensions();
        ui.RefreshGlow();
        UpdateDisplay();
    }

    void ApplyVisibility() { ShowWindow(hWnd, hiddenByUser || hiddenByFullscreen ? SW_HIDE : SW_SHOWNOACTIVATE); }

    // The animation timer's rate: 60 fps, or 30 when the performance settings say so (see EffectivePerf)
    void ApplyFrameRate() { SetTimer(hWnd, TIMER_ANIMATION, 1000 / EffectivePerf().fps, NULL); }

    // Full-screen games, videos and presentations: the notch steps aside, and comes back when they end
    void CheckFullscreen() {
        QUERY_USER_NOTIFICATION_STATE qs = QUNS_ACCEPTS_NOTIFICATIONS;
        bool full = g_settings.hideFullscreen && SUCCEEDED(SHQueryUserNotificationState(&qs)) &&
                    (qs == QUNS_BUSY || qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE);
        if (full == hiddenByFullscreen) return;
        hiddenByFullscreen = full;
        if (full) SetExpanded(false);
        ApplyVisibility();
    }

    // The key goes to DPAPI, never to the ini; an empty key deletes the file
    void SetAiKey(const std::wstring& key) {
        std::wstring path = AppDataFile(L"capri_ai_key.bin");
        if (key.empty()) DeleteFileW(path.c_str());
        else ProtectToFile(path, Utf8(key));
        ai.SetKey(key);
    }

    void OpenSettings(int section) {
        SettingsHooks h;
        h.account = [this](int src, int op) {
            SyncAccount& a = Account(src);
            if (op == 0) ConnectSync(a);
            else if (op == 1) DisconnectSync(a);
            else SyncOne(a);
            UpdateDisplay();
        };
        h.command = [this](int cmd) {
            if (cmd == 1) SwitchMonitor();
            else DestroyWindow(hWnd);
        };
        h.hasKey = [this] { return ai.HasKey(); };
        h.setKey = [this](const std::wstring& key) { SetAiKey(key); };
        SettingsWindow::Open(hWnd, section, &google, &outlook, &ics, h);
    }

    // ---- tray icon: a white pill with two eyes, drawn at the shell's small-icon size (the project has no .ico)
    HICON MakeTrayIcon() {
        using namespace Gdiplus;
        int px = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(hWnd));
        Bitmap bmp(px, px, PixelFormat32bppARGB);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        SolidBrush white(Color(255, 242, 242, 245)), ink(Color(255, 8, 9, 12));
        float h = px * 0.5f, y = px * 0.25f;
        g.FillEllipse(&white, 0.0f, y, h, h);
        g.FillEllipse(&white, px - h, y, h, h);
        g.FillRectangle(&white, h / 2.0f, y, px - h, h);
        g.FillEllipse(&ink, px * 0.33f, px * 0.38f, px * 0.12f, px * 0.24f);
        g.FillEllipse(&ink, px * 0.55f, px * 0.38f, px * 0.12f, px * 0.24f);
        HICON icon = NULL;
        bmp.GetHICON(&icon);
        return icon;
    }

    // Re-run on TaskbarCreated (Explorer restarted). Returns what the shell said.
    BOOL AddTray() {
        if (!trayIcon) trayIcon = MakeTrayIcon();
        NOTIFYICONDATAW nid = { sizeof(nid) };
        nid.hWnd = hWnd;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = trayIcon;
        lstrcpynW(nid.szTip, L"Capri Notch", 128);
        BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4; // clicks arrive as LOWORD(lParam) events, not raw mouse messages
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        return ok;
    }

    void RemoveTray() {
        NOTIFYICONDATAW nid = { sizeof(nid) };
        nid.hWnd = hWnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (trayIcon) DestroyIcon(trayIcon);
        trayIcon = NULL;
    }

    // Right-click on the notch (fromTray false) or on its tray icon: the same menu, the tray adds hide/show
    void ShowMenu(bool fromTray) {
        POINT pt;
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, MENU_CONFIG, L"Configuración");
        if (fromTray) AppendMenuW(menu, MF_STRING, MENU_HIDE, hiddenByUser ? L"Mostrar notch" : L"Ocultar notch");
        else AppendMenuW(menu, MF_STRING, MENU_EXPAND, expanded ? L"Colapsar Notch" : L"Expandir Notch");
        AppendMenuW(menu, MF_STRING, MENU_MOVE, L"Mover al otro monitor");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, MENU_QUIT, L"Salir de Capri Notch");

        SetForegroundWindow(hWnd); // without this the menu stays open when the user clicks elsewhere
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
        DestroyMenu(menu);
        PostMessageW(hWnd, WM_NULL, 0, 0); // and this one closes it properly after a click

        if (cmd == MENU_CONFIG) OpenSettings(SettingsWindow::SEC_GENERAL);
        else if (cmd == MENU_EXPAND) SetExpanded(!expanded);
        else if (cmd == MENU_HIDE) { hiddenByUser = !hiddenByUser; ApplyVisibility(); }
        else if (cmd == MENU_MOVE) SwitchMonitor();
        else if (cmd == MENU_QUIT) DestroyWindow(hWnd);
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
            ui.ShelfThumbsFree(); // thumbnails live only while the shelf is on screen
            whistle.Stop(); // the microphone is never left on behind a closed notch
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

        auto dotOf = [](const SyncAccount& a) {
            return !a.connected ? CapriUI::SYNC_OFF : a.syncing ? CapriUI::SYNC_BUSY : !a.error.empty() ? CapriUI::SYNC_ERR : CapriUI::SYNC_ON;
        };
        ui.syncDot[1] = dotOf(google);
        ui.syncDot[2] = dotOf(outlook);
        ui.linkGoogle = ics.account.find(L"google") != std::wstring::npos; // the link's host decides its agenda mark, G or O

        // Render UI with correct scanline stride
        ui.Render(pvBits, stride, pw, ph, scale, expanded, activeTab, pomo, todo, events, media, ai, whistle, shelf);
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
        // Damped spring towards the target size. The overshoot is clamped to the pixel buffer.
        ULONGLONG now = GetTickCount64();
        float dt = animAt && now - animAt <= 50 ? (now - animAt) / 1000.0f : 0.016f;
        animAt = now;
        bool anim = EffectivePerf().anim;
        if (anim) {
            capri_spring(&animW, &velW, (float)targetWidth, dt);
            capri_spring(&animH, &velH, (float)targetHeight, dt);
        } else { // animations off: the size jumps to its target
            animW = (float)targetWidth;
            animH = (float)targetHeight;
            velW = velH = 0;
        }
        int nw = (std::min)((int)lroundf(animW), BUF_W), nh = (std::min)((int)lroundf(animH), BUF_H);
        bool changed = nw != currentWidth || nh != currentHeight || velW != 0 || velH != 0 || animW != targetWidth || animH != targetHeight;
        currentWidth = nw;
        currentHeight = nh;
        int tuckTarget = tucked ? currentHeight - TUCK_EDGE : 0;
        if (tuckY != tuckTarget) {
            int diff = tuckTarget - tuckY;
            int step = anim ? diff / 4 : diff; // animations off: the tuck jumps in one step
            if (step == 0) step = (diff > 0) ? 1 : -1;
            tuckY += step;
            changed = true;
        }

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
        if (g_onBattery != (lastAc == 0)) { // the battery saver follows the charger
            g_onBattery = lastAc == 0;
            ApplyFrameRate();
            UpdateDisplay();
        }
        if (prev < 0 || prev == lastAc || !g_settings.chargerNotice) return;
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
            int lead = 0;
            int due = AlertDue(e, nowMin, &lead);
            if (!due || ui.calHidden[e.color] || !CapriUI::RemindsFor(e)) continue;
            const CalCategory& c = CAL_CATS[e.color];
            ui.notice = (due == 2 ? L"Ahora · " : lead == 60 ? L"En 1 h · " : L"En " + std::to_wstring(lead) + L" min · ") + e.title;
            ui.noticeRight = EventTime(e.minute);
            ui.noticeIcon = L"";
            ui.noticeColor = Gdiplus::Color(255, c.r, c.g, c.b);
            ui.noticeUntil = GetTickCount64() + 8000;
            if (g_settings.reminderSound) MessageBeep(MB_ICONASTERISK);
            break; // shortcut: one reminder per minute; the later of two simultaneous events is skipped, upgrade to queueing them
        }
    }

    void OnSecondTick() {
        if (whistle.process && !whistle.Running()) UpdateDisplay(); // the worker ended by itself: show why
        CheckReminders();
        CheckFullscreen();
        pomo.Tick();
        media.DetectActiveTrack();
        if (activeTab == TAB_SYSTEM && sensorTick++ % g_settings.perfSensorSec == 0) { // sampled only while shown (open, or collapsed on this tab): the CPU figure is a delta since the last call
            capri_sys_sample(&ui.sys, g_settings.sysTemps, g_settings.sysPower);
            ui.cpuHist[ui.cpuHistPos] = ui.sys.cpu;
            ui.cpuHistPos = (ui.cpuHistPos + 1) % 60;
        }
        if (expanded && activeTab == TAB_WEATHER) FetchWeather(); // a no-op within 15 min of the last answer
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
        if (tucked || media.showLyrics || !g_settings.tuckOnClick) return;
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

    // Scrolls the agenda just enough to bring row `task` of its task list (the dated tasks, then todo.tasks) into view
    void ShowTask(size_t task) {
        const size_t row = ui.calTaskRow + task, shown = 10; // rows of 22 px that fit between the header and the task field
        if (row < ui.calAgendaTop) ui.calAgendaTop = row;
        else if (row >= ui.calAgendaTop + shown) ui.calAgendaTop = row - shown + 1;
        UpdateDisplay();
    }

    // A click on a row of the agenda's task list (rows as the paint lists them): the circle flips done and open,
    // + starts a subtask of a top-level task, X deletes
    void OnTaskHit(int kind, size_t idx) {
        using C = CapriUI;
        const std::vector<size_t> dated = C::DatedTasks(events, C::TodayKey());
        const size_t D = dated.size();
        if (idx >= D + todo.tasks.size()) return;
        if (idx < D) { // a dated task lives in the events
            const size_t ei = dated[idx];
            ui.calEvent = -1; // the events move
            ChangeEvents([&] {
                if (kind == C::HIT_T_REMOVE) {
                    events.Remove(ei);
                } else {
                    CalEvent t = events.events[ei];
                    t.task = t.task == 2 ? 1 : 2;
                    events.Update(ei, t);
                }
            });
            return;
        }
        idx -= D;
        if (kind == C::HIT_T_REMOVE) {
            todo.Remove(idx);
            ui.todoParent = SIZE_MAX; // indices shifted
        } else if (kind == C::HIT_T_SUB) {
            ui.todoParent = idx;
            StartTyping();
        } else {
            todo.Toggle(idx);
        }
    }

    void CommitTodo() {
        std::wstring t = ui.todoDraft;
        t.erase(0, t.find_first_not_of(L' '));
        t.erase(t.find_last_not_of(L' ') + 1);
        ui.todoDraft.clear();
        // A date or a time typed makes a calendar task (not for a subtask, and not while tasksInCalendar is off)
        CalEvent p;
        const int today = CapriUI::TodayKey();
        if (g_settings.tasksInCalendar && ui.todoParent == SIZE_MAX && ParseQuickEvent(t, 0, today, p) && (p.date || p.minute >= 0)) {
            if (!p.date) p.date = today; // a time alone means today
            if (p.minute >= 0 && p.end < 0) p.end = (std::min)(1440, p.minute + 60);
            if (t.find(L'#') == std::wstring::npos) p.color = g_settings.defaultCategory;
            p.alert = g_settings.defaultAlert;
            p.moreAlerts = g_settings.defaultMoreAlerts;
            p.task = 1;
            size_t at = SIZE_MAX;
            ChangeEvents([&] { at = events.Add(p); });
            std::vector<size_t> dated = CapriUI::DatedTasks(events, today); // its row, after the others are found again
            return ShowTask(std::find(dated.begin(), dated.end(), at) - dated.begin());
        }
        size_t at = todo.Add(t, ui.todoParent); // ignores empty text; the parent stays set for the next subtask
        if (at == SIZE_MAX) return UpdateDisplay();
        ShowTask(CapriUI::DatedTasks(events, today).size() + at); // plain tasks come after the dated ones
    }

    // Saves the event window's draft (or rewrites the event it was opened on) and jumps to its day; a blank title does nothing
    void SavePopup() {
        CalEvent e = ui.popEv;
        ApplyTitleTokens(e, CapriUI::TodayKey()); // also trims the title
        if (e.title.empty()) return;
        ui.calEvent = (int)(ui.popEdit >= 0 ? events.Update(ui.popEdit, e) : events.Add(e));
        ScheduleSync(e.src);
        ui.CalSelect(e.date);
        ClosePopup();
    }

    void ClosePopup() {
        ui.calPop = false;
        ui.editing = false;
        UpdateTargetDimensions();
    }

    // Typing into the calendar: the focused field of the event window, else the agenda's task field
    void OnChar(wchar_t ch) {
        bool cal = ui.calPop;
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
        if (editIdx < 0) { e.color = g_settings.defaultCategory; e.alert = g_settings.defaultAlert; e.moreAlerts = g_settings.defaultMoreAlerts; } // new events take the defaults
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
        case C::HIT_POP_DELETE: {
            int src = events.events[ui.popEdit].src;
            events.Remove(ui.popEdit);
            ui.calEvent = -1;
            ScheduleSync(src);
            return ClosePopup();
        }
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
        case C::HIT_POP_ALERT: { // h.a is one bit; Ninguno (0) clears them all
            int mask = AlertMask(e.alert, e.moreAlerts);
            SetAlertMask(h.a ? mask ^ h.a : 0, e.alert, e.moreAlerts);
            return;
        }
        case C::HIT_POP_SOURCE: e.src = h.a; return;
        case C::HIT_POP_TASK: // a task is local: turning it on picks Capri
            e.task = e.task ? 0 : 1;
            if (e.task) e.src = 0;
            return;
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
        case C::HIT_T_TOGGLE: case C::HIT_T_REMOVE: case C::HIT_T_SUB: return OnTaskHit(h.kind, h.a);
        case C::HIT_T_ADD: // the + adds what was typed and keeps typing
            if (!ui.todoDraft.empty()) CommitTodo();
            [[fallthrough]];
        case C::HIT_T_FIELD: // a normal task, unless a subtask is already being typed
            if (!ui.editing) ui.todoParent = SIZE_MAX;
            return StartTyping();
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
            ScheduleSync(e.src);
            return;
        }
        if (h.kind == C::HIT_COLOR) e.color = h.a;
        else if (h.kind == C::HIT_MOVE) e.date = TmKey(KeyTm(e.date, h.a));
        else if (h.kind == C::HIT_REPEAT) e.repeat = (e.repeat + 1) % 5;
        else if (h.kind == C::HIT_TASK_DONE) e.task = e.task == 2 ? 1 : 2;
        ScheduleSync(e.src);
        ui.calEvent = (int)events.Update(ui.calEvent, e);
    }

    // A click on a weather or shelf control (see CapriUI::tabHits)
    void OnTabHit(const CapriUI::CalHit& h) {
        using C = CapriUI;
        switch (h.kind) {
        case C::HIT_W_RETRY: return FetchWeather(true);
        case C::HIT_W_SETUP: return OpenSettings(SettingsWindow::SEC_WEATHER);
        case C::HIT_S_MODE:
            ui.shelfSend = !ui.shelfSend;
            ui.shelfNote.clear();
            ui.ShelfThumbsFree();
            return;
        case C::HIT_S_REMOVE: return shelf.Remove(h.a);
        case C::HIT_S_ITEM:
            if ((size_t)h.a < shelf.files.size()) ShellExecuteW(NULL, L"open", shelf.files[h.a].c_str(), NULL, NULL, SW_SHOWNORMAL);
            return;
        }
    }

    // A file is being dragged over the notch, or was just dropped: open on Archivos so the drop has somewhere to land
    void OnDragNear() {
        whistle.Stop();
        KillTimer(hWnd, TIMER_LEAVE);
        SetTucked(false);
        ui.TabChanged(activeTab, TAB_SHELF);
        activeTab = TAB_SHELF;
        ui.editing = false;
        SetExpanded(true);
        UpdateTargetDimensions();
        UpdateDisplay();
    }

    // Files dropped on the notch: copied to the shelf, or handed to Quick Share in send mode. The caller owns `drop`.
    // shortcut: only real files (CF_HDROP: Explorer, the desktop); an image dragged straight out of a browser is refused. Upgrade to CFSTR_FILECONTENTS for those.
    void OnDrop(HDROP drop) {
        std::vector<std::wstring> paths;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < n; ++i) {
            std::wstring p(DragQueryFileW(drop, i, NULL, 0), L'\0');
            DragQueryFileW(drop, i, p.data(), (UINT)p.size() + 1);
            paths.push_back(p);
        }
        if (paths.empty()) return;
        if (ui.shelfSend) {
            ui.shelfNote = L"Abriendo Quick Share…";
            ShelfSend(hWnd, paths);
        } else {
            ui.shelfNote = L"Copiando…";
            ShelfCopyAsync(hWnd, paths, shelf.dir); // WM_SHELF_COPIED adds the copies
        }
        OnDragNear();
    }

    // Open-Meteo, asked for only while the Clima tab is open: a forecast at most every 15 min, or at once when forced (retry).
    // The place is looked up only while it is unknown (see ApplySettings). The answer comes back as WM_WEATHER.
    void FetchWeather(bool force = false) {
        auto& w = ui.weather;
        ULONGLONG now = GetTickCount64();
        if (w.status == 1 || (!force && w.status != 0 && w.at && now - w.at < 15 * 60000)) return;
        w.status = 1;
        w.autoAsked = g_settings.weatherAuto;
        w.cityAsked = g_settings.weatherCityName;
        WeatherReply known;
        known.located = w.located;
        known.place = w.place;
        known.lat = w.lat;
        known.lon = w.lon;
        std::thread(WeatherWork, hWnd, known, w.autoAsked, w.cityAsked).detach();
    }

    // ---- calendar sync: the two services, their sign-in and timers, and the worker's results (sync.h)
    SyncAccount google, outlook, ics; // Google (src 1), Outlook (src 2) and the calendar link (src 3, read-only)

    SyncAccount& Account(int src) { return src == 1 ? google : src == 2 ? outlook : ics; }

    void LoadProviders() { outlook.prov = OutlookProvider(LoadSyncConfig()); }

    void InitSync() {
        google.src = 1;
        google.tokenFile = AppDataFile(L"capri_sync_google.bin");
        outlook.src = 2;
        outlook.tokenFile = AppDataFile(L"capri_sync_outlook.bin");
        ics.src = 3;
        ics.tokenFile = AppDataFile(L"capri_sync_ics.bin");
        // a file the bridge cannot read (the old OAuth format included) is removed: Google connects again with the script
        if (!LoadBridgeFile(google.tokenFile, google.bridgeUrl, google.bridgeToken, google.account)) DeleteFileW(google.tokenFile.c_str());
        google.connected = !google.bridgeUrl.empty();
        outlook.connected = LoadAccountToken(outlook.tokenFile, outlook.refresh, outlook.account);
        ics.connected = LoadIcsFile(ics.tokenFile, ics.link);
        ics.account = IcsHost(ics.link);
        SyncAll();
    }

    void SyncAll() {
        SyncOne(google);
        SyncOne(outlook);
        SyncOne(ics);
    }

    // Starts a sync of a connected account; asked for while one runs, it runs once more when that one ends
    void SyncOne(SyncAccount& a) {
        if (!a.connected) return;
        if (a.syncing) { a.again = true; return; }
        std::vector<CalEvent> pending, series;
        for (const CalEvent& e : events.events) {
            if (e.src != a.src) continue;
            if (e.dirty) pending.push_back(e);
            if (e.repeat) series.push_back(e);
        }
        int today = CapriUI::TodayKey();
        StartSync(hWnd, a, false, pending, series, TmKey(KeyTm(today, -30)), TmKey(KeyTm(today, 180)));
        UpdateDisplay();
    }

    // A change to a remote event goes up 2 s later (each change restarts the wait)
    void ScheduleSync(int src) {
        if (src == 1 || src == 2) SetTimer(hWnd, TIMER_SYNC_SOON, 2000, NULL); // a link event is never uploaded
    }

    // The pill's banner for sync news; the caller repaints
    void Notify(const wchar_t* icon, const std::wstring& text, Gdiplus::Color color, ULONGLONG ms) {
        ui.notice = text;
        ui.noticeRight.clear();
        ui.noticeIcon = icon;
        ui.noticeColor = color;
        ui.noticeUntil = GetTickCount64() + ms;
    }

    // A change to the event list that the user did not click (a sync): the selected and edited events are found again afterwards
    template <class F> void ChangeEvents(F change) {
        CalEvent sel{}, edit{};
        bool hasSel = ui.calEvent >= 0 && ui.calEvent < (int)events.events.size();
        bool hasEdit = ui.popEdit >= 0 && ui.popEdit < (int)events.events.size();
        if (hasSel) sel = events.events[ui.calEvent];
        if (hasEdit) edit = events.events[ui.popEdit];
        change();
        events.Save();
        // The same event: by local uid, or by remote id (a merged copy gets a new uid)
        auto find = [&](bool has, const CalEvent& want) {
            for (int i = 0; has && i < (int)events.events.size(); ++i) {
                const CalEvent& e = events.events[i];
                if (e.uid == want.uid || (!want.rid.empty() && e.src == want.src && e.rid == want.rid)) return i;
            }
            return -1;
        };
        ui.calEvent = find(hasSel, sel);
        ui.popEdit = find(hasEdit, edit); // gone: the draft saves as a new event rather than being lost
    }

    // The worker's answer: applies the pushes and the pulled window, then the account's state and the banner
    void OnSyncDone(int src, SyncResult* raw) {
        std::unique_ptr<SyncResult> r(raw);
        SyncAccount& a = Account(src);
        a.syncing = false;
        const std::wstring name = src == 1 ? L"Google" : src == 2 ? L"Outlook" : L"Enlace";
        const Gdiplus::Color red(255, 239, 68, 68);
        if (r->gen == a.gen) { // an answer from a connection that was removed meanwhile is dropped
            std::wstring prevError = a.error;
            std::string prevRefresh = a.refresh;
            a.error = r->error;
            if (r->loggedIn) a.connected = true;
            if (r->revoked) {
                a.connected = false;
                DeleteFileW(a.tokenFile.c_str());
            }
            if (!r->refresh.empty()) a.refresh = r->refresh;
            if (!r->account.empty()) a.account = r->account;
            a.access = r->access;
            a.expiresAt = r->expiresAt;
            if (src == 1) {
                if (a.connected && r->loggedIn) SaveBridgeAccount(a);
            } else if (src == 2) {
                if (a.connected && (r->loggedIn || r->refresh != prevRefresh)) SaveAccountToken(a.tokenFile, a.refresh, a.account);
            } else if (a.connected && r->loggedIn) {
                SaveIcsFile(a.tokenFile, a.link);
            }
            if (a.connected && r->error.empty()) a.lastSync = GetTickCount64();
            if (!r->pushes.empty() || r->pulled) {
                ChangeEvents([&] {
                    ApplyPush(events.events, r->pushes);
                    if (r->pulled) MergePull(events.events, src, r->fromKey, r->toKey, r->fetched);
                });
            }
            if (r->loggedIn) Notify(L"", name + L" conectado", Gdiplus::Color(255, 74, 222, 128), 3000);
            else if (r->revoked) Notify(L"", L"Vuelve a conectar " + name, red, 6000);
            else if (r->login) Notify(L"", L"No se pudo conectar " + name, red, 6000);
            else if (!r->error.empty() && r->error != prevError) Notify(L"", L"Error al sincronizar " + name, red, 6000);
        }
        if (a.again) {
            a.again = false;
            SyncOne(a);
        }
        UpdateDisplay();
    }

    // Connect. Google: its URL and key are checked by the bridge's first call (worker). Outlook: needs the ID in capri_sync.ini;
    // without it the commented template is created and opened to fill in.
    void ConnectSync(SyncAccount& a) {
        if (a.connected || a.syncing) return;
        if (a.src == 1 && !IsBridgeUrl(a.bridgeUrl)) { Notify(L"", L"La URL debe terminar en /exec (la de tu aplicación web)", Gdiplus::Color(255, 251, 191, 36), 6000); return UpdateDisplay(); }
        if (a.src == 1 && a.bridgeToken.empty()) { Notify(L"", L"Copia primero el script", Gdiplus::Color(255, 251, 191, 36), 6000); return UpdateDisplay(); }
        if (a.src == 3 && !NormalizeIcsLink(a.link)) { Notify(L"", L"El enlace debe empezar por https:// o webcal://", Gdiplus::Color(255, 251, 191, 36), 6000); return UpdateDisplay(); }
        LoadProviders();
        if (a.src == 2 && a.prov.clientId.empty()) {
            CreateSyncIni(SyncIniPath());
            ShellExecuteW(hWnd, L"open", L"notepad.exe", SyncIniPath().c_str(), NULL, SW_SHOWNORMAL);
            Notify(L"", L"Configura capri_sync.ini", Gdiplus::Color(255, 251, 191, 36), 6000);
            return UpdateDisplay();
        }
        a.error.clear();
        int today = CapriUI::TodayKey();
        StartSync(hWnd, a, true, {}, {}, TmKey(KeyTm(today, -30)), TmKey(KeyTm(today, 180)));
        UpdateDisplay();
    }

    // Disconnect: local changes not uploaded yet are asked about first; the service's events leave the calendar.
    // The events of a removed service are only dropped here, after the question.
    void DisconnectSync(SyncAccount& a) {
        int pending = 0;
        for (const CalEvent& e : events.events) pending += e.src == a.src && e.dirty != 0;
        if (pending && MessageBoxW(hWnd, (L"Hay " + std::to_wstring(pending) + L" cambios sin subir. ¿Desconectar igualmente?").c_str(),
                                   L"Capri Notch", MB_YESNO | MB_ICONWARNING) != IDYES) return;
        DeleteFileW(a.tokenFile.c_str());
        ChangeEvents([&] {
            auto& v = events.events;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const CalEvent& e) { return e.src == a.src; }), v.end());
        });
        a.connected = false;
        a.again = false;
        a.gen++; // a sync still running for this connection is dropped when it ends
        a.refresh.clear();
        a.access.clear();
        a.bridgeUrl.clear();
        a.bridgeToken.clear();
        a.link.clear();
        a.account.clear();
        a.error.clear();
        a.lastSync = 0;
        UpdateDisplay();
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

        // Settings gear at the right end of the tab row (drawn on every tab; not while the event window covers the calendar)
        if (!ui.calPop && y >= 8 && y <= 38 && x >= w - 44 && x <= w - 12) return OpenSettings(SettingsWindow::SectionOfTab(activeTab));

        // Icon tab row (y: 8 - 38)
        if (y >= 8 && y <= 38) {
            for (int i = 0; i < TAB_COUNT; ++i) {
                float tabX = 16.0f + i * TAB_STEP;
                if (x >= tabX && x <= tabX + TAB_W) {
                    ui.TabChanged(activeTab, (NotchTab)i);
                    activeTab = (NotchTab)i;
                    ui.editing = false;
                    if (i == TAB_AI && whistle.model == CACTUS_WHISTLE) whistle.Start(hWnd); // choosing the tab starts the dictation; the screen is only shared by a click
                    else whistle.Stop();
                    if (i == TAB_CALENDAR) ui.CalHome();
                    if (i == TAB_WEATHER) FetchWeather();
                    sensorTick = 0; // the first second on Sistema reads the sensors again
                    if (i == TAB_SYSTEM) capri_sys_sample(&ui.sys, g_settings.sysTemps, g_settings.sysPower); // memory and disk on the first paint; CPU follows a second later
                    UpdateTargetDimensions();
                    UpdateDisplay();
                    return;
                }
            }
        }

        // Areas the clock and weather tabs registered while painting
        for (auto it = ui.tabHits.rbegin(); it != ui.tabHits.rend(); ++it)
            if (it->r.Contains((float)x, (float)y)) {
                OnTabHit(*it);
                UpdateDisplay();
                return;
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
        } else if (activeTab == TAB_CALENDAR) {
            // The top-most area the last paint registered under the pointer wins
            for (auto it = ui.calHits.rbegin(); it != ui.calHits.rend(); ++it)
                if (it->r.Contains((float)x, (float)y)) {
                    OnCalHit(*it);
                    break;
                }
            UpdateDisplay();
        } else if (activeTab == TAB_AI) {
            if (y >= 46 && y < 70 && x >= w - 156 && x <= w - 20) {
                // Model chips: Whistle (w-156..w-90) and Mirage (w-86..w-20); changing ends the one that was running
                int m = x < w - 88 ? CACTUS_WHISTLE : CACTUS_MIRAGE;
                if (m != whistle.model) {
                    whistle.Stop();
                    whistle.model = m;
                    whistle.text.clear();
                    whistle.error.clear();
                }
                UpdateDisplay();
            } else if (y >= 70 && y <= 98 && x >= 20 && x <= w - 20) {
                // The chosen model's bar: on and off; without the download it opens the setting that gets it
                if (!CactusInstalled(whistle.model)) return OpenSettings(SettingsWindow::SEC_AI);
                if (whistle.Running()) whistle.Stop();
                else whistle.Start(hWnd);
                UpdateDisplay();
            } else if (y >= 192 && y <= 222) {
                // Quick prompts (y: 192..222)
                if (x >= 24 && x <= 194) {
                    whistle.text.clear(); // the answer takes the card back from an old dictation
                    ai.Ask(L"Resume brevemente el enfoque para hoy");
                } else if (x >= 204 && x <= 374) {
                    whistle.text.clear();
                    ai.Ask(L"Dame un consejo rapido para concentrarme mejor");
                }
                UpdateDisplay();
            }
        }
    }
};

static CapriNotchApp g_App;

// OLE drop target of the notch: a file dragged over it opens it on Archivos before the drop (WM_DROPFILES has no "entered" event).
// One static instance, so the reference count is not kept.
struct NotchDropTarget : IDropTarget {
    bool files = false; // the drag in progress carries files
    static FORMATETC Hdrop() { return { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL }; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) { *out = this; return S_OK; }
        *out = NULL;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        FORMATETC fmt = Hdrop();
        files = data && data->QueryGetData(&fmt) == S_OK;
        if (files) g_App.OnDragNear();
        *effect = files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (files) SetTimer(g_App.hWnd, TIMER_LEAVE, g_settings.leaveDelay, NULL); // left without dropping: close like a pointer that left
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        FORMATETC fmt = Hdrop();
        STGMEDIUM med = {};
        *effect = DROPEFFECT_NONE;
        if (files && data && SUCCEEDED(data->GetData(&fmt, &med))) {
            g_App.OnDrop((HDROP)med.hGlobal);
            ReleaseStgMedium(&med);
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }
};
static NotchDropTarget g_dropTarget;

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
            if (!g_App.CursorOver() && !g_App.ui.editing && !g_App.whistle.Running()) g_App.SetExpanded(false); // typing or dictating keeps it open
        } else if (wParam == TIMER_TUCK) {
            g_App.OnTuckTick();
        } else if (wParam == TIMER_SYNC) {
            g_App.SyncAll();
        } else if (wParam == TIMER_SYNC_SOON) {
            KillTimer(hWnd, TIMER_SYNC_SOON);
            g_App.SyncAll();
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
            if (!g_App.expanded && !g_App.tucked && g_settings.hoverOpen) SetTimer(hWnd, TIMER_HOVER, g_settings.hoverDelay, NULL);
        }
        KillTimer(hWnd, TIMER_LEAVE); // back inside before the grace period ran out
        break;

    case WM_MOUSELEAVE:
        g_App.isTrackingMouse = false;
        KillTimer(hWnd, TIMER_HOVER);
        if (g_App.expanded) SetTimer(hWnd, TIMER_LEAVE, g_settings.leaveDelay, NULL);
        break;

    case WM_NEAR_CLICK: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) }; // signed: monitors left of/above the primary
        g_App.OnNearClick(pt);
        break;
    }

    case WM_SYNC_DONE:
        g_App.OnSyncDone((int)wParam, (SyncResult*)lParam);
        break;

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
        if (g_App.expanded && g_App.activeTab == TAB_CALENDAR && !g_App.ui.calPop) {
            g_App.ui.CalWheel(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
            g_App.UpdateDisplay();
        }
        if (g_App.expanded && g_App.activeTab == TAB_SHELF) {
            if (GET_WHEEL_DELTA_WPARAM(wParam) > 0) { if (g_App.ui.shelfTop) --g_App.ui.shelfTop; } else ++g_App.ui.shelfTop; // the paint clamps the end
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

    case WM_RBUTTONUP:
        g_App.ShowMenu(false);
        break;

    case WM_TRAY: // NOTIFYICON_VERSION_4: the event is in LOWORD(lParam)
        if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT) g_App.OpenSettings(SettingsWindow::SEC_GENERAL);
        else if (LOWORD(lParam) == WM_CONTEXTMENU) g_App.ShowMenu(true);
        break;

    case WM_WEATHER: {
        std::unique_ptr<WeatherReply> r((WeatherReply*)lParam);
        auto& w = g_App.ui.weather;
        if (g_settings.weatherAuto != w.autoAsked || g_settings.weatherCityName != w.cityAsked) {
            w.status = 0; // the place settings changed while this was in flight: the next tick asks again
        } else {
            if (r->located) {
                w.located = true;
                w.place = r->place;
                w.lat = r->lat;
                w.lon = r->lon;
            }
            w.status = !r->located ? (r->net ? 4 : 3) : (r->ok && capri_weather_parse(r->body.data(), r->body.size(), &w.data)) ? 2 : 3;
            w.at = GetTickCount64();
        }
        g_App.UpdateDisplay();
        break;
    }

    case WM_SHELF_COPIED: {
        std::unique_ptr<std::vector<std::wstring>> made((std::vector<std::wstring>*)lParam);
        for (auto it = made->rbegin(); it != made->rend(); ++it) g_App.shelf.Add(*it); // the first one dropped ends on top
        g_App.ui.shelfTop = 0;
        g_App.ui.shelfNote = wParam ? L"No se pudo copiar " + std::to_wstring(wParam) + L" (las carpetas no se guardan)" : L"";
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); // the copy is over: hand its pages back
        g_App.UpdateDisplay();
        break;
    }

    case WM_SHELF_SENT:
        g_App.ui.shelfNote = wParam ? L"Elige tu móvil en la ventana de Quick Share" : L"Quick Share no respondió. ¿Está instalado?";
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); // the send is over: hand its pages back
        g_App.UpdateDisplay();
        break;

    case WM_COPYDATA: { // the Whistle worker's answers (WhistleSend)
        const COPYDATASTRUCT* cd = (const COPYDATASTRUCT*)lParam;
        if (cd && g_App.whistle.process && cd->cbData < (1u << 18))
            g_App.whistle.OnMessage(cd->dwData, std::string((const char*)cd->lpData, cd->cbData));
        g_App.UpdateDisplay();
        return TRUE;
    }

    case WM_SETTINGS_CHANGED:
        g_App.ApplySettings();
        break;

    case WM_SETTINGCHANGE:              // a new wallpaper...
    case WM_DWMCOLORIZATIONCOLORCHANGED: // ...or accent: the glow that follows them is read again, once
        if (g_settings.glowSource != 1 && g_settings.glowSource != 2) return DefWindowProc(hWnd, msg, wParam, lParam);
        if (msg == WM_SETTINGCHANGE && wParam != SPI_SETDESKWALLPAPER) return DefWindowProc(hWnd, msg, wParam, lParam);
        g_App.ui.RefreshGlow(true);
        g_App.UpdateDisplay();
        return DefWindowProc(hWnd, msg, wParam, lParam);

    case WM_USER + 101: // Re-open or expand request from duplicate launch
        g_App.SetTucked(false);
        g_App.SetExpanded(true);
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        g_App.UpdateDisplay();
        break;

    case WM_DESTROY:
        RevokeDragDrop(hWnd);
        g_App.RemoveTray();
        PostQuitMessage(0);
        break;

    default:
        if (msg == g_taskbarCreated) { // Explorer restarted and dropped the tray icon
            g_App.AddTray();
            return 0;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Worker mode: this process is the Whistle or Mirage engine of a running notch (see whistle.h). It must not reach the single-instance check.
    if (const char* w = strstr(lpCmdLine, "--whistle ")) return WhistleWorker((HWND)(UINT_PTR)strtoull(w + 10, NULL, 10));
    if (const char* m = strstr(lpCmdLine, "--mirage ")) return MirageWorker((HWND)(UINT_PTR)strtoull(m + 9, NULL, 10));

    // Quick debug log
    FILE* dbg = fopen("C:\\capri_debug.txt", "w");
    if (dbg) { fprintf(dbg, "START\n"); fflush(dbg); }

    // OLE (an STA) for the notch's drop target; the WinRT media detection runs in it too
    HRESULT roHr = OleInitialize(NULL);
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

    // Dark context menus (tray and notch right-click): uxtheme ordinals 135 SetPreferredAppMode(1 = AllowDark) and 136 FlushMenuThemes, Win10 1903+
    if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        auto setMode = (int (WINAPI*)(int))GetProcAddress(ux, MAKEINTRESOURCEA(135));
        auto flush = (void (WINAPI*)())GetProcAddress(ux, MAKEINTRESOURCEA(136));
        if (setMode && flush) { setMode(1); flush(); }
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
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

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
    RegisterDragDrop(hWnd, &g_dropTarget);

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
    OleUninitialize();
    return (int)msg.wParam;
}
