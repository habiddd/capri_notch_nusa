// g++ -std=c++17 -Wall tests/pill_test.cpp -o %TEMP%\pill_test.exe -static -static-libgcc -static-libstdc++ -lgdiplus -lgdi32 -lwinhttp -lwinmm -luser32 -lshell32 -lruntimeobject -lole32 -loleaut32 -lshcore -lws2_32 -lbcrypt -lcrypt32 -ldwmapi -lpsapi -lpdh -lpowrprof && %TEMP%\pill_test.exe
#define _WIN32_WINNT 0x0A00 // same prelude as main.cpp: ui.h is not self-contained
#include <windows.h>
#include <roapi.h>
#include <propidl.h>
#include <gdiplus.h>
#include <cstdio>
#include "../src/types.h"
#include "../src/pomodoro.h"
#include "../src/todo.h"
#include "../src/events.h"
#include "../src/media.h"
#include "../src/ai_client.h"
#include "../src/audio_wake.h"
#include <cassert>
#include "../src/settings.h"
#include "../src/tabs.h"
#include "../src/ui.h"

int main() {
    using V = std::vector<std::pair<int, std::wstring>>;
    V none, ev = { { 600, L"Pasado" }, { 930, L"Dentista" } };
    assert(CapriUI::IdleLine(ev, 700, 5, L"fecha") == L"15:30 · Dentista"); // future event wins, past one skipped
    assert(CapriUI::IdleLine(ev, 930, 0, L"fecha") == L"15:30 · Dentista"); // starting now still counts
    assert(CapriUI::IdleLine(ev, 931, 1, L"fecha") == L"1 pendiente");
    assert(CapriUI::IdleLine(none, 0, 3, L"fecha") == L"3 pendientes");
    assert(CapriUI::IdleLine(none, 0, 0, L"fecha") == L"fecha");
    return 0;
}
