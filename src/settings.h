#pragma once
#include <windows.h>
#include <algorithm>
#include <initializer_list>
#include <string>
#include "oauth.h"

// Every setting the user can change. The defaults are today's behavior, so without capri_settings.ini nothing changes.
struct Settings {
    // General
    bool startWithWindows = false; // the Run key is the truth: the app reads it at start
    bool hoverOpen = true;         // false: only a click expands the notch
    int hoverDelay = 180;          // ms
    int leaveDelay = 300;          // ms
    bool tuckOnClick = true;
    bool hideFullscreen = false;   // on: the pill tucks to its edge over full-screen apps
    int idleTuckSec = 0;           // 0 (off), 10, 30, 60 or 300: unused this long over an app, the pill tucks to its edge
    // Pill
    bool eyes = true;
    int sleepMin = 2;
    bool equalizer = true, chargerNotice = true, pillLyrics = true;
    // Pomodoro
    int focusMin = 25, breakMin = 5;
    bool pomoSound = true;
    // Calendar
    int calDefaultView = 0;   // CapriUI::CalView
    int defaultAlert = 10;    // -1, 0, 10, 30, 60
    int defaultMoreAlerts = 0; // the other default alerts as bits (see AlertMask in events.h)
    int defaultCategory = 0;  // index into CAL_CATS
    bool reminderSound = true;
    bool tasksInCalendar = true; // dated tasks show in the calendar, calendar tasks in Tareas
    int syncMinutes = 5;      // 5, 15, 30
    // AI: presets fill the endpoint; the custom fields are used only by provider 3
    int aiProvider = 0;       // 0 OpenAI, 1 DeepSeek, 2 Ollama, 3 custom
    std::wstring aiHost, aiPath = L"/v1/chat/completions", aiModel;
    int aiPort = 443;
    bool aiHttps = true;
    int handEngine = 0;       // the camera's hand engine: 0 MediaPipe (fast, CPU), 1 RTMPose (precise, GPU)
    // Sistema: CPU and GPU temperatures, and battery watts, when Windows offers them
    bool sysTemps = true, sysPower = true;
    bool shelfThumbs = true; // Archivos, save mode: a thumbnail beside each name
    // What each tab's collapsed view shows (the pill only ever grows sideways)
    bool pillArt = true, pillArtist = false;                               // music: cover, "title · artist"
    bool pillPomoIdle = true, pillPomoBar = true, pillPomoSessions = false; // pomodoro: shown while stopped, progress line, session count
    bool pillSys = true, pillSysCpu = true, pillSysGpu = true, pillSysRam = true, pillSysDisk = false, pillSysPower = true;
    // Clima: the place is where the user is (Windows location, else IP) unless a city is typed (empty = automatic)
    bool weatherAuto = true;
    std::wstring weatherCityName;
    // Performance
    bool perfAnimations = true;    // resize spring, tab/open/lyric transitions, settings-window transitions
    bool perfGlow = true;          // ambient light and gradients
    int glowSource = 0;                             // the glow's colors while no cover shows: 0 classic blue and gold, 1 the wallpaper, 2 Windows' accent, 3 the two below
    int glowLeft = 0x2856C8, glowRight = 0xBE9628;  // 0xRRGGBB
    int perfFps = 60;              // 60 or 30: rate of the animation timer
    int perfSensorSec = 1;         // 1, 2 or 5: seconds between Sistema readings
    bool perfBatterySaver = false; // on battery: no animations, no glow, at most 30 fps
};

inline Settings g_settings;
inline constexpr const wchar_t* APP_VERSION = L"1.0";

inline bool g_onBattery = false; // kept up to date by the app (main.cpp) from the power status
struct Perf { bool anim, glow; int fps; };
// The performance settings as they apply right now: the battery saver, when on and unplugged, overrides the three of them
inline Perf EffectivePerf(const Settings& s = g_settings, bool onBattery = g_onBattery) {
    bool save = s.perfBatterySaver && onBattery;
    return { s.perfAnimations && !save, s.perfGlow && !save, save ? (std::min)(s.perfFps, 30) : s.perfFps };
}

struct AiEndpoint { std::wstring host, path, model; int port; bool https; bool needKey; };

// Where the requests go for the chosen provider. An empty model means the provider's own default.
inline AiEndpoint AiPreset(const Settings& s) {
    auto model = [&](const wchar_t* def) { return s.aiModel.empty() ? std::wstring(def) : s.aiModel; };
    switch (s.aiProvider) {
    case 1: return { L"api.deepseek.com", L"/chat/completions", model(L"deepseek-chat"), 443, true, true };
    case 2: return { L"localhost", L"/v1/chat/completions", model(L"llama3.2"), 11434, false, false };
    case 3: return { s.aiHost, s.aiPath, model(L"gpt-4o-mini"), s.aiPort, s.aiHttps, false };
    default: return { L"api.openai.com", L"/v1/chat/completions", model(L"gpt-4o-mini"), 443, true, true };
    }
}

