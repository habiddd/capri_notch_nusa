#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <oleacc.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>
#include "settings.h"
#include "sync.h"
#include "events.h"
#include "whistle.h"

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
    enum Section { SEC_GENERAL, SEC_PILL, SEC_POMODORO, SEC_CALENDAR, SEC_ACCOUNTS, SEC_WEATHER, SEC_SYSTEM, SEC_PERF, SEC_AI, SEC_ABOUT, SEC_COUNT };

    // The tab gear opens the section of its tab
    static int SectionOfTab(int tab) { static const int s[] = { SEC_PILL, SEC_POMODORO, SEC_CALENDAR, SEC_AI, SEC_WEATHER, SEC_SYSTEM, SEC_GENERAL }; return s[tab]; }

    static void Open(HWND notch, int section, SyncAccount* google, SyncAccount* outlook, SyncAccount* ics, const SettingsHooks& h) {
        CommitFocus(); // a field still being typed is saved before the old hooks and accounts go
        notchWnd = notch;
        acc[1] = google;
        acc[2] = outlook;
        acc[3] = ics;
        hooks = h;
        cfg = LoadSyncConfig();
        scriptCopied = false;
        focus = -1;
        edit.clear();
        fieldErr = -1;
        fieldErrText.clear();
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
            if (LOWORD(l) == HTCLIENT && HitAt(hx, hy) >= 0) {
                SetCursor(LoadCursor(NULL, hits[HitAt(hx, hy)].kind == HK_FIELD ? IDC_IBEAM : IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: Click((float)(short)LOWORD(l) / Scale(h), (float)(short)HIWORD(l) / Scale(h)); return 0;
        case WM_MOUSEWHEEL: {
            float maxScroll = (std::max)(0.0f, contentH - (BOTTOM - TOP));
            scrollY = (std::max)(0.0f, (std::min)(maxScroll, scrollY - GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 40.0f));
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        case WM_CHAR: Key((wchar_t)w); return 0;
        case WM_GETOBJECT: // screen readers ask for the client area; its children are the painted controls
            if ((LONG)l == (LONG)OBJID_CLIENT) {
                if (!accObj) accObj = new Accessible(h);
                return LresultFromObject(IID_IAccessible, w, accObj);
            }
            break;
        case WM_DESTROY:
            KillTimer(h, 1);
            CommitFocus();
            if (accObj) { accObj->Release(); accObj = nullptr; }
            wnd = NULL;
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

private:
    // Layout in logical px (the window is scaled by DPI). Rows scroll inside [TOP, BOTTOM).
    static constexpr float W = 780, H = 540, SIDE = 200, CX = 232, CR = 756, TOP = 84, BOTTOM = 524, ROW = 56;
    static constexpr float SIDE_TOP = 40.0f, SIDE_STEP = 44.0f; // sidebar rows: 10 of them, so they start above the content's TOP

    // Controls: what a row is, by id
    enum Id { ID_START, ID_HOVER_OPEN, ID_HOVER_MS, ID_LEAVE_MS, ID_TUCK, ID_FULLSCREEN, ID_EYES, ID_SLEEP, ID_EQUALIZER,
              ID_CHARGER, ID_LYRICS, ID_FOCUS, ID_BREAK, ID_POMO_SOUND, ID_CAL_VIEW, ID_ALERT, ID_CATEGORY, ID_REMIND_SOUND,
              ID_SYNC_MIN, ID_AI_PROVIDER, ID_AI_HTTPS, ID_WEATHER_AUTO, ID_SYS_TEMPS, ID_SYS_POWER, ID_TASKS_CAL,
              ID_PILL_ART, ID_PILL_ARTIST, ID_PILL_POMO_IDLE, ID_PILL_POMO_BAR, ID_PILL_POMO_SESSIONS, ID_PILL_SYS, ID_PILL_SYS_CPU,
              ID_PILL_SYS_GPU, ID_PILL_SYS_RAM, ID_PILL_SYS_DISK, ID_PILL_SYS_POWER, // the ID_PILL_ block follows PillFlags' order
              ID_PERF_ANIM, ID_PERF_GLOW, ID_PERF_SAVER, ID_PERF_FPS, ID_PERF_SENSOR, ID_SHELF_THUMBS, ID_GLOW_SOURCE, ID_HAND_ENGINE, ID_IDLE_TUCK };
    enum Field { F_HOST, F_PATH, F_MODEL, F_PORT, F_KEY, F_G_URL, F_MS_ID, F_ICS, F_WCITY };
    enum BtnId { B_MONITOR, B_CLEAR_KEY, B_DATA, B_QUIT, B_GUIDE, B_COPY_SCRIPT, B_WHISTLE, B_HAND, B_GLOW_COLOR, B_ACC_CONNECT, B_ACC_DISCONNECT, B_ACC_SYNC };
    enum HitKind { HK_SECTION, HK_BOOL, HK_STEP, HK_CHIP, HK_FIELD, HK_BUTTON };

    struct Hit { Gdiplus::RectF r; int kind, a, b; std::wstring name; bool on = false, shown = true; };
    struct Num { int* v; int lo, hi, step; const wchar_t* unit; };
    struct Choice { std::vector<std::wstring> labels; std::vector<int> values; };

    static inline HWND wnd = NULL, notchWnd = NULL;
    static inline int section = SEC_GENERAL;
    static inline float scrollY = 0, contentH = 0;
    static inline int focus = -1; // field being typed in, -1 none
    static inline std::wstring edit;
    static inline int fieldErr = -1; // field whose last value was refused, -1 none
    static inline std::wstring fieldErrText;
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
        for (size_t i = hits.size(); i-- > 0;) if (hits[i].shown && hits[i].r.Contains(x, y)) return (int)i; // a scrolled-out control is not under the pointer
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
        case ID_SHELF_THUMBS: return &s.shelfThumbs;
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
        case ID_GLOW_SOURCE: return &s.glowSource;
        case ID_HAND_ENGINE: return &s.handEngine;
        case ID_IDLE_TUCK: return &s.idleTuckSec;
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
        case ID_GLOW_SOURCE: return { { L"Clásico", L"Fondo", L"Windows", L"Propio" }, { 0, 1, 2, 3 } };
        case ID_HAND_ENGINE: return { { L"MediaPipe", L"RTMPose" }, { 0, 1 } };
        case ID_IDLE_TUCK: return { { L"Nunca", L"10 s", L"30 s", L"1 min", L"5 min" }, { 0, 10, 30, 60, 300 } };
        default: return { { L"OpenAI", L"DeepSeek", L"Ollama local", L"Personalizado" }, { 0, 1, 2, 3 } }; // ID_AI_PROVIDER
        }
    }

    static std::wstring FieldText(int f) {
        Settings& s = g_settings;
        switch (f) {
        case F_HOST: return s.aiHost;
        case F_PATH: return s.aiPath;
        case F_MODEL: return s.aiModel;
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
        const wchar_t* err = NULL; // a refused value keeps the old one and says why
        switch (f) {
        case F_HOST: if (v.empty()) err = L"El servidor no puede quedar vacío"; else s.aiHost = v; break;
        case F_PATH: if (v.empty()) err = L"La ruta no puede quedar vacía"; else s.aiPath = v; break;
        case F_MODEL: s.aiModel = v; break;
        case F_WCITY: s.weatherCityName = TrimWs(v); break; // empty = the place is found automatically
        case F_PORT: { int p; if (ParseIntW(v, p) && p >= 1 && p <= 65535) s.aiPort = p; else err = L"Escribe un puerto entre 1 y 65535"; break; }
        case F_KEY: if (!v.empty()) hooks.setKey(v); return Invalidate(); // the key lives in DPAPI, not in the ini
        case F_G_URL: if (!acc[1]->connected) acc[1]->bridgeUrl = v; return Invalidate(); // checked and saved by Conectar
        case F_ICS: if (!acc[3]->connected) acc[3]->link = v; return Invalidate();         // checked and saved by Conectar
        case F_MS_ID: return WriteSyncKey(L"microsoft_client_id", v);
        }
        if (err) {
            fieldErr = f;
            fieldErrText = err;
        } else if (fieldErr == f) {
            fieldErr = -1;
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
        if (f != F_KEY && v == FieldText(f)) { Invalidate(); return; } // passed over unchanged: nothing to save
        CommitField(f, v);
        Invalidate();
    }

    static void Focus(int f) {
        CommitFocus();
        if (f == fieldErr) fieldErr = -1; // the user is fixing it
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
        } else if (ch == 13) { // Enter saves; Esc drops what was typed
            CommitFocus();
        } else if (ch == 27) {
            focus = -1;
            edit.clear();
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
        int i = HitAt(x, y); // the top-most control under the pointer wins
        if (i < 0) return;
        Hit h = hits[i]; // copied: Activate can change what the next paint builds
        Activate(h);
    }

    static void Activate(const Hit& h) {
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
    }


    static void DoButton(int b, int src) {
        switch (b) {
        case B_MONITOR: hooks.command(1); break;
        case B_CLEAR_KEY:
            if (Ask(L"¿Borrar la clave de API? Tendrás que pegarla de nuevo para usar la IA.")) hooks.setKey(L"");
            Invalidate();
            break;
        case B_DATA: ShellExecuteW(wnd, L"open", AppDataFile(L"").c_str(), NULL, NULL, SW_SHOWNORMAL); break; // shortcut: the files sit loose in %LOCALAPPDATA%, not in a folder of their own
        case B_QUIT:
            if (Ask(L"¿Salir de Capri Notch? Para volver a abrirlo, usa el acceso directo del Escritorio.")) hooks.command(2);
            break;
        case B_GUIDE: ShellExecuteW(wnd, L"open", ExeFile(L"README.md").c_str(), NULL, NULL, SW_SHOWNORMAL); break; // shortcut: the README sits next to the exe, a project folder build only
        case B_COPY_SCRIPT: CopyScript(); break;
        case B_WHISTLE: // src is the model. Installed: remove it (fails quietly while the notch is using it); else download it
            if (g_cactusDownload[src] == 1) break;
            if (CactusInstalled(src)) {
                if (!Ask((L"¿Borrar " + std::wstring(CACTUS_NAME[src]) + L"? Para usarlo otra vez hay que descargarlo de nuevo.").c_str())) break;
                DeleteFileW(CactusModel(src).c_str());
                if (!CactusInstalled(1 - src)) DeleteFileW(WhistleDll().c_str()); // the runtime goes with the last model
            } else {
                CactusDownload(src);
            }
            Invalidate();
            break;
        case B_HAND: // src is the hand engine. Installed: remove its two files (the running camera keeps them in memory); else download them
            if (g_handDownload[src] == 1) break;
            if (HandInstalled(src)) {
                if (!Ask((L"¿Borrar " + std::wstring(src == HAND_RTMPOSE ? L"RTMPose" : L"MediaPipe") + L"? Para usarlo otra vez hay que descargarlo de nuevo.").c_str())) break;
                DeleteFileW(HandModel(src, 0).c_str());
                DeleteFileW(HandModel(src, 1).c_str());
            } else {
                HandDownload(src);
            }
            Invalidate();
            break;
        case B_GLOW_COLOR: { // src 0 left, 1 right. Windows' own color dialog, loaded only for this click so it costs no memory otherwise
            int& v = src ? g_settings.glowRight : g_settings.glowLeft;
            static COLORREF custom[16];
            CHOOSECOLORW cc = { sizeof cc };
            cc.hwndOwner = wnd;
            cc.lpCustColors = custom;
            cc.rgbResult = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
            cc.Flags = CC_RGBINIT | CC_FULLOPEN;
            typedef BOOL (WINAPI *ChooseFn)(CHOOSECOLORW*);
            HMODULE dlg = LoadLibraryExW(L"comdlg32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
            ChooseFn choose = dlg ? (ChooseFn)(void*)GetProcAddress(dlg, "ChooseColorW") : NULL;
            if (choose && choose(&cc)) {
                v = GetRValue(cc.rgbResult) << 16 | GetGValue(cc.rgbResult) << 8 | GetBValue(cc.rgbResult);
                Changed();
            }
            if (dlg) FreeLibrary(dlg);
            break;
        }
        case B_ACC_DISCONNECT: // the question is the notch's, so this window is off meanwhile: no second click
            if (wnd) EnableWindow(wnd, FALSE);
            { auto run = hooks.account; run(src, 1); } // a copy: reopening from the tray during the question replaces hooks
            if (wnd) { EnableWindow(wnd, TRUE); SetForegroundWindow(wnd); }
            break;
        default: { auto run = hooks.account; run(src, b - B_ACC_CONNECT); break; } // same: connect can ask too
        }
        if (b == B_ACC_CONNECT || b == B_ACC_DISCONNECT || b == B_ACC_SYNC) Invalidate();
    }

    // Destructive actions ask first; No is the default button
    static bool Ask(const wchar_t* text) {
        return MessageBoxW(wnd, text, L"Capri Notch", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
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

    // Narrator and other screen readers: the painted controls as MSAA children; child id = index in hits + 1
    struct Accessible : IAccessible {
        LONG refs = 1;
        IAccessible* sys = nullptr; // Windows' own object for the client area: parent, window name, location
        explicit Accessible(HWND h) { CreateStdAccessibleObject(h, OBJID_CLIENT, IID_IAccessible, (void**)&sys); }
        virtual ~Accessible() { if (sys) sys->Release(); }
        static const Hit* Child(const VARIANT& v) {
            if (!wnd || v.vt != VT_I4 || v.lVal < 1 || v.lVal > (LONG)hits.size()) return nullptr;
            return &hits[v.lVal - 1];
        }
        static bool Self(const VARIANT& v) { return v.vt == VT_I4 && v.lVal == CHILDID_SELF; }
        // IUnknown
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** p) override {
            if (id == IID_IUnknown || id == IID_IDispatch || id == IID_IAccessible) { *p = static_cast<IAccessible*>(this); AddRef(); return S_OK; }
            *p = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
        ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
        // IDispatch: screen readers do not use it
        HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override { *n = 0; return S_OK; }
        HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
        // IAccessible
        HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** p) override { *p = nullptr; return sys ? sys->get_accParent(p) : S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accChildCount(long* n) override { *n = wnd ? (long)hits.size() : 0; return S_OK; }
        HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** p) override { *p = nullptr; return S_FALSE; } // simple elements
        HRESULT STDMETHODCALLTYPE get_accName(VARIANT v, BSTR* s) override {
            *s = nullptr;
            if (Self(v)) return sys ? sys->get_accName(v, s) : S_FALSE;
            const Hit* h = Child(v);
            if (!h) return E_INVALIDARG;
            *s = SysAllocString(h->name.c_str());
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_accValue(VARIANT v, BSTR* s) override {
            *s = nullptr;
            const Hit* h = Child(v);
            if (!h || h->kind != HK_FIELD || h->a == F_KEY) return S_FALSE; // the API key is never read out
            *s = SysAllocString((focus == h->a ? edit : FieldText(h->a)).c_str());
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR* s) override { *s = nullptr; return S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accRole(VARIANT v, VARIANT* r) override {
            if (Self(v)) return sys ? sys->get_accRole(v, r) : S_FALSE;
            const Hit* h = Child(v);
            if (!h) return E_INVALIDARG;
            static const long roles[] = { ROLE_SYSTEM_PAGETAB, ROLE_SYSTEM_CHECKBUTTON, ROLE_SYSTEM_PUSHBUTTON, ROLE_SYSTEM_RADIOBUTTON, ROLE_SYSTEM_TEXT, ROLE_SYSTEM_PUSHBUTTON }; // by HitKind
            r->vt = VT_I4;
            r->lVal = h->kind == HK_CHIP && h->a == ID_ALERT ? ROLE_SYSTEM_CHECKBUTTON : roles[h->kind]; // alert chips are several, so checks
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_accState(VARIANT v, VARIANT* r) override {
            if (Self(v)) return sys ? sys->get_accState(v, r) : S_FALSE;
            const Hit* h = Child(v);
            if (!h) return E_INVALIDARG;
            long st = STATE_SYSTEM_FOCUSABLE;
            if (h->kind == HK_FIELD && h->a == focus && GetFocus() == wnd) st |= STATE_SYSTEM_FOCUSED; // the field being typed in
            if (h->on) st |= h->kind == HK_SECTION ? STATE_SYSTEM_SELECTED : STATE_SYSTEM_CHECKED;
            if (h->kind == HK_SECTION) st |= STATE_SYSTEM_SELECTABLE;
            if (!h->shown) st |= STATE_SYSTEM_OFFSCREEN;
            r->vt = VT_I4;
            r->lVal = st;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* s) override { *s = nullptr; return S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* f, VARIANT, long* t) override { *f = nullptr; *t = 0; return S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR* s) override { *s = nullptr; return S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* r) override {
            int i = -1;
            for (size_t j = 0; j < hits.size() && focus >= 0; ++j) if (hits[j].kind == HK_FIELD && hits[j].a == focus) { i = (int)j; break; }
            if (!wnd || GetFocus() != wnd) { r->vt = VT_EMPTY; return S_FALSE; }
            r->vt = VT_I4;
            r->lVal = i >= 0 ? i + 1 : CHILDID_SELF;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* r) override { r->vt = VT_EMPTY; return S_FALSE; }
        HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT v, BSTR* s) override {
            *s = nullptr;
            const Hit* h = Child(v);
            if (!h) return S_FALSE;
            *s = SysAllocString(h->kind == HK_FIELD ? L"Editar" : L"Pulsar");
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE accLocation(long* x, long* y, long* w, long* h, VARIANT v) override {
            if (Self(v)) return sys ? sys->accLocation(x, y, w, h, v) : S_FALSE;
            const Hit* c = Child(v);
            if (!c) return E_INVALIDARG;
            float s = Scale(wnd);
            POINT p = { (LONG)(c->r.X * s), (LONG)(c->r.Y * s) };
            ClientToScreen(wnd, &p);
            *x = p.x; *y = p.y; *w = (long)(c->r.Width * s); *h = (long)(c->r.Height * s);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE accNavigate(long dir, VARIANT start, VARIANT* end) override {
            if (Self(start) && sys) return sys->accNavigate(dir, start, end);
            end->vt = VT_EMPTY;
            return S_FALSE;
        }
        HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT* r) override {
            if (!wnd) { r->vt = VT_EMPTY; return S_FALSE; }
            POINT p = { x, y };
            ScreenToClient(wnd, &p);
            float s = Scale(wnd);
            int i = HitAt(p.x / s, p.y / s);
            RECT rc;
            GetClientRect(wnd, &rc);
            if (i < 0 && !PtInRect(&rc, p)) { r->vt = VT_EMPTY; return S_FALSE; }
            r->vt = VT_I4;
            r->lVal = i >= 0 ? i + 1 : CHILDID_SELF;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT v) override {
            const Hit* c = Child(v);
            if (!c) return E_INVALIDARG;
            Hit h = *c; // Activate can change what the next paint builds
            Activate(h);
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
        HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }
    };
    static inline Accessible* accObj = nullptr;

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
        Font fHead(&fam, 9.0f, FontStyleBold, UnitPoint);
        LinearGradientBrush side(RectF(0.0f, 0.0f, SIDE, H), Color(255, 22, 23, 29), Color(255, 16, 17, 21), LinearGradientModeVertical);
        SolidBrush surface(Color(255, 22, 23, 27)), chip(Color(255, 44, 46, 52));
        SolidBrush sideHot(Color(255, 28, 30, 36)), chipHot(Color(255, 69, 71, 76)); // hovered row; chip 12 % lighter
        SolidBrush accentB(ACCENT);
        SolidBrush textWhite(Color(255, 242, 242, 245)), textDim(Color(255, 142, 145, 152)), dangerText(Color(255, 248, 113, 113));
        SolidBrush tabWell(Color(255, 34, 37, 48));
        SolidBrush green(Color(255, 74, 222, 128)), amber(Color(255, 251, 191, 36));
        SolidBrush red(Color(255, 239, 68, 68));
        Pen surfaceEdge(Color(26, 255, 255, 255), 1.0f), divider(Color(255, 48, 49, 55), 1.0f), selRing(Color(150, 255, 255, 255), 1.2f);
        Pen focusEdge(ACCENT, 1.0f), fieldEdge(Color(46, 255, 255, 255), 1.0f), errEdge(Color(255, 248, 113, 113), 1.0f);
        Pen selEdge(Color(160, 124, 156, 255), 1.0f);
        StringFormat center, leftMid, rightMid;
        center.SetAlignment(StringAlignmentCenter);
        leftMid.SetAlignment(StringAlignmentNear);
        rightMid.SetAlignment(StringAlignmentFar);
        for (StringFormat* f : { &center, &leftMid, &rightMid }) {
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
        bool off = false; // the row is switched off by its parent: shown dimmed, no hits
        std::wstring rowName; // the row being painted: its name leads every hit's name
        auto hit = [&](RectF r, int kind, int a, int b = 0, const std::wstring& label = L"", bool on = false) {
            if (off) return;
            // a scrolled-out hit stays in the list (screen readers see it as offscreen); HitAt skips it
            hits.push_back({ r, kind, a, b, label.empty() ? rowName : rowName + L": " + label, on, r.Y + r.Height > TOP && r.Y < BOTTOM });
        };
        auto hot = [&](const RectF& r) { return r.Contains(hx, hy) != FALSE; };

        // Sidebar: icon and name per section; the active one sits on a chip
        static const wchar_t* secIcon[] = { L"", L"", L"", L"", L"", L"\uE706", L"\uE950", L"\uEC4A", L"", L"" };
        static const wchar_t* secName[] = { L"General", L"Píldora", L"Pomodoro", L"Calendario", L"Cuentas", L"Clima", L"Sistema", L"Rendimiento", L"IA", L"Acerca de" };
        fillRound(&chip, 12.0f, ChipY(), 176.0f, 36.0f, 9.0f); // slides from the old section to the new one
        fillRound(&accentB, 12.0f, ChipY() + 9.0f, 3.0f, 18.0f, 1.5f); // accent bar on the chip's left edge
        for (int i = 0; i < SEC_COUNT; ++i) {
            float y = SIDE_TOP + i * SIDE_STEP;
            bool on = i == section;
            if (!on && hot(RectF(12.0f, y, 176.0f, 36.0f))) fillRound(&sideHot, 12.0f, y, 176.0f, 36.0f, 9.0f);
            g.DrawString(secIcon[i], -1, &fIcon, RectF(22.0f, y, 24.0f, 36.0f), &center, on ? &accentB : &textDim);
            g.DrawString(secName[i], -1, &fBody, RectF(50.0f, y, 140.0f, 36.0f), &leftMid, on ? &textWhite : &textDim);
            hits.push_back({ RectF(12.0f, y, 176.0f, 36.0f), HK_SECTION, i, 0, secName[i], on, true });
        }

        g.DrawString(secName[section], -1, &fTitle, RectF(CX, 24.0f, 520.0f, 32.0f), &leftMid, &textWhite);

        // Row helpers. ry is the top of the current row; controls sit in its middle band.
        scrollY = (std::min)(scrollY, (std::max)(0.0f, contentH - (BOTTOM - TOP))); // a section that got shorter stops at its end
        float ry = TOP - scrollY;
        float top0 = ry;
        g.SetClip(RectF(CX - 8.0f, TOP, CR - CX + 16.0f, BOTTOM - TOP));
        float ease = Ease(secAt, SECTION_MS);
        GraphicsState content = g.Save();
        g.TranslateTransform(0.0f, 12.0f * (1.0f - ease)); // the new section rises 12 px into place
        auto row = [&](const std::wstring& name, const std::wstring& desc, float descW = 300.0f) {
            rowName = name;
            g.DrawString(name.c_str(), -1, &fBody, RectF(CX, ry + 9.0f, descW, 20.0f), &leftMid, &textWhite);
            g.DrawString(desc.c_str(), -1, &fDesc, RectF(CX, ry + 29.0f, descW, 16.0f), &leftMid, &textDim);
            g.DrawLine(&divider, CX, ry + ROW - 1.0f, CR, ry + ROW - 1.0f);
        };
        auto next = [&] {
            if (off) { SolidBrush veil(Color(150, 14, 15, 18)); g.FillRectangle(&veil, CX - 8.0f, ry, CR - CX + 16.0f, ROW - 1.0f); } // switched off by its parent: dimmed, not clickable
            ry += ROW;
        };
        auto header = [&](const wchar_t* text) { // group title inside a section: more space above than below
            g.DrawString(text, -1, &fHead, RectF(CX, ry + 14.0f, CR - CX, 18.0f), &leftMid, &textDim);
            ry += 36.0f;
        };
        auto note = [&](const std::wstring& text) { // helper text: not a setting, so no row, no divider
            g.DrawString(text.c_str(), -1, &fDesc, RectF(CX, ry + 4.0f, CR - CX, 16.0f), &leftMid, &textDim);
            ry += 28.0f;
        };
        auto toggle = [&](int id) {
            bool on = *BoolOf(id);
            float t = on ? 1.0f : 0.0f; // 0 off, 1 on; only the last clicked toggle is mid-way
            if (id == toggleId) t = on ? Ease(toggleAt, TOGGLE_MS) : 1.0f - Ease(toggleAt, TOGGLE_MS);
            float x = CR - 40.0f, y = ry + 17.0f;
            SolidBrush track(Mix(Color(255, 44, 46, 52), ACCENT, t));
            fillRound(&track, x, y, 40.0f, 22.0f, 11.0f);
            Pen trackEdge(Color((BYTE)(60 * (1 - t)), 255, 255, 255), 1.0f); // the off track stays visible on the background
            GraphicsPath tp;
            RoundPath(tp, x, y, 40.0f, 22.0f, 11.0f);
            g.DrawPath(&trackEdge, &tp);
            SolidBrush knob(Mix(Color(255, 142, 145, 152), Color(255, 255, 255, 255), t));
            g.FillEllipse(&knob, x + 3.0f + 18.0f * t, y + 3.0f, 16.0f, 16.0f);
            hit(RectF(CX, ry, CR - CX, ROW - 1.0f), HK_BOOL, id, 0, L"", on); // the whole row flips it: the toggle is its only control
        };
        auto stepper = [&](int id) {
            Num n = NumOf(id);
            float x = CR - 124.0f, y = ry + 15.0f;
            fillRound(&chip, x, y, 124.0f, 26.0f, 8.0f);
            g.DrawString(L"", -1, &fIconSmall, RectF(x, y, 30.0f, 26.0f), &center, hot(RectF(x, y, 30.0f, 26.0f)) ? &accentB : &textWhite);
            g.DrawString(L"", -1, &fIconSmall, RectF(x + 94.0f, y, 30.0f, 26.0f), &center, hot(RectF(x + 94.0f, y, 30.0f, 26.0f)) ? &accentB : &textWhite);
            g.DrawString((std::to_wstring(*n.v) + n.unit).c_str(), -1, &fSmall, RectF(x + 30.0f, y, 64.0f, 26.0f), &center, &textWhite);
            hit(RectF(x, y, 30.0f, 26.0f), HK_STEP, id, -1, std::to_wstring(*n.v) + n.unit + L", bajar");
            hit(RectF(x + 94.0f, y, 30.0f, 26.0f), HK_STEP, id, 1, std::to_wstring(*n.v) + n.unit + L", subir");
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
                fillRound(on ? (Brush*)&tabWell : hot(RectF(x, y, w[i], 26.0f)) ? (Brush*)&chipHot : (Brush*)&chip, x, y, w[i], 26.0f, 8.0f);
                if (on) { // the selected chip is marked by its hairline, not by a bright fill
                    GraphicsPath p;
                    RoundPath(p, x, y, w[i], 26.0f, 8.0f);
                    g.DrawPath(&selEdge, &p);
                }
                g.DrawString(c.labels[i].c_str(), -1, &fSmall, RectF(x, y, w[i], 26.0f), &center, on ? &accentB : &textWhite);
                hit(RectF(x, y, w[i], 26.0f), HK_CHIP, id, c.values[i], c.labels[i], on);
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
                hit(RectF(cx - 12.0f, ry + 16.0f, 24.0f, 24.0f), HK_CHIP, id, i, CAL_CATS[i].name, i == cur);
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
                if (shown.empty()) { // the dim text says what an empty field means
                    shown = f == F_MODEL ? AiPreset(st).model : f == F_WCITY ? L"Automática" : L"Sin definir";
                    placeholder = true;
                }
            }
            GraphicsPath p;
            RoundPath(p, x, y, w, 26.0f, 8.0f);
            g.FillPath(&surface, &p);
            const bool bad = fieldErr == f && !on;
            g.DrawPath(on ? &focusEdge : bad ? &errEdge : &fieldEdge, &p);
            g.DrawString(shown.c_str(), -1, &fSmall, RectF(x + 10.0f, y, w - 20.0f, 26.0f), &leftMid, placeholder ? &textDim : &textWhite);
            if (bad) g.DrawString(fieldErrText.c_str(), -1, &fDesc, RectF(x + w - 320.0f, y + 27.0f, 320.0f, 14.0f), &rightMid, &dangerText);
            hit(RectF(x, y, w, 26.0f), HK_FIELD, f);
            fieldOrder.push_back(f);
        };
        auto button = [&](float x, int id, int b, float w, const wchar_t* text, bool danger = false) {
            fillRound(hot(RectF(x, ry + 15.0f, w, 26.0f)) ? (Brush*)&chipHot : (Brush*)&chip, x, ry + 15.0f, w, 26.0f, 8.0f);
            g.DrawString(text, -1, &fSmall, RectF(x, ry + 15.0f, w, 26.0f), &center, danger ? &dangerText : &textWhite);
            hit(RectF(x, ry + 15.0f, w, 26.0f), HK_BUTTON, id, b, text);
        };
        auto account = [&](int src, const wchar_t* name) {
            rowName = name;
            SyncAccount& a = *acc[src];
            SolidBrush& c = a.syncing ? amber : !a.error.empty() ? red : !a.connected ? textDim : green;
            g.FillEllipse(&c, CX + 4.0f, ry + 24.0f, 8.0f, 8.0f);
            g.DrawString(name, -1, &fBody, RectF(CX + 18.0f, ry + 9.0f, CR - 216.0f - (CX + 18.0f), 20.0f), &leftMid, &textWhite);
            g.DrawString(Status(a).c_str(), -1, &fDesc, RectF(CX + 18.0f, ry + 29.0f, CR - 216.0f - (CX + 18.0f), 16.0f), &leftMid, &textDim);
            g.DrawLine(&divider, CX, ry + ROW - 1.0f, CR, ry + ROW - 1.0f);
            if (a.connected) { // connecting is done by the row under the name (URL, ID or link), not here
                button(CR - 208.0f, B_ACC_DISCONNECT, src, 100.0f, L"Desconectar", true);
                button(CR - 100.0f, B_ACC_SYNC, src, 100.0f, L"Sincronizar");
            }
        };

        switch (section) {
        case SEC_GENERAL:
            row(L"Iniciar con Windows", L"Abre Capri Notch al iniciar sesión"); toggle(ID_START); next();
            row(L"Abrir al pasar el ratón", L"Si lo apagas, solo se abre con un clic"); toggle(ID_HOVER_OPEN); next();
            row(L"Retraso al abrir", L"Lo que tarda en abrirse con el puntero encima"); stepper(ID_HOVER_MS); next();
            row(L"Retraso al cerrar", L"Gracia antes de contraerse al salir el puntero"); stepper(ID_LEAVE_MS); next();
            row(L"Esconder al hacer clic al lado", L"Un clic junto a la píldora la recoge"); toggle(ID_TUCK); next();
            row(L"Recoger en pantalla completa", L"Queda en el borde con juegos, vídeos y presentaciones"); toggle(ID_FULLSCREEN); next();
            row(L"Recoger sin uso", L"En una app, tras este tiempo sin tocarla", 200.0f); chips(ID_IDLE_TUCK); next();
            row(L"Monitor", L"Muestra la píldora en la siguiente pantalla"); button(CR - 170.0f, B_MONITOR, 0, 170.0f, L"Mover al otro monitor"); next();
            row(L"Archivos: miniaturas", L"Vista previa junto al nombre en el modo guardar"); toggle(ID_SHELF_THUMBS); next();
            break;
        case SEC_PILL: {
            // Everything the collapsed pill can show, grouped by the tab that feeds it
            header(L"En reposo");
            row(L"Dormir tras", L"Cierra los ojos tras estos minutos sin usar el equipo"); stepper(ID_SLEEP); next();
            row(L"Ojos", L"Mascota que parpadea en reposo; sin ojos, un calendario"); toggle(ID_EYES); next();
            header(L"Con música");
            row(L"Letra en la píldora", L"La línea que suena, si las letras están activas"); toggle(ID_LYRICS); next();
            row(L"Carátula", L"La portada del disco a la izquierda"); toggle(ID_PILL_ART); next();
            row(L"Artista", L"Título · artista en vez de solo el título"); toggle(ID_PILL_ARTIST); next();
            row(L"Ecualizador", L"Barras animadas en la píldora mientras suena música"); toggle(ID_EQUALIZER); next();
            note(L"Letras de LRCLIB: gratis y sin cuenta. Se activan con el botón de la pestaña Música.");
            header(L"Con Pomodoro");
            row(L"Verlo parado", L"El temporizador en la píldora aunque no corra"); toggle(ID_PILL_POMO_IDLE); next();
            row(L"Barra de progreso", L"Línea roja bajo la píldora"); toggle(ID_PILL_POMO_BAR); next();
            row(L"Sesiones", L"Añade las sesiones completadas hoy"); toggle(ID_PILL_POMO_SESSIONS); next();
            header(L"Con Sistema");
            row(L"Vista compacta", L"Las cifras en la píldora, con la pestaña Sistema elegida"); toggle(ID_PILL_SYS); next();
            off = !st.pillSys;
            row(L"CPU", L"Uso y, con Temperaturas, sus grados"); toggle(ID_PILL_SYS_CPU); next();
            off = !st.pillSys || !st.sysTemps;
            row(L"GPU", st.sysTemps ? L"Temperatura de la gráfica" : L"Necesita Temperaturas, en Sistema"); toggle(ID_PILL_SYS_GPU); next();
            off = !st.pillSys;
            row(L"Memoria", L"Porcentaje de RAM en uso"); toggle(ID_PILL_SYS_RAM); next();
            row(L"Disco", L"Espacio libre en C:"); toggle(ID_PILL_SYS_DISK); next();
            row(L"Batería", L"A la derecha: vatios, o el porcentaje si no hay dato"); toggle(ID_PILL_SYS_POWER); next();
            off = false;
            header(L"Avisos");
            row(L"Aviso del cargador", L"Aviso al enchufar o desenchufar"); toggle(ID_CHARGER); next();
            break;
        }
        case SEC_POMODORO:
            row(L"Enfoque", L"Duración de cada sesión de trabajo"); stepper(ID_FOCUS); next();
            row(L"Descanso", L"Duración de cada pausa"); stepper(ID_BREAK); next();
            row(L"Sonido al terminar", L"Pitido al cambiar de fase"); toggle(ID_POMO_SOUND); next();
            break;
        case SEC_CALENDAR:
            row(L"Vista inicial", L"Con la que se abre el calendario al arrancar", 200.0f); chips(ID_CAL_VIEW); next();
            row(L"Aviso por defecto", L"Para los eventos nuevos", 200.0f); chips(ID_ALERT); next();
            row(L"Categoría por defecto", L"Para los eventos nuevos · " + std::wstring(CAL_CATS[(std::max)(0, (std::min)(CAL_CAT_COUNT - 1, st.defaultCategory))].name), 200.0f);
            dots(ID_CATEGORY); next(); // the name in the description: the choice is not color-only
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
            ry += 20.0f; // gap between providers
            account(2, L"Outlook"); next();
            row(L"1. ID de aplicación de Microsoft", L"Id. de aplicación (cliente) de Azure", 240.0f); field(F_MS_ID, CR - 260.0f, 260.0f); next();
            row(L"2. Iniciar sesión", acc[2]->connected ? L"Conectado con tu cuenta de Microsoft" : L"Se abre el navegador para dar permiso", 300.0f);
            if (!acc[2]->connected) button(CR - 100.0f, B_ACC_CONNECT, 2, 100.0f, L"Conectar");
            next();
            ry += 20.0f;
            account(3, L"Calendario por enlace"); next();
            row(L"1. Copia el enlace ICS", L"Outlook: Configuración → Calendario → Calendarios compartidos → Publicar un calendario", 520.0f); next();
            row(L"2. Enlace ICS", acc[3]->connected ? L"Desconecta para cambiarlo" : L"Solo lectura. Se guarda cifrado", 186.0f);
            field(F_ICS, CR - 330.0f, 222.0f);
            if (!acc[3]->connected) button(CR - 100.0f, B_ACC_CONNECT, 3, 100.0f, L"Conectar");
            next();
            header(L"Todas las cuentas");
            row(L"Intervalo de sincronización", L"Cada cuánto se sincronizan las cuentas", 200.0f); chips(ID_SYNC_MIN); next();
            row(L"Guía paso a paso", L"Pasos del script de Google y del ID de Outlook"); button(CR - 110.0f, B_GUIDE, 0, 110.0f, L"Ver guía"); next();
            note(L"Samsung Calendar: guarda tus eventos en Google u Outlook desde el móvil y llegarán aquí.");
            break;
        case SEC_WEATHER:
            row(L"Detectar mi ubicación", L"Ubicación de Windows o, si no, aproximada por tu IP"); toggle(ID_WEATHER_AUTO); next();
            row(L"Ciudad", L"Vacío = automática. Ejemplo: Bogotá", 240.0f); field(F_WCITY, CR - 260.0f, 260.0f); next();
            break;
        case SEC_SYSTEM:
            row(L"Temperaturas", L"CPU y GPU, cuando Windows las ofrece"); toggle(ID_SYS_TEMPS); next();
            row(L"Potencia de la batería", L"Vatios que entran o salen"); toggle(ID_SYS_POWER); next();
            break;
        case SEC_PERF:
            row(L"Animaciones", L"Rebote al abrir, cambio de pestaña y transiciones"); toggle(ID_PERF_ANIM); next();
            row(L"Fotogramas", L"Menos fotogramas, menos uso de CPU"); chips(ID_PERF_FPS); next();
            row(L"Luz ambiental y degradados", L"El brillo de color del panel y los fondos degradados"); toggle(ID_PERF_GLOW); next();
            off = !st.perfGlow;
            row(L"Color de la luz", L"Sin música. Con música manda la carátula", 200.0f); chips(ID_GLOW_SOURCE); next();
            if (g_settings.glowSource == 3) {
                row(L"Colores propios", L"Izquierda y derecha del panel");
                button(CR - 206.0f, B_GLOW_COLOR, 0, 100.0f, L"Izquierda");
                button(CR - 100.0f, B_GLOW_COLOR, 1, 100.0f, L"Derecha");
                for (int i = 0; i < 2; ++i) { // a dot of the current color inside each button
                    int v = i ? g_settings.glowRight : g_settings.glowLeft;
                    SolidBrush c(Color(255, (BYTE)(v >> 16), (BYTE)(v >> 8), (BYTE)v));
                    g.FillEllipse(&c, CR - (i ? 100.0f : 206.0f) + 8.0f, ry + 23.0f, 10.0f, 10.0f);
                }
                next();
            }
            off = false;
            row(L"Lectura de sensores", L"Cada cuánto se leen CPU, GPU y batería en Sistema"); chips(ID_PERF_SENSOR); next();
            row(L"Ahorro con batería", L"Sin animaciones ni luces y a 30 fps al desenchufar"); toggle(ID_PERF_SAVER); next();
            break;
        case SEC_AI:
            row(L"Proveedor", L"Dónde se envían tus preguntas", 150.0f); chips(ID_AI_PROVIDER); next();
            row(L"Clave de API", L"Cifrada con tu cuenta de Windows", 230.0f);
            if (hooks.hasKey()) button(CR - 70.0f, B_CLEAR_KEY, 0, 70.0f, L"Borrar", true);
            field(F_KEY, CR - 248.0f, 170.0f);
            next();
            row(L"Modelo", L"Vacío = el que usa el proveedor", 240.0f); field(F_MODEL, CR - 260.0f, 260.0f); next();
            if (st.aiProvider == 3) {
                row(L"Servidor", L"Host, sin https://", 240.0f); field(F_HOST, CR - 260.0f, 260.0f); next();
                row(L"Ruta", L"Por ejemplo /v1/chat/completions", 240.0f); field(F_PATH, CR - 260.0f, 260.0f); next();
                row(L"Puerto", L"443 para https, 11434 para Ollama", 240.0f); field(F_PORT, CR - 100.0f, 80.0f); next();
                row(L"HTTPS", L"Conexión cifrada"); toggle(ID_AI_HTTPS); next();
            }
            for (int m = 0; m < CACTUS_CAMERA; ++m) {
                static const wchar_t* titles[] = { L"Whistle (voz a texto)", L"Mirage (ve tu pantalla)" };
                static const wchar_t* sizes[] = { L"17", L"22" };
                const bool has = CactusInstalled(m), busy = g_cactusDownload[m] == 1;
                const std::wstring mb = sizes[m];
                row(titles[m], busy ? L"Descargando de Hugging Face… (" + mb + L" MB)" : has ? L"Instalado · modelo local de Cactus, " + mb + L" MB en disco"
                                   : g_cactusDownload[m] == 2 ? std::wstring(L"No se pudo descargar. Revisa tu conexión") : L"No instalado · " + mb + L" MB, se ejecuta en tu PC");
                button(CR - 110.0f, B_WHISTLE, m, 110.0f, busy ? L"Descargando" : has ? L"Borrar" : L"Descargar", has);
                next();
            }
            row(L"Motor de manos", L"El que usa la cámara del chip Cámara", 150.0f); chips(ID_HAND_ENGINE); next();
            for (int e = 0; e < HAND_ENGINE_COUNT; ++e) {
                static const wchar_t* titles[] = { L"MediaPipe · rápido", L"RTMPose · más preciso" };
                static const wchar_t* sizes[] = { L"8 MB, CPU", L"59 MB, usa la GPU si hay" };
                const bool has = HandInstalled(e), busy = g_handDownload[e] == 1;
                row(titles[e], busy ? std::wstring(L"Descargando…") : has ? L"Instalado · " + std::wstring(sizes[e])
                                   : g_handDownload[e] == 2 ? std::wstring(L"No se pudo descargar. Revisa tu conexión") : L"No instalado · " + std::wstring(sizes[e]));
                button(CR - 110.0f, B_HAND, e, 110.0f, busy ? L"Descargando" : has ? L"Borrar" : L"Descargar", has);
                next();
            }
            break;
        case SEC_ABOUT: {
            PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
            GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
            row(L"Capri Notch", L"Versión " + std::wstring(APP_VERSION) + L" · " + std::to_wstring(pmc.WorkingSetSize >> 20) + L" MB en uso");
            next();
            row(L"Carpeta de datos", L"Ajustes, eventos, tareas y tokens"); button(CR - 110.0f, B_DATA, 0, 110.0f, L"Abrir carpeta"); next();
            row(L"Salir de Capri Notch", L"Cierra Capri Notch hasta que lo vuelvas a abrir"); button(CR - 90.0f, B_QUIT, 0, 90.0f, L"Salir", true); next();
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
        if (contentH > BOTTOM - TOP) { // scroll thumb: its size and place show how much is above and below
            float view = BOTTOM - TOP, maxS = contentH - view;
            float th = (std::max)(28.0f, view * view / contentH), ty = TOP + (maxS > 0 ? scrollY / maxS : 0) * (view - th);
            SolidBrush thumb(Color(60, 255, 255, 255));
            fillRound(&thumb, CR + 12.0f, ty, 3.0f, th, 1.5f);
        }

        Graphics screen(dc);
        screen.DrawImage(&bmp, 0, 0, bmp.GetWidth(), bmp.GetHeight());
    }
};
