// g++ -std=c++17 -Wall tests/whistle_test.cpp -o %TEMP%\whistle_test.exe -static -lwinhttp -lwinmm -lbcrypt -lshell32 -lole32 -lgdi32 && %TEMP%\whistle_test.exe
#define _WIN32_WINNT 0x0A00 // same prelude as main.cpp: BCryptHash needs Windows 10 headers
#include <windows.h>
#include <cassert>
#include <cstdio>
#include "../src/whistle.h"

int main() {
    // The checksum that guards the runtime download
    assert(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // The worker's answers: text is joined with spaces, pending is replaced, junk is ignored
    WhistleEngine w;
    w.OnMessage(WHISTLE_READY, "");
    assert(w.ready && w.text.empty());
    w.OnMessage(WHISTLE_TEXT, "{\"text\":\"\",\"pending\":\"hola que\",\"language\":\"es\"}");
    assert(w.text.empty() && w.pending == L"hola que");
    w.OnMessage(WHISTLE_TEXT, "{\"text\":\"hola qu\\u00e9 tal\",\"pending\":\"\"}");
    w.OnMessage(WHISTLE_TEXT, "{\"text\":\"todo bien\",\"pending\":\"y\"}");
    assert(w.text == L"hola qué tal todo bien" && w.pending == L"y");
    w.OnMessage(WHISTLE_TEXT, "not json");
    assert(w.text == L"hola qué tal todo bien");
    w.OnMessage(WHISTLE_TEXT, "{\"text\":\"" + std::string(5000, 'a') + "\",\"pending\":\"\"}");
    assert(w.text.size() == 4000); // only the tail is kept
    w.OnMessage(WHISTLE_ERROR, "No se pudo abrir el micr\xC3\xB3" "fono.");
    assert(w.error == L"No se pudo abrir el micrófono.");
    assert(!w.Running()); // nothing was started

    // Mirage: each look replaces the card's line with the scene and the text it read
    WhistleEngine m;
    m.model = CACTUS_MIRAGE;
    m.OnMessage(WHISTLE_TEXT, "{\"scene\":{\"setting\":\"pantalla de computadora\"},\"text\":[{\"string\":\"Claude Code\",\"bbox\":[1,2,3,4],\"score\":0.5},{\"string\":\"main.cpp\"}]}");
    assert(m.text == L"pantalla de computadora · Claude Code · main.cpp");
    m.OnMessage(WHISTLE_TEXT, "{\"scene\":{},\"text\":[]}");
    assert(m.text.empty());
    assert(CactusModel(CACTUS_MIRAGE).find(L"\\models\\Mirage.cact") != std::wstring::npos);

    puts("whistle_test ok");
    return 0;
}
