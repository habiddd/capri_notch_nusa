#include <windows.h>
#include <roapi.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    MessageBoxW(NULL, L"Step 1: Before RoInitialize", L"Debug", MB_OK);
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    wchar_t buf[128];
    wsprintfW(buf, L"Step 2: RoInitialize = 0x%08X", (unsigned)hr);
    MessageBoxW(NULL, buf, L"Debug", MB_OK);
    return 0;
}

