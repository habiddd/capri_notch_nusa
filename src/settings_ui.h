#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include "settings.h"
#include "sync.h"
#include "events.h"

// The settings window: an ordinary window painted with GDI+ into an off-screen bitmap, in the notch's look. Every change is
// saved and posted to the notch (WM_SETTINGS_CHANGED), which applies it at once. Account and key actions go through hooks.
#define WM_SETTINGS_CHANGED (WM_USER + 110)

struct SettingsHooks {
    std::function<void(int src, int op)> account; // op 0 connect, 1 disconnect, 2 sync now
    std::function<void(int cmd)> command;         // 1 move to the other monitor, 2 quit
    std::function<bool()> hasKey;
    std::function<void(const std::wstring&)> setKey; // "" removes the key
};

class SettingsWindow {
public:
    enum Section { SEC_GENERAL, SEC_PILL, SEC_MUSIC, SEC_POMODORO, SEC_CALENDAR, SEC_ACCOUNTS, SEC_WEATHER, SEC_SYSTEM, SEC_PERF, SEC_AI, SEC_ABOUT, SEC_COUNT };

    // The tab gear opens the section of its tab
    static int SectionOfTab(int tab) { static const int s[] = { SEC_MUSIC, SEC_POMODORO, SEC_GENERAL, SEC_CALENDAR, SEC_AI, SEC_WEATHER, SEC_SYSTEM }; return s[tab]; }

