// g++ -std=c++17 -Wall tests/settings_test.cpp -o %TEMP%\settings_test.exe -static -lgdiplus -lgdi32 -lwinhttp -lwinmm -luser32 -lshell32 -lruntimeobject -lole32 -loleaut32 -lshcore -lws2_32 -lbcrypt -lcrypt32 -ldwmapi -lpsapi
// Uses only %TEMP% files. Never touches the Run key or the real capri_settings.ini / key files.
#define _WIN32_WINNT 0x0A00 // same prelude as main.cpp: the headers are not self-contained
#include <windows.h>
#include <cassert>
#include <cstdio>
#include "../src/settings.h"
#include "../src/pomodoro.h"

static std::wstring TempFile(const wchar_t* name) {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    return std::wstring(t) + name;
}

int main() {
    std::wstring ini = TempFile(L"capri_settings_test.ini");
    DeleteFileW(ini.c_str());

    // No file: the defaults are today's behavior, and LoadSettings says there was nothing to read
    Settings s;
    assert(!LoadSettings(ini, s));
    assert(!s.startWithWindows && s.hoverOpen && s.hoverDelay == 180 && s.leaveDelay == 300 && s.tuckOnClick && !s.hideFullscreen);
    assert(s.eyes && s.sleepMin == 2 && s.equalizer && s.chargerNotice && s.pillLyrics);
    assert(s.focusMin == 25 && s.breakMin == 5 && s.pomoSound && s.calDefaultView == 0 && s.defaultAlert == 10);
    assert(s.defaultCategory == 0 && s.reminderSound && s.syncMinutes == 5);
    assert(s.aiProvider == 0 && s.aiHost.empty() && s.aiPath == L"/v1/chat/completions" && s.aiModel.empty() && s.aiPort == 443 && s.aiHttps);

    // Round trip of every field, with an accent in a text field
    Settings w;
    w.startWithWindows = true; w.hoverOpen = false; w.hoverDelay = 450; w.leaveDelay = 1200; w.tuckOnClick = false; w.hideFullscreen = true;
    w.eyes = false; w.sleepMin = 7; w.equalizer = false; w.chargerNotice = false; w.pillLyrics = false;
    w.focusMin = 50; w.breakMin = 12; w.pomoSound = false;
    w.calDefaultView = 2; w.defaultAlert = 30; w.defaultCategory = 4; w.reminderSound = false; w.syncMinutes = 15;
    w.aiProvider = 3; w.aiHost = L"ia.ejemplo.es"; w.aiPath = L"/api/chat"; w.aiModel = L"modelo ñandú"; w.aiPort = 8443; w.aiHttps = false;
    assert(SaveSettings(ini, w));
    Settings r;
    assert(LoadSettings(ini, r));
    assert(r.startWithWindows && !r.hoverOpen && r.hoverDelay == 450 && r.leaveDelay == 1200 && !r.tuckOnClick && r.hideFullscreen);
    assert(!r.eyes && r.sleepMin == 7 && !r.equalizer && !r.chargerNotice && !r.pillLyrics);
    assert(r.focusMin == 50 && r.breakMin == 12 && !r.pomoSound);
    assert(r.calDefaultView == 2 && r.defaultAlert == 30 && r.defaultCategory == 4 && !r.reminderSound && r.syncMinutes == 15);
    assert(r.aiProvider == 3 && r.aiHost == L"ia.ejemplo.es" && r.aiPath == L"/api/chat" && r.aiModel == L"modelo ñandú");
    assert(r.aiPort == 8443 && !r.aiHttps);

    // Out-of-range, not in the list, garbage, unknown keys: the defaults, and the valid lines around them still count
    std::string bad = "hoverDelay=5000\r\nleaveDelay=50\r\nfocusMin=abc\r\nbreakMin=\r\nsyncMinutes=7\r\ndefaultAlert=5\r\n"
                      "aiProvider=9\r\naiPort=0\r\nunknownKey=xyz\r\nsleepMin=11\r\nhoverOpen=maybe\r\n  eyes = 0  \r\n";
    assert(WriteBytesFile(ini, bad));
    Settings b;
    assert(LoadSettings(ini, b));
    assert(b.hoverDelay == 180 && b.leaveDelay == 300 && b.breakMin == 5 && b.defaultAlert == 10 && b.aiProvider == 0);
    assert(b.aiPort == 443 && b.sleepMin == 2 && b.hoverOpen && b.syncMinutes == 5 && b.focusMin == 25);
    assert(!b.eyes); // spaces around the value are trimmed

    // AI presets: the endpoint of each provider
    Settings p;
    AiEndpoint e = AiPreset(p);
    assert(e.host == L"api.openai.com" && e.path == L"/v1/chat/completions" && e.port == 443 && e.https && e.model == L"gpt-4o-mini" && e.needKey);
    p.aiProvider = 1;
    e = AiPreset(p);
    assert(e.host == L"api.deepseek.com" && e.path == L"/chat/completions" && e.port == 443 && e.https && e.model == L"deepseek-chat" && e.needKey);
    p.aiProvider = 2;
    e = AiPreset(p);
    assert(e.host == L"localhost" && e.path == L"/v1/chat/completions" && e.port == 11434 && !e.https && e.model == L"llama3.2" && !e.needKey);
    p.aiProvider = 3; p.aiHost = L"mi.host"; p.aiPath = L"/x"; p.aiPort = 8080; p.aiHttps = false; p.aiModel = L"m";
    e = AiPreset(p);
    assert(e.host == L"mi.host" && e.path == L"/x" && e.port == 8080 && !e.https && e.model == L"m");
    p.aiProvider = 0; p.aiModel = L"gpt-4o";
    assert(AiPreset(p).model == L"gpt-4o"); // a chosen model wins over the preset's

    // The Run value: the exe in quotes (the test checks the shape; the Run key itself is not touched)
    std::wstring cmd = RunCommandLine();
    assert(cmd.size() > 2 && cmd.front() == L'"' && cmd.back() == L'"' && cmd.find(L".exe") != std::wstring::npos);

    // Pomodoro with 1-minute phases: the 61st tick flips (the first 60 count the minute down to 0)
    g_settings.focusMin = 1; g_settings.breakMin = 1; g_settings.pomoSound = false;
    PomodoroManager pomo;
    pomo.Reset();
    assert(pomo.state.totalSeconds == 60 && pomo.state.remainingSeconds == 60);
    pomo.Toggle();
    for (int i = 0; i < 60; ++i) pomo.Tick();
    assert(!pomo.state.isBreak && pomo.state.remainingSeconds == 0);
    pomo.Tick();
    assert(pomo.state.isBreak && pomo.state.totalSeconds == 60 && pomo.state.sessionsCompleted == 1);

    // A paused session keeps its place when the durations do not change, and restarts at a new one when they do
    pomo.Toggle();
    pomo.Reset();
    for (int i = 0; i < 10; ++i) pomo.Tick(); // not running: no change
    pomo.Toggle();
    for (int i = 0; i < 10; ++i) pomo.Tick();
    pomo.Toggle();
    pomo.Retime();
    assert(pomo.state.remainingSeconds == 50);
    g_settings.focusMin = 2;
    pomo.Retime();
    assert(pomo.state.totalSeconds == 120 && pomo.state.remainingSeconds == 120);

    // capri_sync.ini writer: replaces one key, keeps comments, appends a missing key
    std::wstring t = L"; comentario\r\nfoo=1\r\nmicrosoft_client_id=\r\n";
    std::wstring o = SetIniValue(t, L"microsoft_client_id", L"abc");
    assert(ParseSyncConfig(o).microsoftId == L"abc" && o.find(L"; comentario") != std::wstring::npos);
    std::wstring o2 = SetIniValue(o, L"mi_clave", L"ms1");
    assert(IniValue(o2, L"mi_clave") == L"ms1" && ParseSyncConfig(o2).microsoftId == L"abc");
    assert(IniValue(SetIniValue(L"", L"microsoft_client_id", L"s"), L"microsoft_client_id") == L"s");

    DeleteFileW(ini.c_str());
    std::printf("settings_test ok\n");
    return 0;
}
