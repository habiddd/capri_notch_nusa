#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "types.h"
#include "json.h"

// Whistle (Cactus Compute): speech to text that runs on the CPU, with no network and no key.
// The model never loads into the notch. A worker process (this exe started with --whistle) loads Cactus's runtime DLL and
// the 16.9 MB model, listens to the microphone and sends what it hears to the notch. The runtime has no call to unload a
// model, so ending the worker is how its memory is given back: all of it, at once.

inline std::wstring WhistleDir() { return AppDataFile(L"CapriNotch"); }
inline std::wstring WhistleDll() { return WhistleDir() + L"\\libcactus.dll"; }
// The two Cactus models that share the runtime, by index
enum { CACTUS_WHISTLE, CACTUS_MIRAGE, CACTUS_COUNT };
inline constexpr const wchar_t* CACTUS_NAME[] = { L"Whistle", L"Mirage" };
inline constexpr const wchar_t* CACTUS_MODEL_PATH[] = { L"/Cactus-Compute/whistle/resolve/main/whistle.cact", L"/Cactus-Compute/mirage/resolve/main/mirage.cact" };
inline std::wstring CactusModel(int m) { return WhistleDir() + L"\\models\\" + CACTUS_NAME[m] + L".cact"; }
inline bool CactusInstalled(int m) {
    return GetFileAttributesW(WhistleDll().c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(CactusModel(m).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Lower-case hex SHA-256, "" on failure
inline std::string Sha256Hex(const std::string& bytes) {
    BCRYPT_ALG_HANDLE alg = NULL;
    UCHAR hash[32];
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) return "";
    NTSTATUS st = BCryptHash(alg, NULL, 0, (PUCHAR)bytes.data(), (ULONG)bytes.size(), hash, sizeof hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(st)) return "";
    std::string hex;
    char two[3];
    for (UCHAR b : hash) { snprintf(two, sizeof two, "%02x", b); hex += two; }
    return hex;
}

// The runtime is code, so it is checked when it is downloaded: only the exact official build is accepted, Cactus engine
// 4.1.1's Windows x64 wheel (a zip that holds libcactus.dll), against the SHA-256 its release manifest publishes. Models are data; cactus_load rejects a bad one.
inline constexpr const wchar_t* WHISTLE_HOST = L"huggingface.co";
inline constexpr const wchar_t* WHISTLE_WHL_PATH = L"/Cactus-Compute/cactus/resolve/main/releases/4.1.1/python/cactus_runtime-4.1.1-py3-none-win_amd64.whl";
inline constexpr const char* WHISTLE_WHL_SHA256 = "876eea21e31bcad795b9e84cfaadf65e01a32a64ef113c8b77616d33ab8e3169";

inline std::atomic<int> g_cactusDownload[CACTUS_COUNT] = {}; // per model: 0 idle, 1 downloading, 2 the last download failed

// Downloads model m, and the runtime when it is not there yet, on a thread; g_cactusDownload[m] tells how it went.
inline void CactusDownload(int m) {
    if (g_cactusDownload[m].exchange(1) == 1) return;
    std::thread([m] {
        static std::mutex one; // both models downloading at once would both write the runtime
        std::lock_guard<std::mutex> lock(one);
        bool ok = false;
        std::string whl, model;
        DWORD code = 0;
        std::wstring dir = WhistleDir(), whlFile = dir + L"\\cactus_runtime.whl";
        SHCreateDirectoryExW(NULL, (dir + L"\\models").c_str(), NULL);
        if (GetFileAttributesW(WhistleDll().c_str()) == INVALID_FILE_ATTRIBUTES &&
            HttpsRequest(L"GET", WHISTLE_HOST, WHISTLE_WHL_PATH, L"", "", whl, code, nullptr, 8u << 20) && code == 200 &&
            Sha256Hex(whl) == WHISTLE_WHL_SHA256 && WriteBytesFile(whlFile, whl)) {
            // The wheel is a zip; Windows' own tar reads it (full path, so nothing else named tar is run)
            wchar_t sys[MAX_PATH];
            GetSystemDirectoryW(sys, MAX_PATH);
            std::wstring tar = std::wstring(sys) + L"\\tar.exe";
            std::wstring cmd = L"\"" + tar + L"\" -xf \"" + whlFile + L"\" -C \"" + dir + L"\" --strip-components=1 cactus/libcactus.dll";
            STARTUPINFOW si = { sizeof si };
            PROCESS_INFORMATION pi = {};
            DeleteFileW(WhistleDll().c_str());
            if (CreateProcessW(tar.c_str(), cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 30000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        DeleteFileW(whlFile.c_str());
        ok = GetFileAttributesW(WhistleDll().c_str()) != INVALID_FILE_ATTRIBUTES && HttpsRequest(L"GET", WHISTLE_HOST, CACTUS_MODEL_PATH[m], L"", "", model, code, nullptr, 64u << 20) &&
             code == 200 && model.size() > (1u << 20) && WriteBytesFile(CactusModel(m), model);
        g_cactusDownload[m] = ok ? 0 : 2;
    }).detach();
}

// What the worker sends the notch in WM_COPYDATA's dwData; the data is UTF-8
enum : ULONG_PTR { WHISTLE_READY = 0x57480001, WHISTLE_TEXT = 0x57480002, WHISTLE_ERROR = 0x57480003 };

inline void WhistleSend(HWND to, ULONG_PTR kind, const std::string& utf8) {
    COPYDATASTRUCT cd = { kind, (DWORD)utf8.size(), (PVOID)utf8.data() };
    SendMessageTimeoutW(to, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 2000, NULL);
}

// Loads the runtime and model m into this process; NULL when either is missing or bad
inline HMODULE CactusLoad(int m) {
    typedef int (*LoadFn)(const unsigned char*, unsigned long long);
    HMODULE lib = LoadLibraryExW(WhistleDll().c_str(), NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    LoadFn load = lib ? (LoadFn)(void*)GetProcAddress(lib, "cactus_load") : NULL;
    std::string model; // the runtime copies what it needs: the file's bytes go when this returns
    return load && ReadBytesFile(CactusModel(m), model) && load((const unsigned char*)model.data(), model.size()) >= 0 ? lib : NULL;
}

// The worker process: loads the runtime and the model, then feeds it one second of microphone at a time until the notch's
// window is gone or the notch ends this process. Each answer is the runtime's JSON: "text" settled by that second and the
// "pending" words that may still change.
inline int WhistleWorker(HWND notch) {
    auto fail = [&](const char* why) { WhistleSend(notch, WHISTLE_ERROR, why); return 1; };
    typedef int (*StreamFn)(const float*, int, const char*, const char*, char*, int);
    HMODULE lib = CactusLoad(CACTUS_WHISTLE);
    StreamFn stream = lib ? (StreamFn)(void*)GetProcAddress(lib, "cactus_stream_transcribe_process") : NULL;
    if (!stream) return fail("Whistle no carga. Desc\xC3\xA1rgalo otra vez en Configuraci\xC3\xB3n.");

    // 16 kHz mono, what Whistle expects, in two buffers of one second that take turns
    static short pcm[2][16000];
    WAVEFORMATEX wfx = { WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0 };
    WAVEHDR hdr[2] = {};
    HWAVEIN in = NULL;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (waveInOpen(&in, WAVE_MAPPER, &wfx, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) return fail("No se pudo abrir el micr\xC3\xB3" "fono.");
    for (int i = 0; i < 2; ++i) {
        hdr[i].lpData = (LPSTR)pcm[i];
        hdr[i].dwBufferLength = sizeof pcm[i];
        waveInPrepareHeader(in, &hdr[i], sizeof hdr[i]);
        waveInAddBuffer(in, &hdr[i], sizeof hdr[i]);
    }
    waveInStart(in);
    WhistleSend(notch, WHISTLE_READY, "");

    std::vector<float> samples(16000);
    std::vector<char> out(1 << 18);
    while (IsWindow(notch)) {
        WaitForSingleObject(ev, 1000);
        for (WAVEHDR& h : hdr) {
            if (!(h.dwFlags & WHDR_DONE)) continue;
            int n = (int)(h.dwBytesRecorded / sizeof(short));
            for (int i = 0; i < n; ++i) samples[i] = ((const short*)h.lpData)[i] / 32768.0f;
            out[0] = 0;
            if (n > 0 && stream(samples.data(), n, NULL, NULL, out.data(), (int)out.size()) >= 0) WhistleSend(notch, WHISTLE_TEXT, out.data());
            waveInAddBuffer(in, &h, sizeof h);
        }
    }
    return 0;
}

// The Mirage worker: while the notch lives, looks at the notch's monitor every two seconds and, when it changed, sends
// Mirage's JSON (the scene and the text it read). The pixels never leave this process.
inline int MirageWorker(HWND notch) {
    auto fail = [&](const char* why) { WhistleSend(notch, WHISTLE_ERROR, why); return 1; };
    typedef int (*DescribeFn)(const unsigned char*, int, int, const char*, const char*, const char*, char*, int);
    HMODULE lib = CactusLoad(CACTUS_MIRAGE);
    DescribeFn describe = lib ? (DescribeFn)(void*)GetProcAddress(lib, "cactus_describe") : NULL;
    if (!describe) return fail("Mirage no carga. Desc\xC3\xA1rgalo otra vez en Configuraci\xC3\xB3n.");

    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(notch, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down rows, what the runtime expects
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = NULL;
    HBITMAP bmp = w > 0 && h > 0 ? CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!bmp) return fail("No se pudo capturar la pantalla.");
    SelectObject(mem, bmp);
    WhistleSend(notch, WHISTLE_READY, "");

    std::vector<unsigned char> rgb((size_t)w * h * 3), last;
    std::vector<char> out(1 << 18);
    while (IsWindow(notch)) {
        BitBlt(mem, 0, 0, w, h, screen, mi.rcMonitor.left, mi.rcMonitor.top, SRCCOPY);
        RECT own;
        GetWindowRect(notch, &own);
        OffsetRect(&own, -mi.rcMonitor.left, -mi.rcMonitor.top);
        FillRect(mem, &own, (HBRUSH)GetStockObject(BLACK_BRUSH)); // or Mirage would read its own answer in the notch
        GdiFlush();
        const unsigned char* p = (const unsigned char*)bits;
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) { rgb[i * 3] = p[i * 4 + 2]; rgb[i * 3 + 1] = p[i * 4 + 1]; rgb[i * 3 + 2] = p[i * 4]; }
        // shortcut: any changed pixel reruns the model (about a second of CPU), compare a downscaled frame if that is too often
        if (rgb != last) {
            out[0] = 0;
            if (describe(rgb.data(), w, h, "{\"sections\":[\"scene\",\"text\"],\"max_text_lines\":12}", "es", NULL, out.data(), (int)out.size()) >= 0)
                WhistleSend(notch, WHISTLE_TEXT, out.data());
            last = rgb;
        }
        Sleep(2000);
    }
    return 0;
}

// Mirage's answer as one line for the card: the scene, then the text it read on the screen
inline std::wstring MirageSummary(const Json& j) {
    std::wstring s = j["scene"]["setting"].str();
    for (const Json& line : j["text"].a) s += (s.empty() ? L"" : L" · ") + line["string"].str();
    return s;
}

// The notch's side: starts and ends the worker (either model), and keeps what it has heard or seen.
class WhistleEngine {
public:
    HANDLE process = NULL;
    bool ready = false;                 // the worker has the model loaded and the microphone open
    std::wstring text, pending, error;  // settled transcript, the words still changing, why it is not running
    int model = CACTUS_WHISTLE;         // which model Start runs; change it only while stopped

    // Also notices a worker that ended by itself
    bool Running() {
        if (process && WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
            Close();
            if (error.empty()) error = std::wstring(CACTUS_NAME[model]) + L" se detuvo";
        }
        return process != NULL;
    }

    void Start(HWND notch) {
        if (Running() || !CactusInstalled(model)) return;
        text.clear();
        pending.clear();
        error.clear();
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + (model == CACTUS_MIRAGE ? L"\" --mirage " : L"\" --whistle ") + std::to_wstring((unsigned long long)(UINT_PTR)notch);
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(exe, cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            process = pi.hProcess;
        } else {
            error = L"No se pudo iniciar " + std::wstring(CACTUS_NAME[model]);
        }
    }

    // Ends the worker: the model and everything the runtime allocated go with its process
    void Stop() {
        if (!process) return;
        TerminateProcess(process, 0);
        Close();
    }

    // What the worker sent (see WhistleSend). Text is joined with spaces, as the runtime asks; only the tail is kept.
    void OnMessage(ULONG_PTR kind, const std::string& utf8) {
        if (kind == WHISTLE_READY) {
            ready = true;
        } else if (kind == WHISTLE_ERROR) {
            error = Wide(utf8);
        } else if (kind == WHISTLE_TEXT) {
            Json j;
            if (!JsonParse(utf8, j)) return;
            if (model == CACTUS_MIRAGE) { text = MirageSummary(j); return; } // each look replaces the last one
            std::wstring more = j["text"].str();
            if (!more.empty()) text += (text.empty() ? L"" : L" ") + more;
            if (text.size() > 4000) text.erase(0, text.size() - 4000);
            pending = j["pending"].str();
        }
    }

    ~WhistleEngine() { Stop(); }

private:
    void Close() {
        CloseHandle(process);
        process = NULL;
        ready = false;
        pending.clear();
    }
};