    static void Open(HWND notch, int section, SyncAccount* google, SyncAccount* outlook, SyncAccount* ics, const SettingsHooks& h) {
        notchWnd = notch;
        acc[1] = google;
        acc[2] = outlook;
        acc[3] = ics;
        hooks = h;
        cfg = LoadSyncConfig();
        scriptCopied = false;
        focus = -1;
        edit.clear();
        scrollY = 0;
        SectionSet(section);
        if (wnd) {
            ShowWindow(wnd, SW_SHOW);
            SetForegroundWindow(wnd);
            InvalidateRect(wnd, NULL, FALSE);
            return;
        }
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(NULL);
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.lpszClassName = L"CapriSettingsWindow";
            RegisterClassExW(&wc);
            registered = true;
        }
        float s = Scale(notch);
        RECT rc = { 0, 0, (LONG)(W * s), (LONG)(H * s) };
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&rc, style, FALSE);
        MONITORINFO mi = { sizeof(MONITORINFO) };
        GetMonitorInfoW(MonitorFromWindow(notch, MONITOR_DEFAULTTONEAREST), &mi);
        int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2;
        int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;
        wnd = CreateWindowExW(WS_EX_LAYERED, L"CapriSettingsWindow", L"Configuración · Capri Notch", style, x, y, ww, wh, NULL, NULL,
                              GetModuleHandleW(NULL), NULL);
        if (!wnd) return;
        SetLayeredWindowAttributes(wnd, 0, EffectivePerf().anim ? 0 : 255, LWA_ALPHA); // the open fade starts invisible
        openAt = GetTickCount64();
        Animate();
        BOOL dark = TRUE;
        DwmSetWindowAttribute(wnd, 20, &dark, sizeof dark); // DWMWA_USE_IMMERSIVE_DARK_MODE: dark title bar
        COLORREF caption = RGB(14, 15, 18); // the window body colour; Win11 only, Win10 ignores it
        DwmSetWindowAttribute(wnd, 35, &caption, sizeof caption); // DWMWA_CAPTION_COLOR
        DWORD corners = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(wnd, 33, &corners, sizeof corners); // DWMWA_WINDOW_CORNER_PREFERENCE
        SetTimer(wnd, 1, 1000, NULL); // account states and the memory figure change without input
        ShowWindow(wnd, SW_SHOW);
        SetForegroundWindow(wnd);
    }

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            Paint(dc, Scale(h));
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (w == 2) Tick(h);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        case WM_MOUSEMOVE: Hover(h, (float)(short)LOWORD(l) / Scale(h), (float)(short)HIWORD(l) / Scale(h)); return 0;
        case WM_MOUSELEAVE: Hover(h, -1.0f, -1.0f); return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT && HitAt(hx, hy) >= 0) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
            break;
        case WM_LBUTTONDOWN: Click((float)(short)LOWORD(l) / Scale(h), (float)(short)HIWORD(l) / Scale(h)); return 0;
        case WM_MOUSEWHEEL: {
            float maxScroll = (std::max)(0.0f, contentH - (BOTTOM - TOP));
            scrollY = (std::max)(0.0f, (std::min)(maxScroll, scrollY - GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 40.0f));
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        case WM_CHAR: Key((wchar_t)w); return 0;
        case WM_DESTROY:
            KillTimer(h, 1);
            CommitFocus();
            wnd = NULL;
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

private:
    // Layout in logical px (the window is scaled by DPI). Rows scroll inside [TOP, BOTTOM).
    static constexpr float W = 780, H = 540, SIDE = 200, CX = 232, CR = 756, TOP = 84, BOTTOM = 524, ROW = 56;
    static constexpr float SIDE_TOP = 40.0f, SIDE_STEP = 44.0f; // sidebar rows: 11 of them, so they start above the content's TOP

    // Controls: what a row is, by id
    enum Id { ID_START, ID_HOVER_OPEN, ID_HOVER_MS, ID_LEAVE_MS, ID_TUCK, ID_FULLSCREEN, ID_EYES, ID_SLEEP, ID_EQUALIZER,
              ID_CHARGER, ID_LYRICS, ID_FOCUS, ID_BREAK, ID_POMO_SOUND, ID_CAL_VIEW, ID_ALERT, ID_CATEGORY, ID_REMIND_SOUND,
              ID_SYNC_MIN, ID_AI_PROVIDER, ID_AI_HTTPS, ID_WEATHER_AUTO, ID_SYS_TEMPS, ID_SYS_POWER, ID_TASKS_CAL,
              ID_PILL_ART, ID_PILL_ARTIST, ID_PILL_POMO_IDLE, ID_PILL_POMO_BAR, ID_PILL_POMO_SESSIONS, ID_PILL_SYS, ID_PILL_SYS_CPU,
              ID_PILL_SYS_GPU, ID_PILL_SYS_RAM, ID_PILL_SYS_DISK, ID_PILL_SYS_POWER, // the ID_PILL_ block follows PillFlags' order
              ID_PERF_ANIM, ID_PERF_GLOW, ID_PERF_SAVER, ID_PERF_FPS, ID_PERF_SENSOR };
    enum Field { F_HOST, F_PATH, F_MODEL, F_PORT, F_KEY, F_G_URL, F_MS_ID, F_ICS, F_WCITY };
    enum BtnId { B_MONITOR, B_CLEAR_KEY, B_DATA, B_QUIT, B_GUIDE, B_COPY_SCRIPT, B_ACC_CONNECT, B_ACC_DISCONNECT, B_ACC_SYNC };
    enum HitKind { HK_SECTION, HK_BOOL, HK_STEP, HK_CHIP, HK_FIELD, HK_BUTTON };

    struct Hit { Gdiplus::RectF r; int kind, a, b; };
    struct Num { int* v; int lo, hi, step; const wchar_t* unit; };
    struct Choice { std::vector<std::wstring> labels; std::vector<int> values; };

    static inline HWND wnd = NULL, notchWnd = NULL;
    static inline int section = SEC_GENERAL;
    static inline float scrollY = 0, contentH = 0;
    static inline int focus = -1; // field being typed in, -1 none
    static inline std::wstring edit;
    static inline std::vector<Hit> hits;
    static inline std::vector<int> fieldOrder;
    static inline SyncAccount* acc[4] = { nullptr, nullptr, nullptr, nullptr }; // 1 Google, 2 Outlook, 3 calendar link
    static inline SyncConfig cfg;
    static inline SettingsHooks hooks;
    static inline bool scriptCopied = false; // the button says "Copiado" until the window closes
    static inline float hx = -1, hy = -1;    // pointer in logical px, -1 outside the window
    static inline ULONGLONG openAt = 0, secAt = 0, toggleAt = 0; // animation starts (GetTickCount64), 0 = never
    static inline float chipFrom = 0;        // sidebar chip's y where its current slide started
    static inline int toggleId = -1;         // the toggle that animates

    static constexpr ULONGLONG OPEN_MS = 160, SECTION_MS = 160, TOGGLE_MS = 140;

    static float Scale(HWND h) { return GetDpiForWindow(h) / 96.0f; }

    static float ChipY() {
        float to = SIDE_TOP + section * SIDE_STEP;
        return chipFrom + (to - chipFrom) * Ease(secAt, SECTION_MS);
    }

    static void SectionSet(int s) {
        if (s != section) {
            chipFrom = ChipY();
            secAt = GetTickCount64();
            Animate();
        }
        section = s;
        scrollY = 0;
    }

    static void Invalidate() { if (wnd) InvalidateRect(wnd, NULL, FALSE); }

    static int HitAt(float x, float y) {
        for (size_t i = hits.size(); i-- > 0;) if (hits[i].r.Contains(x, y)) return (int)i;
        return -1;
    }

    // Repaints only when the pointer moves onto or off a control
    static void Hover(HWND h, float x, float y) {
        // Asked for only while the pointer is inside: from outside, Windows answers with WM_MOUSELEAVE at once, forever
        TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
        if (x >= 0) TrackMouseEvent(&tme);
        if (HitAt(x, y) != HitAt(hx, hy)) Invalidate();
        hx = x;
        hy = y;
    }

    // Ease-out from the start time at, over ms. at 0 = never started, so already done.
    static float Ease(ULONGLONG at, ULONGLONG ms) {
        if (!EffectivePerf().anim) return 1.0f; // animations off: everything is at its final state at once
        ULONGLONG el = at ? GetTickCount64() - at : ms;
        if (el >= ms) return 1.0f;
        float t = 1.0f - el / (float)ms;
        return 1.0f - t * t * t;
    }

    static Gdiplus::Color Mix(Gdiplus::Color a, Gdiplus::Color b, float t) {
        auto lerp = [t](BYTE p, BYTE q) { return (BYTE)(p + (q - p) * t + 0.5f); };
        return Gdiplus::Color(lerp(a.GetR(), b.GetR()), lerp(a.GetG(), b.GetG()), lerp(a.GetB(), b.GetB()));
    }

    static bool Animating() { return Ease(openAt, OPEN_MS) < 1 || Ease(secAt, SECTION_MS) < 1 || Ease(toggleAt, TOGGLE_MS) < 1; }

    // Starts the 60 fps timer (id 2); Tick stops it when nothing moves, so an idle window does not repaint
    static void Animate() {
        if (!wnd) return;
        SetTimer(wnd, 2, 16, NULL);
        Invalidate();
    }

    static void Tick(HWND h) {
        SetLayeredWindowAttributes(h, 0, (BYTE)(255 * Ease(openAt, OPEN_MS)), LWA_ALPHA); // ends at 255
        if (!Animating()) KillTimer(h, 2);
    }

    // Saves, and tells the notch to apply it
    static void Changed() {
        SaveSettings(SettingsIniPath(), g_settings);
        if (notchWnd) PostMessageW(notchWnd, WM_SETTINGS_CHANGED, 0, 0);
        Invalidate();
    }

    static bool* BoolOf(int id) {
        Settings& s = g_settings;
        if (id >= ID_PILL_ART && id <= ID_PILL_SYS_POWER) return PillFlags(s)[id - ID_PILL_ART].second;
        switch (id) {
        case ID_START: return &s.startWithWindows;
        case ID_HOVER_OPEN: return &s.hoverOpen;
        case ID_TUCK: return &s.tuckOnClick;
        case ID_FULLSCREEN: return &s.hideFullscreen;
        case ID_EYES: return &s.eyes;
        case ID_EQUALIZER: return &s.equalizer;
        case ID_CHARGER: return &s.chargerNotice;
        case ID_LYRICS: return &s.pillLyrics;
        case ID_POMO_SOUND: return &s.pomoSound;
        case ID_REMIND_SOUND: return &s.reminderSound;
        case ID_WEATHER_AUTO: return &s.weatherAuto;
        case ID_SYS_TEMPS: return &s.sysTemps;
        case ID_SYS_POWER: return &s.sysPower;
        case ID_TASKS_CAL: return &s.tasksInCalendar;
        case ID_PERF_ANIM: return &s.perfAnimations;
        case ID_PERF_GLOW: return &s.perfGlow;
        case ID_PERF_SAVER: return &s.perfBatterySaver;
        default: return &s.aiHttps; // ID_AI_HTTPS
        }
    }

    static Num NumOf(int id) {
        Settings& s = g_settings;
        switch (id) {
        case ID_HOVER_MS: return { &s.hoverDelay, 0, 1000, 50, L" ms" };
        case ID_LEAVE_MS: return { &s.leaveDelay, 100, 2000, 100, L" ms" };
        case ID_SLEEP: return { &s.sleepMin, 1, 10, 1, L" min" };
        case ID_FOCUS: return { &s.focusMin, 5, 90, 5, L" min" };
        default: return { &s.breakMin, 1, 30, 1, L" min" }; // ID_BREAK
        }
    }

    static int* ChoiceOf(int id) {
        Settings& s = g_settings;
        switch (id) {
        case ID_CAL_VIEW: return &s.calDefaultView;
        case ID_CATEGORY: return &s.defaultCategory;
        case ID_SYNC_MIN: return &s.syncMinutes;
        case ID_PERF_FPS: return &s.perfFps;
        case ID_PERF_SENSOR: return &s.perfSensorSec;
        default: return &s.aiProvider; // ID_AI_PROVIDER
        }
    }

    static Choice ChoicesOf(int id) {
        switch (id) {
        case ID_CAL_VIEW: return { { L"Agenda", L"Día", L"Semana", L"Mes" }, { 0, 1, 2, 3 } };
        case ID_ALERT: return { { L"Ninguno", L"Al empezar", L"10 min", L"30 min", L"1 h" }, { 0, 1, 2, 4, 8 } }; // alert bits, see AlertMask
        case ID_SYNC_MIN: return { { L"5 min", L"15 min", L"30 min" }, { 5, 15, 30 } };
        case ID_PERF_FPS: return { { L"60 fps", L"30 fps" }, { 60, 30 } };
        case ID_PERF_SENSOR: return { { L"1 s", L"2 s", L"5 s" }, { 1, 2, 5 } };
        default: return { { L"OpenAI", L"DeepSeek", L"Ollama local", L"Personalizado" }, { 0, 1, 2, 3 } }; // ID_AI_PROVIDER
        }
    }

    static std::wstring FieldText(int f) {
        Settings& s = g_settings;
        switch (f) {
        case F_HOST: return s.aiHost;
        case F_PATH: return s.aiPath;
        case F_MODEL: return AiPreset(s).model; // the model in use, so an empty field shows the provider's
        case F_PORT: return std::to_wstring(s.aiPort);
        case F_G_URL: return acc[1]->bridgeUrl;
        case F_ICS: return acc[3]->link;
        case F_WCITY: return s.weatherCityName;
        case F_MS_ID: return cfg.microsoftId;
        default: return L""; // F_KEY: never shown
        }
    }

    static void WriteSyncKey(const wchar_t* key, const std::wstring& v) {
        std::wstring path = SyncIniPath(), text;
        CreateSyncIni(path); // keeps the user's IDs; only creates the template when there is no file
        ReadUtf8File(path, text);
        WriteUtf8File(path, SetIniValue(text, key, v));
        cfg = LoadSyncConfig();
        Changed();
    }

    static void CommitField(int f, const std::wstring& v) {
        Settings& s = g_settings;
        switch (f) {
        case F_HOST: if (!v.empty()) s.aiHost = v; break;
        case F_PATH: if (!v.empty()) s.aiPath = v; break;
        case F_MODEL: s.aiModel = v; break;
        case F_WCITY: s.weatherCityName = TrimWs(v); break; // empty = the place is found automatically
        case F_PORT: { int p; if (ParseIntW(v, p) && p >= 1 && p <= 65535) s.aiPort = p; break; }
        case F_KEY: if (!v.empty()) hooks.setKey(v); return Invalidate(); // the key lives in DPAPI, not in the ini
        case F_G_URL: if (!acc[1]->connected) acc[1]->bridgeUrl = v; return Invalidate(); // checked and saved by Conectar
        case F_ICS: if (!acc[3]->connected) acc[3]->link = v; return Invalidate();         // checked and saved by Conectar
        case F_MS_ID: return WriteSyncKey(L"microsoft_client_id", v);
        }
        Changed();
    }

    // Ends the field being typed in: its text is saved (losing focus saves)
    static void CommitFocus() {
        if (focus < 0) return;
        int f = focus;
        std::wstring v = edit;
        focus = -1;
        edit.clear();
        CommitField(f, v);
        Invalidate();
    }

    static void Focus(int f) {
        CommitFocus();
        focus = f;
        edit = f == F_KEY ? L"" : FieldText(f); // the key is typed fresh, never read back
        Invalidate();
    }

    // Clipboard text as one line: no breaks or tabs, no spaces at the edges
    static std::wstring Clipboard() {
        std::wstring out;
        if (!OpenClipboard(wnd)) return out;
        if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) { out = p; GlobalUnlock(hd); }
        }
        CloseClipboard();
        out.erase(std::remove_if(out.begin(), out.end(), [](wchar_t c) { return c == L'\r' || c == L'\n' || c == L'\t'; }), out.end());
        size_t a = out.find_first_not_of(L' ');
        out = a == std::wstring::npos ? L"" : out.substr(a, out.find_last_not_of(L' ') - a + 1);
        return out;
    }

    static void Key(wchar_t ch) {
        if (focus < 0) {
            if (ch == 27) DestroyWindow(wnd); // Esc closes the window when no field has focus
            return;
        }
        const size_t maxLen = 500;
        if (ch == 22) { // Ctrl+V
            edit += Clipboard();
            if (edit.size() > maxLen) edit.resize(maxLen);
        } else if (ch == 8) {
            if (!edit.empty()) edit.pop_back();
        } else if (ch == 13 || ch == 27) { // Enter saves; Esc just leaves the field
            CommitFocus();
        } else if (ch == 9) { // Tab: the next field
            size_t i = 0;
            while (i < fieldOrder.size() && fieldOrder[i] != focus) ++i;
            if (!fieldOrder.empty()) Focus(fieldOrder[(i + 1) % fieldOrder.size()]);
            return;
        } else if (ch >= 32 && ch != 127 && edit.size() < maxLen) {
            edit += ch;
        }
        Invalidate();
    }

    static void Click(float x, float y) {
        CommitFocus();
        for (size_t i = hits.size(); i-- > 0;) {
            const Hit& h = hits[i];
            if (!h.r.Contains(x, y)) continue;
            switch (h.kind) {
            case HK_SECTION: SectionSet(h.a); break;
            case HK_BOOL:
                *BoolOf(h.a) = !*BoolOf(h.a);
                toggleId = h.a;
                toggleAt = GetTickCount64();
                Changed();
                Animate();
                break;
            case HK_STEP: {
                Num n = NumOf(h.a);
                *n.v = (std::max)(n.lo, (std::min)(n.hi, *n.v + h.b * n.step));
                Changed();
                break;
            }
            case HK_CHIP:
                if (h.a == ID_CATEGORY) {
                    g_settings.defaultCategory = h.b;
                } else if (h.a == ID_ALERT) { // h.b is one bit; Ninguno (0) clears them all
                    int mask = AlertMask(g_settings.defaultAlert, g_settings.defaultMoreAlerts);
                    SetAlertMask(h.b ? mask ^ h.b : 0, g_settings.defaultAlert, g_settings.defaultMoreAlerts);
                } else {
                    if (h.a == ID_AI_PROVIDER && g_settings.aiProvider != h.b) g_settings.aiModel.clear(); // each provider has its own model
                    *ChoiceOf(h.a) = h.b;
                }
                Changed();
                break;
            case HK_FIELD: Focus(h.a); break;
            case HK_BUTTON: DoButton(h.a, h.b); break;
            }
            break; // the top-most control under the pointer wins
        }
    }

    static void DoButton(int b, int src) {
        switch (b) {
        case B_MONITOR: hooks.command(1); break;
        case B_CLEAR_KEY: hooks.setKey(L""); Invalidate(); break;
        case B_DATA: ShellExecuteW(wnd, L"open", AppDataFile(L"").c_str(), NULL, NULL, SW_SHOWNORMAL); break; // shortcut: the files sit loose in %LOCALAPPDATA%, not in a folder of their own
        case B_QUIT: hooks.command(2); break;
        case B_GUIDE: ShellExecuteW(wnd, L"open", ExeFile(L"README.md").c_str(), NULL, NULL, SW_SHOWNORMAL); break; // shortcut: the README sits next to the exe, a project folder build only
        case B_COPY_SCRIPT: CopyScript(); break;
        default: hooks.account(src, b - B_ACC_CONNECT); break;
        }
        if (b == B_ACC_CONNECT || b == B_ACC_DISCONNECT || b == B_ACC_SYNC) Invalidate();
    }

    // The key is made once and reused, so a script copied before still matches. It is saved before the copy: a restart must
    // not lose the key the user is about to paste.
    static void CopyScript() {
        SyncAccount& g = *acc[1];
        if (g.bridgeToken.empty()) g.bridgeToken = RandomToken(24);
        if (g.bridgeToken.empty() || !SaveBridgeAccount(g)) return Invalidate(); // no random bytes, or no file: nothing to copy
        CopyText(BridgeScript(g.bridgeToken));
        scriptCopied = true;
        Invalidate();
    }

    static void CopyText(const std::wstring& text) {
        if (!OpenClipboard(wnd)) return;
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        wchar_t* p = h ? (wchar_t*)GlobalLock(h) : NULL;
        if (p) {
            std::copy(text.begin(), text.end(), p);
            p[text.size()] = L'\0';
            GlobalUnlock(h);
            if (SetClipboardData(CF_UNICODETEXT, h)) h = NULL; // the clipboard owns it now
        }
        if (h) GlobalFree(h);
        CloseClipboard();
    }

    static std::wstring ExeFile(const wchar_t* name) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(NULL, p, MAX_PATH);
        std::wstring s(p);
        return s.substr(0, s.find_last_of(L'\\') + 1) + name;
    }

    static const wchar_t* IconFace() {
        return Gdiplus::FontFamily(L"Segoe Fluent Icons").IsAvailable() ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }

    static void RoundPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
        float d = r * 2.0f;
        p.AddArc(x, y, d, d, 180, 90);
        p.AddArc(x + w - d, y, d, d, 270, 90);
        p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
        p.AddArc(x, y + h - d, d, d, 90, 90);
        p.CloseFigure();
    }

    static std::wstring Status(const SyncAccount& a) {
        if (a.syncing) return L"Sincronizando...";
        if (!a.error.empty()) return a.error;
        if (!a.connected) return L"Sin conectar";
        return a.account.empty() ? L"Conectado" : a.account;
    }

    static void Paint(HDC dc, float s) {
        using namespace Gdiplus;
        Bitmap bmp((INT)(W * s), (INT)(H * s), PixelFormat32bppARGB);
        bmp.SetResolution(96.0f, 96.0f); // else point sizes pick up the monitor DPI on top of s
        Graphics g(&bmp);
        g.ScaleTransform(s, s);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        hits.clear();
        fieldOrder.clear();
        Settings& st = g_settings;
        FontFamily fam(L"Segoe UI");
        FontFamily icons(IconFace());
        Font fTitle(&fam, 15.0f, FontStyleBold, UnitPoint);
        Font fBody(&fam, 10.0f, FontStyleRegular, UnitPoint);
        Font fSmall(&fam, 9.0f, FontStyleRegular, UnitPoint);
        Font fDesc(&fam, 7.5f, FontStyleRegular, UnitPoint);
        Font fIcon(&icons, 11.0f, FontStyleRegular, UnitPoint);
        Font fIconSmall(&icons, 8.0f, FontStyleRegular, UnitPoint);
        LinearGradientBrush side(RectF(0.0f, 0.0f, SIDE, H), Color(255, 22, 23, 29), Color(255, 16, 17, 21), LinearGradientModeVertical);
        SolidBrush surface(Color(255, 22, 23, 27)), chip(Color(255, 44, 46, 52));
        SolidBrush sideHot(Color(255, 28, 30, 36)), chipHot(Color(255, 69, 71, 76)); // hovered row; chip 12 % lighter
        SolidBrush accentB(ACCENT);
        SolidBrush textWhite(Color(255, 242, 242, 245)), textDim(Color(255, 142, 145, 152)), ink(Color(255, 8, 9, 12));
        SolidBrush todayFill(Color(255, 245, 245, 247)), green(Color(255, 74, 222, 128)), amber(Color(255, 251, 191, 36));
        SolidBrush red(Color(255, 239, 68, 68)), grey(Color(255, 90, 92, 98));
        Pen surfaceEdge(Color(26, 255, 255, 255), 1.0f), divider(Color(255, 48, 49, 55), 1.0f), selRing(Color(150, 255, 255, 255), 1.2f);
        Pen focusEdge(ACCENT, 1.0f);
        StringFormat center, leftMid;
        center.SetAlignment(StringAlignmentCenter);
        leftMid.SetAlignment(StringAlignmentNear);
        for (StringFormat* f : { &center, &leftMid }) {
            f->SetLineAlignment(StringAlignmentCenter);
            f->SetTrimming(StringTrimmingEllipsisCharacter);
            f->SetFormatFlags(StringFormatFlagsNoWrap);
        }

        g.Clear(Color(255, 14, 15, 18));
        SolidBrush sideSolid(Color(255, 20, 21, 25));
        g.FillRectangle(EffectivePerf().glow ? (Brush*)&side : (Brush*)&sideSolid, 0.0f, 0.0f, SIDE, H);
        g.DrawLine(&surfaceEdge, SIDE, 0.0f, SIDE, H);

        auto fillRound = [&](Brush* b, float x, float y, float w, float h, float r) {
            GraphicsPath p;
            RoundPath(p, x, y, w, h, r);
            g.FillPath(b, &p);
        };
        auto hit = [&](RectF r, int kind, int a, int b = 0) { // content hits only where the row is on screen
            if (r.Y + r.Height > TOP && r.Y < BOTTOM) hits.push_back({ r, kind, a, b });
        };
        auto hot = [&](const RectF& r) { return r.Contains(hx, hy) != FALSE; };

        // Sidebar: icon and name per section; the active one sits on a chip
        static const wchar_t* secIcon[] = { L"", L"", L"", L"", L"", L"", L"\uE706", L"\uE950", L"\uEC4A", L"", L"" };
        static const wchar_t* secName[] = { L"General", L"Píldora", L"Música", L"Pomodoro", L"Calendario", L"Cuentas", L"Clima", L"Sistema", L"Rendimiento", L"IA", L"Acerca de" };
        fillRound(&chip, 12.0f, ChipY(), 176.0f, 36.0f, 9.0f); // slides from the old section to the new one
        fillRound(&accentB, 12.0f, ChipY() + 9.0f, 3.0f, 18.0f, 1.5f); // accent bar on the chip's left edge
        for (int i = 0; i < SEC_COUNT; ++i) {
            float y = SIDE_TOP + i * SIDE_STEP;
            bool on = i == section;
            if (!on && hot(RectF(12.0f, y, 176.0f, 36.0f))) fillRound(&sideHot, 12.0f, y, 176.0f, 36.0f, 9.0f);
            g.DrawString(secIcon[i], -1, &fIcon, RectF(22.0f, y, 24.0f, 36.0f), &center, on ? &accentB : &textDim);
            g.DrawString(secName[i], -1, &fBody, RectF(50.0f, y, 140.0f, 36.0f), &leftMid, on ? &textWhite : &textDim);
            hits.push_back({ RectF(12.0f, y, 176.0f, 36.0f), HK_SECTION, i, 0 });
        }

        g.DrawString(secName[section], -1, &fTitle, RectF(CX, 24.0f, 520.0f, 32.0f), &leftMid, &textWhite);

        // Row helpers. ry is the top of the current row; controls sit in its middle band.
        float ry = TOP - scrollY;
        float top0 = ry;
        g.SetClip(RectF(CX - 8.0f, TOP, CR - CX + 16.0f, BOTTOM - TOP));
        float ease = Ease(secAt, SECTION_MS);
        GraphicsState content = g.Save();
        g.TranslateTransform(0.0f, 12.0f * (1.0f - ease)); // the new section rises 12 px into place
        auto row = [&](const std::wstring& name, const std::wstring& desc, float descW = 300.0f) {
            g.DrawString(name.c_str(), -1, &fBody, RectF(CX, ry + 9.0f, descW, 20.0f), &leftMid, &textWhite);
            g.DrawString(desc.c_str(), -1, &fDesc, RectF(CX, ry + 29.0f, descW, 16.0f), &leftMid, &textDim);
            g.DrawLine(&divider, CX, ry + ROW - 1.0f, CR, ry + ROW - 1.0f);
        };
        auto next = [&] { ry += ROW; };
        auto toggle = [&](int id) {
            bool on = *BoolOf(id);
            float t = on ? 1.0f : 0.0f; // 0 off, 1 on; only the last clicked toggle is mid-way
            if (id == toggleId) t = on ? Ease(toggleAt, TOGGLE_MS) : 1.0f - Ease(toggleAt, TOGGLE_MS);
            float x = CR - 40.0f, y = ry + 17.0f;
            SolidBrush track(Mix(Color(255, 44, 46, 52), ACCENT, t));
            fillRound(&track, x, y, 40.0f, 22.0f, 11.0f);
            SolidBrush knob(Mix(Color(255, 142, 145, 152), Color(255, 255, 255, 255), t));
            g.FillEllipse(&knob, x + 3.0f + 18.0f * t, y + 3.0f, 16.0f, 16.0f);
            hit(RectF(x, y, 40.0f, 22.0f), HK_BOOL, id);
        };
        auto stepper = [&](int id) {
            Num n = NumOf(id);
            float x = CR - 124.0f, y = ry + 15.0f;
            fillRound(&chip, x, y, 124.0f, 26.0f, 8.0f);
            g.DrawString(L"", -1, &fIconSmall, RectF(x, y, 30.0f, 26.0f), &center, hot(RectF(x, y, 30.0f, 26.0f)) ? &accentB : &textWhite);
            g.DrawString(L"", -1, &fIconSmall, RectF(x + 94.0f, y, 30.0f, 26.0f), &center, hot(RectF(x + 94.0f, y, 30.0f, 26.0f)) ? &accentB : &textWhite);
            g.DrawString((std::to_wstring(*n.v) + n.unit).c_str(), -1, &fSmall, RectF(x + 30.0f, y, 64.0f, 26.0f), &center, &textWhite);
            hit(RectF(x, y, 30.0f, 26.0f), HK_STEP, id, -1);
            hit(RectF(x + 94.0f, y, 30.0f, 26.0f), HK_STEP, id, 1);
        };
        auto chips = [&](int id) {
            Choice c = ChoicesOf(id);
            const bool alerts = id == ID_ALERT; // several can be on: its bits, not one value
            const int cur = alerts ? 0 : *ChoiceOf(id);
            const int mask = AlertMask(g_settings.defaultAlert, g_settings.defaultMoreAlerts);
            std::vector<float> w;
            float total = -6.0f;
            for (const auto& label : c.labels) {
                RectF box;
                g.MeasureString(label.c_str(), -1, &fSmall, PointF(0.0f, 0.0f), &box);
                w.push_back(box.Width + 20.0f);
                total += w.back() + 6.0f;
            }
            float x = CR - total, y = ry + 15.0f;
            for (size_t i = 0; i < c.labels.size(); ++i) {
                bool on = alerts ? (c.values[i] ? (mask & c.values[i]) != 0 : mask == 0) : cur == c.values[i];
                fillRound(on ? (Brush*)&todayFill : hot(RectF(x, y, w[i], 26.0f)) ? (Brush*)&chipHot : (Brush*)&chip, x, y, w[i], 26.0f, 8.0f);
                g.DrawString(c.labels[i].c_str(), -1, &fSmall, RectF(x, y, w[i], 26.0f), &center, on ? &ink : &textWhite);
                hit(RectF(x, y, w[i], 26.0f), HK_CHIP, id, c.values[i]);
                x += w[i] + 6.0f;
            }
        };
        auto dots = [&](int id) {
            int cur = *ChoiceOf(id);
            for (int i = 0; i < CAL_CAT_COUNT; ++i) {
                float cx = CR - 12.0f - (CAL_CAT_COUNT - 1 - i) * 24.0f, cy = ry + 28.0f;
                SolidBrush c(Color(255, CAL_CATS[i].r, CAL_CATS[i].g, CAL_CATS[i].b));
                g.FillEllipse(&c, cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);
                if (i == cur) g.DrawEllipse(&selRing, cx - 9.0f, cy - 9.0f, 18.0f, 18.0f);
                hit(RectF(cx - 12.0f, ry + 16.0f, 24.0f, 24.0f), HK_CHIP, id, i);
            }
        };
        auto field = [&](int f, float x, float w) {
            float y = ry + 15.0f;
            bool on = focus == f, hasKey = f == F_KEY && hooks.hasKey();
            std::wstring shown;
            bool placeholder = false;
            if (on) {
                shown = (f == F_KEY ? std::wstring(edit.size(), L'•') : edit) + L"|";
                if (shown.size() > 30) shown = L"…" + shown.substr(shown.size() - 29); // the tail stays visible with the caret
            } else if (f == F_KEY) {
                shown = hasKey ? std::wstring(12, L'•') : L"Sin clave";
                placeholder = !hasKey;
            } else {
                shown = FieldText(f);
                if (shown.empty()) { shown = L"Sin definir"; placeholder = true; }
            }
            GraphicsPath p;
            RoundPath(p, x, y, w, 26.0f, 8.0f);
            g.FillPath(&surface, &p);
            g.DrawPath(on ? &focusEdge : &surfaceEdge, &p);
            g.DrawString(shown.c_str(), -1, &fSmall, RectF(x + 10.0f, y, w - 20.0f, 26.0f), &leftMid, placeholder ? &textDim : &textWhite);
            hit(RectF(x, y, w, 26.0f), HK_FIELD, f);
            fieldOrder.push_back(f);
        };
        auto button = [&](float x, int id, int b, float w, const wchar_t* text) {
            fillRound(hot(RectF(x, ry + 15.0f, w, 26.0f)) ? (Brush*)&chipHot : (Brush*)&chip, x, ry + 15.0f, w, 26.0f, 8.0f);
            g.DrawString(text, -1, &fSmall, RectF(x, ry + 15.0f, w, 26.0f), &center, &textWhite);
            hit(RectF(x, ry + 15.0f, w, 26.0f), HK_BUTTON, id, b);
        };
        auto account = [&](int src, const wchar_t* name) {
            SyncAccount& a = *acc[src];
            SolidBrush& c = a.syncing ? amber : !a.connected ? grey : !a.error.empty() ? red : green;
            g.FillEllipse(&c, CX + 4.0f, ry + 24.0f, 8.0f, 8.0f);
            g.DrawString(name, -1, &fBody, RectF(CX + 18.0f, ry + 9.0f, 220.0f, 20.0f), &leftMid, &textWhite);
            g.DrawString(Status(a).c_str(), -1, &fDesc, RectF(CX + 18.0f, ry + 29.0f, 220.0f, 16.0f), &leftMid, &textDim);
            g.DrawLine(&divider, CX, ry + ROW - 1.0f, CR, ry + ROW - 1.0f);
            if (!a.connected) {
                if (src == 2) button(CR - 100.0f, B_ACC_CONNECT, src, 100.0f, L"Conectar"); // Google connects from its URL row
            } else {
                button(CR - 100.0f, B_ACC_DISCONNECT, src, 100.0f, L"Desconectar");
                button(CR - 208.0f, B_ACC_SYNC, src, 100.0f, L"Sincronizar");
            }
        };

        switch (section) {
        case SEC_GENERAL:
            row(L"Iniciar con Windows", L"Abre Capri Notch al iniciar sesión"); toggle(ID_START); next();
            row(L"Abrir al pasar el ratón", L"Si lo apagas, solo se abre con un clic"); toggle(ID_HOVER_OPEN); next();
            row(L"Retraso al abrir", L"Lo que tarda en abrirse con el puntero encima"); stepper(ID_HOVER_MS); next();
            row(L"Retraso al cerrar", L"Gracia antes de contraerse al salir el puntero"); stepper(ID_LEAVE_MS); next();
            row(L"Esconder al hacer clic al lado", L"Un clic junto a la píldora la recoge"); toggle(ID_TUCK); next();
            row(L"Ocultar en pantalla completa", L"Se oculta con juegos, vídeos y presentaciones"); toggle(ID_FULLSCREEN); next();
            row(L"Monitor", L"Muestra la píldora en la siguiente pantalla"); button(CR - 170.0f, B_MONITOR, 0, 170.0f, L"Mover al otro monitor"); next();
            break;
        case SEC_PILL:
            row(L"Dormir tras", L"Cierra los ojos tras estos minutos sin usar el equipo"); stepper(ID_SLEEP); next();
            row(L"Aviso del cargador", L"Aviso al enchufar o desenchufar"); toggle(ID_CHARGER); next();
            row(L"Letra en la píldora", L"La línea que suena, si las letras están activas"); toggle(ID_LYRICS); next();
            break;
        case SEC_MUSIC:
            row(L"Letra en la píldora", L"Igual que en Píldora"); toggle(ID_LYRICS); next();
            row(L"Compacta: carátula", L"La portada del disco a la izquierda"); toggle(ID_PILL_ART); next();
            row(L"Compacta: artista", L"Título · artista en vez de solo el título"); toggle(ID_PILL_ARTIST); next();
            row(L"Letras", L"LRCLIB: gratis, sin cuenta. Actívalas con el botón de la pestaña Música."); next();
            break;
        case SEC_POMODORO:
            row(L"Enfoque", L"Duración de cada sesión de trabajo"); stepper(ID_FOCUS); next();
            row(L"Descanso", L"Duración de cada pausa"); stepper(ID_BREAK); next();
            row(L"Sonido al terminar", L"Pitido al cambiar de fase"); toggle(ID_POMO_SOUND); next();
            row(L"Compacta: verlo parado", L"El temporizador en la píldora aunque no corra"); toggle(ID_PILL_POMO_IDLE); next();
            row(L"Compacta: barra de progreso", L"Línea roja bajo la píldora"); toggle(ID_PILL_POMO_BAR); next();
            row(L"Compacta: sesiones", L"Añade las sesiones completadas hoy"); toggle(ID_PILL_POMO_SESSIONS); next();
            break;
        case SEC_CALENDAR:
            row(L"Vista inicial", L"Con la que se abre el calendario al arrancar", 200.0f); chips(ID_CAL_VIEW); next();
            row(L"Aviso por defecto", L"Para los eventos nuevos", 200.0f); chips(ID_ALERT); next();
            row(L"Categoría por defecto", L"Para los eventos nuevos", 200.0f); dots(ID_CATEGORY); next();
            row(L"Sonido de recordatorios", L"Pitido cuando llega el aviso de un evento"); toggle(ID_REMIND_SOUND); next();
            row(L"Tareas con fecha en el calendario", L"Las tareas con día u hora se ven en el calendario y al revés"); toggle(ID_TASKS_CAL); next();
            break;
        case SEC_ACCOUNTS:
            account(1, L"Google Calendar"); next();
            row(L"1. Script del puente", L"Pégalo en un proyecto nuevo de script.google.com"); button(CR - 170.0f, B_COPY_SCRIPT, 0, 170.0f, scriptCopied ? L"Copiado" : L"Copiar script"); next();
            row(L"2. URL de la aplicación web", acc[1]->connected ? L"Desconecta para cambiarla" : L"Termina en /exec", 186.0f);
            field(F_G_URL, CR - 330.0f, 222.0f);
            if (!acc[1]->connected) button(CR - 100.0f, B_ACC_CONNECT, 1, 100.0f, L"Conectar");
            next();
            account(2, L"Outlook"); next();
            row(L"ID de aplicación de Microsoft", L"Id. de aplicación (cliente) de Azure", 240.0f); field(F_MS_ID, CR - 260.0f, 260.0f); next();
            account(3, L"Calendario por enlace"); next();
            row(L"Dónde está el enlace", L"Outlook: Configuración → Calendario → Calendarios compartidos → Publicar un calendario → enlace ICS", 500.0f); next();
            row(L"Enlace ICS", acc[3]->connected ? L"Desconecta para cambiarlo" : L"Solo lectura. Se guarda cifrado", 186.0f);
            field(F_ICS, CR - 330.0f, 222.0f);
            if (!acc[3]->connected) button(CR - 100.0f, B_ACC_CONNECT, 3, 100.0f, L"Conectar");
            next();
            row(L"Intervalo de sincronización", L"Cada cuánto se sincronizan las cuentas", 200.0f); chips(ID_SYNC_MIN); next();
            row(L"Guía paso a paso", L"Pasos del script de Google y del ID de Outlook"); button(CR - 110.0f, B_GUIDE, 0, 110.0f, L"Ver guía"); next();
            row(L"Samsung", L"Guarda tus eventos en Google u Outlook desde el móvil"); next();
            break;
        case SEC_WEATHER:
            row(L"Detectar mi ubicación", L"Ubicación de Windows o, si no, aproximada por tu IP"); toggle(ID_WEATHER_AUTO); next();
            row(L"Ciudad", L"Vacío = automática. Ejemplo: Bogotá", 240.0f); field(F_WCITY, CR - 260.0f, 260.0f); next();
            break;
        case SEC_SYSTEM:
            row(L"Temperaturas", L"CPU y GPU, cuando Windows las ofrece"); toggle(ID_SYS_TEMPS); next();
            row(L"Potencia de la batería", L"Vatios que entran o salen"); toggle(ID_SYS_POWER); next();
            row(L"Vista compacta", L"El sistema en la píldora, con esta pestaña elegida"); toggle(ID_PILL_SYS); next();
            row(L"Compacta: CPU", L"Uso y, con Temperaturas, sus grados"); toggle(ID_PILL_SYS_CPU); next();
            row(L"Compacta: GPU", L"Temperatura de la gráfica"); toggle(ID_PILL_SYS_GPU); next();
            row(L"Compacta: memoria", L"Porcentaje de RAM en uso"); toggle(ID_PILL_SYS_RAM); next();
            row(L"Compacta: disco", L"Espacio libre en C:"); toggle(ID_PILL_SYS_DISK); next();
            row(L"Compacta: batería", L"A la derecha: vatios, o el porcentaje si no hay dato"); toggle(ID_PILL_SYS_POWER); next();
            break;
        case SEC_PERF:
            row(L"Animaciones", L"Rebote al abrir, cambio de pestaña y transiciones"); toggle(ID_PERF_ANIM); next();
            row(L"Fotogramas", L"Menos fotogramas, menos uso de CPU"); chips(ID_PERF_FPS); next();
            row(L"Luz ambiental y degradados", L"El brillo de color del panel y los fondos degradados"); toggle(ID_PERF_GLOW); next();
            row(L"Lectura de sensores", L"Cada cuánto se leen CPU, GPU y batería en Sistema"); chips(ID_PERF_SENSOR); next();
            row(L"Ahorro con batería", L"Sin animaciones ni luces y a 30 fps al desenchufar"); toggle(ID_PERF_SAVER); next();
            row(L"Ecualizador", L"Barras animadas en la píldora mientras suena música"); toggle(ID_EQUALIZER); next();
            row(L"Ojos", L"Mascota que parpadea en reposo; sin ojos, un calendario"); toggle(ID_EYES); next();
            break;
        case SEC_AI:
            row(L"Proveedor", L"Dónde se envían tus preguntas", 150.0f); chips(ID_AI_PROVIDER); next();
            row(L"Clave de API", L"Cifrada con tu cuenta de Windows", 230.0f);
            button(CR - 70.0f, B_CLEAR_KEY, 0, 70.0f, L"Borrar");
            field(F_KEY, CR - 248.0f, 170.0f);
            next();
            row(L"Modelo", L"Vacío = el que usa el proveedor", 240.0f); field(F_MODEL, CR - 260.0f, 260.0f); next();
            if (st.aiProvider == 3) {
                row(L"Servidor", L"Host, sin https://", 240.0f); field(F_HOST, CR - 260.0f, 260.0f); next();
                row(L"Ruta", L"Por ejemplo /v1/chat/completions", 240.0f); field(F_PATH, CR - 260.0f, 260.0f); next();
                row(L"Puerto", L"443 para https, 11434 para Ollama", 240.0f); field(F_PORT, CR - 100.0f, 80.0f); next();
                row(L"HTTPS", L"Conexión cifrada"); toggle(ID_AI_HTTPS); next();
            }
            break;
        case SEC_ABOUT: {
            PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
            GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
            row(L"Capri Notch", L"Versión " + std::wstring(APP_VERSION) + L" · " + std::to_wstring(pmc.WorkingSetSize >> 20) + L" MB en uso");
            next();
            row(L"Carpeta de datos", L"Ajustes, eventos, tareas y tokens"); button(CR - 110.0f, B_DATA, 0, 110.0f, L"Abrir carpeta"); next();
            row(L"Salir", L"Cierra Capri Notch hasta que lo vuelvas a abrir"); button(CR - 180.0f, B_QUIT, 0, 180.0f, L"Salir de Capri Notch"); next();
            break;
        }
        }
        g.Restore(content);
        if (ease < 1) { // the content fades in over the background colour
            SolidBrush veil(Color((BYTE)(255 * (1 - ease)), 14, 15, 18));
            g.FillRectangle(&veil, CX - 8.0f, TOP, CR - CX + 16.0f, BOTTOM - TOP);
        }
        contentH = ry - top0;
        g.ResetClip();

        Graphics screen(dc);
        screen.DrawImage(&bmp, 0, 0, bmp.GetWidth(), bmp.GetHeight());
    }
};
