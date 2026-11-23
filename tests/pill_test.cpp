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
#include <cassert>
#include "../src/settings.h"
#include "../src/tabs.h"
#include "../src/ui.h"

int main() {
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR tok;
    Gdiplus::GdiplusStartup(&tok, &gsi, NULL);
    using V = std::vector<std::pair<int, std::wstring>>;
    V none, ev = { { 600, L"Pasado" }, { 930, L"Dentista" } };
    assert(CapriUI::IdleLine(ev, 700, 5, L"fecha") == L"15:30 · Dentista"); // future event wins, past one skipped
    assert(CapriUI::IdleLine(ev, 930, 0, L"fecha") == L"15:30 · Dentista"); // starting now still counts
    assert(CapriUI::IdleLine(ev, 931, 1, L"fecha") == L"1 pendiente");
    assert(CapriUI::IdleLine(none, 0, 3, L"fecha") == L"3 pendientes");
    assert(CapriUI::IdleLine(none, 0, 0, L"fecha") == L"fecha");

    // The gesture bump's pop: starts at 0, lands on 1, overshoots a little in between
    {
        float mx = 0;
        for (float p = 0; p <= 1.0f; p += 0.01f) mx = (std::max)(mx, CapriUI::PopScale(p));
        assert(CapriUI::PopScale(0) == 0 && CapriUI::PopScale(1) == 1 && mx > 1 && mx < 1.15f);
    }

    // The glow's colors: each half's average, lifted so its strongest channel is 210
    {
        Gdiplus::Bitmap two(2, 1, PixelFormat32bppARGB);
        two.SetPixel(0, 0, Gdiplus::Color(255, 100, 0, 0));
        two.SetPixel(1, 0, Gdiplus::Color(255, 0, 0, 50));
        Gdiplus::Color t[2];
        assert(TwoTints(&two, t));
        assert(t[0].GetR() == 210 && t[0].GetG() == 0 && t[0].GetB() == 0 && t[1].GetB() == 210 && t[1].GetR() == 0);
    }
    return 0;
}