// The collapsed-view switches with their ini keys: one list, so load and save cannot drift apart
inline std::vector<std::pair<const wchar_t*, bool*>> PillFlags(Settings& s) {
    return { { L"pillArt", &s.pillArt }, { L"pillArtist", &s.pillArtist }, { L"pillPomoIdle", &s.pillPomoIdle },
             { L"pillPomoBar", &s.pillPomoBar }, { L"pillPomoSessions", &s.pillPomoSessions }, { L"pillSys", &s.pillSys },
             { L"pillSysCpu", &s.pillSysCpu }, { L"pillSysGpu", &s.pillSysGpu }, { L"pillSysRam", &s.pillSysRam },
             { L"pillSysDisk", &s.pillSysDisk }, { L"pillSysPower", &s.pillSysPower } };
}

inline std::wstring SettingsIniPath() { return AppDataFile(L"capri_settings.ini"); }

// Whole non-negative or negative integer, or false
inline bool ParseIntW(const std::wstring& s, int& out) {
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    if (*end) return false;
    out = (int)v;
    return true;
}

// Missing file: false and the defaults stay. Values out of range or not in the list are ignored; unknown keys too.
inline bool LoadSettings(const std::wstring& path, Settings& s) {
    std::wstring text;
    if (!ReadUtf8File(path, text)) return false;
    auto num = [&](const wchar_t* k, int& v, int lo, int hi) { int x; if (ParseIntW(IniValue(text, k), x) && x >= lo && x <= hi) v = x; };
    auto numIn = [&](const wchar_t* k, int& v, std::initializer_list<int> ok) {
        int x;
        if (ParseIntW(IniValue(text, k), x)) for (int o : ok) if (x == o) v = x;
    };
    auto flag = [&](const wchar_t* k, bool& v) { std::wstring f = IniValue(text, k); if (f == L"1") v = true; else if (f == L"0") v = false; };
    auto str = [&](const wchar_t* k, std::wstring& v) { std::wstring x = IniValue(text, k); if (!x.empty()) v = x; };
    flag(L"startWithWindows", s.startWithWindows);
    flag(L"hoverOpen", s.hoverOpen);
    num(L"hoverDelay", s.hoverDelay, 0, 1000);
    num(L"leaveDelay", s.leaveDelay, 100, 2000);
    flag(L"tuckOnClick", s.tuckOnClick);
    flag(L"hideFullscreen", s.hideFullscreen);
    numIn(L"idleTuckSec", s.idleTuckSec, { 0, 10, 30, 60, 300 });
    flag(L"eyes", s.eyes);
    num(L"sleepMin", s.sleepMin, 1, 10);
    flag(L"equalizer", s.equalizer);
    flag(L"chargerNotice", s.chargerNotice);
    flag(L"pillLyrics", s.pillLyrics);
    num(L"focusMin", s.focusMin, 5, 90);
    num(L"breakMin", s.breakMin, 1, 30);
    flag(L"pomoSound", s.pomoSound);
    num(L"calDefaultView", s.calDefaultView, 0, 3);
    numIn(L"defaultAlert", s.defaultAlert, { -1, 0, 10, 30, 60 });
    num(L"defaultMoreAlerts", s.defaultMoreAlerts, 0, 15);
    num(L"defaultCategory", s.defaultCategory, 0, 5);
    flag(L"reminderSound", s.reminderSound);
    flag(L"tasksInCalendar", s.tasksInCalendar);
    numIn(L"syncMinutes", s.syncMinutes, { 5, 15, 30 });
    num(L"aiProvider", s.aiProvider, 0, 3);
    num(L"handEngine", s.handEngine, 0, 1);
    str(L"aiHost", s.aiHost);
    str(L"aiPath", s.aiPath);
    str(L"aiModel", s.aiModel);
    num(L"aiPort", s.aiPort, 1, 65535);
    flag(L"aiHttps", s.aiHttps);
    flag(L"sysTemps", s.sysTemps);
    flag(L"sysPower", s.sysPower);
    flag(L"shelfThumbs", s.shelfThumbs);
    for (auto& f : PillFlags(s)) flag(f.first, *f.second);
    flag(L"weatherAuto", s.weatherAuto);
    str(L"weatherCityName", s.weatherCityName);
    flag(L"perfAnimations", s.perfAnimations);
    flag(L"perfGlow", s.perfGlow);
    num(L"glowSource", s.glowSource, 0, 3);
    num(L"glowLeft", s.glowLeft, 0, 0xFFFFFF);
    num(L"glowRight", s.glowRight, 0, 0xFFFFFF);
    numIn(L"perfFps", s.perfFps, { 30, 60 });
    numIn(L"perfSensorSec", s.perfSensorSec, { 1, 2, 5 });
    flag(L"perfBatterySaver", s.perfBatterySaver);
    return true;
}

inline bool SaveSettings(const std::wstring& path, const Settings& s) {
    auto flag = [](bool b) { return std::wstring(b ? L"1" : L"0"); };
    std::wstring t = L"; Capri Notch: ajustes. Se cambian desde la ventana de configuración; no hace falta editarlo.\r\n";
    auto line = [&](const wchar_t* k, const std::wstring& v) { t += std::wstring(k) + L"=" + v + L"\r\n"; };
    line(L"startWithWindows", flag(s.startWithWindows));
    line(L"hoverOpen", flag(s.hoverOpen));
    line(L"hoverDelay", std::to_wstring(s.hoverDelay));
    line(L"leaveDelay", std::to_wstring(s.leaveDelay));
    line(L"tuckOnClick", flag(s.tuckOnClick));
    line(L"hideFullscreen", flag(s.hideFullscreen));
    line(L"idleTuckSec", std::to_wstring(s.idleTuckSec));
    line(L"eyes", flag(s.eyes));
    line(L"sleepMin", std::to_wstring(s.sleepMin));
    line(L"equalizer", flag(s.equalizer));
    line(L"chargerNotice", flag(s.chargerNotice));
    line(L"pillLyrics", flag(s.pillLyrics));
    line(L"focusMin", std::to_wstring(s.focusMin));
    line(L"breakMin", std::to_wstring(s.breakMin));
    line(L"pomoSound", flag(s.pomoSound));
    line(L"calDefaultView", std::to_wstring(s.calDefaultView));
    line(L"defaultAlert", std::to_wstring(s.defaultAlert));
    line(L"defaultMoreAlerts", std::to_wstring(s.defaultMoreAlerts));
    line(L"defaultCategory", std::to_wstring(s.defaultCategory));
    line(L"reminderSound", flag(s.reminderSound));
    line(L"tasksInCalendar", flag(s.tasksInCalendar));
    line(L"syncMinutes", std::to_wstring(s.syncMinutes));
    line(L"aiProvider", std::to_wstring(s.aiProvider));
    line(L"handEngine", std::to_wstring(s.handEngine));
    line(L"aiHost", s.aiHost);
    line(L"aiPath", s.aiPath);
    line(L"aiModel", s.aiModel);
    line(L"aiPort", std::to_wstring(s.aiPort));
    line(L"aiHttps", flag(s.aiHttps));
    line(L"sysTemps", flag(s.sysTemps));
    line(L"sysPower", flag(s.sysPower));
    line(L"shelfThumbs", flag(s.shelfThumbs));
    for (auto& f : PillFlags(const_cast<Settings&>(s))) line(f.first, flag(*f.second));
    line(L"weatherAuto", flag(s.weatherAuto));
    line(L"weatherCityName", s.weatherCityName);
    line(L"perfAnimations", flag(s.perfAnimations));
    line(L"perfGlow", flag(s.perfGlow));
    line(L"glowSource", std::to_wstring(s.glowSource));
    line(L"glowLeft", std::to_wstring(s.glowLeft));
    line(L"glowRight", std::to_wstring(s.glowRight));
    line(L"perfFps", std::to_wstring(s.perfFps));
    line(L"perfSensorSec", std::to_wstring(s.perfSensorSec));
    line(L"perfBatterySaver", flag(s.perfBatterySaver));
    return WriteBytesFile(path, Utf8(t));
}

// The command the Run key holds: the exe in quotes, so a path with spaces starts
inline std::wstring RunCommandLine() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    return L"\"" + std::wstring(exe) + L"\"";
}

inline constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr const wchar_t* RUN_APPROVED_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
inline constexpr const wchar_t* RUN_VALUE = L"CapriNotch";

inline bool RunAtLogin() {
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE, RRF_RT_REG_SZ, NULL, NULL, NULL) != ERROR_SUCCESS) return false;
    // Disabling the app in Task Manager keeps the Run value and sets an odd first byte here
    BYTE approved[12] = {};
    DWORD n = sizeof(approved);
    bool disabled = RegGetValueW(HKEY_CURRENT_USER, RUN_APPROVED_KEY, RUN_VALUE, RRF_RT_REG_BINARY, NULL, approved, &n) == ERROR_SUCCESS && (approved[0] & 1);
    return !disabled;
}

inline void SetRunAtLogin(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = RunCommandLine();
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_APPROVED_KEY, RUN_VALUE); // turning it on here undoes a Task Manager "disabled"
    } else {
        RegDeleteValueW(k, RUN_VALUE);
    }
    RegCloseKey(k);
}
