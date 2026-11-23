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
#include "settings.h"

// Whistle (Cactus Compute): speech to text that runs on the CPU, with no network and no key.
// The model never loads into the notch. A worker process (this exe started with --whistle) loads Cactus's runtime DLL and
// the 16.9 MB model, listens to the microphone and sends what it hears to the notch. The runtime has no call to unload a
// model, so ending the worker is how its memory is given back: all of it, at once.

inline std::wstring WhistleDir() { return AppDataFile(L"CapriNotch"); }
inline std::wstring WhistleDll() { return WhistleDir() + L"\\libcactus.dll"; }
// The two Cactus models that share the runtime, by index
enum { CACTUS_WHISTLE, CACTUS_MIRAGE, CACTUS_CAMERA, CACTUS_COUNT };
inline constexpr const wchar_t* CACTUS_NAME[] = { L"Whistle", L"Mirage", L"Cámara" };
inline constexpr const wchar_t* CACTUS_MODEL_PATH[] = { L"/Cactus-Compute/whistle/resolve/main/whistle.cact", L"/Cactus-Compute/mirage/resolve/main/mirage.cact", L"" };
inline std::wstring CactusModel(int m) { return WhistleDir() + L"\\models\\" + CACTUS_NAME[m] + L".cact"; }
// The camera's two ONNX models (camera.h): 0 palm detector, 1 hand landmarks
inline std::wstring CameraModel(int i) { return WhistleDir() + (i ? L"\\models\\hand.onnx" : L"\\models\\palm.onnx"); }
// The hand engines (Settings::handEngine): MediaPipe's two models, or RTMPose's detector and pose model
enum { HAND_MEDIAPIPE, HAND_RTMPOSE, HAND_ENGINE_COUNT };
inline std::wstring HandModel(int e, int i) {
    if (e == HAND_MEDIAPIPE) return CameraModel(i);
    return WhistleDir() + (i ? L"\\models\\rtm_pose.onnx" : L"\\models\\rtm_det.onnx");
}
inline bool HandInstalled(int e) { return GetFileAttributesW(HandModel(e, 0).c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(HandModel(e, 1).c_str()) != INVALID_FILE_ATTRIBUTES; }
// OpenCV Zoo's MediaPipe hand models (Apache 2.0), by the hash each file must have
inline constexpr const wchar_t* CAMERA_MODEL_PATH[] = { L"/opencv/palm_detection_mediapipe/resolve/main/palm_detection_mediapipe_2023feb.onnx", L"/opencv/handpose_estimation_mediapipe/resolve/main/handpose_estimation_mediapipe_2023feb.onnx" };
inline constexpr const char* CAMERA_MODEL_SHA256[] = { "78ff51c38496b7fc8b8ebdb6cc8c1abb02fa6c38427c6848254cdaba57fcce7c", "db0898ae717b76b075d9bf563af315b29562e11f8df5027a1ef07b02bef6d81c" };
inline constexpr const wchar_t* CAMERA_HELP = L"Índice y medio arriba: subir volumen · Índice y medio abajo: bajar volumen · Pulgar a tu derecha: siguiente canción · Índice y medio a tu izquierda: canción anterior · Palma abierta 1 s: bloquear el PC (quítala para cancelar)";
inline bool CactusInstalled(int m) {
    if (m == CACTUS_CAMERA) return HandInstalled(g_settings.handEngine);
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

// RTMPose: OpenMMLab's zips (Apache-2.0 code; weights trained on public hand datasets incl. COCO). Each zip and each ONNX inside it has its published hash.
struct HandZip { const wchar_t* path; const char* zipSha; const wchar_t* member; const char* onnxSha; int index; };
inline constexpr const wchar_t* RTM_HOST = L"download.openmmlab.com";
inline constexpr HandZip RTM_ZIPS[2] = {
    { L"/mmpose/v1/projects/rtmposev1/onnx_sdk/rtmdet_nano_8xb32-300e_hand-267f9c8f.zip", "9c0370a43c02b2fe42b4382aba7383d97cfa3ed35623b655cac4f0c25cfde402",
      L"20230831/rtmdet_onnx/rtmdet_nano_8xb32-300e_hand-267f9c8f/end2end.onnx", "568d3ea97a5b142488366b67e036b6a5cb0a1fef9087a710cb8e66b6979fbac2", 0 },
    { L"/mmpose/v1/projects/rtmposev1/onnx_sdk/rtmpose-m_simcc-hand5_pt-aic-coco_210e-256x256-74fb594_20230320.zip", "45c9e1aa21b8fe33859973c49bce4647645d312e7a226e398a42f9137d44a56d",
      L"20230831/rtmpose_onnx/rtmpose-m_simcc-hand5_pt-aic-coco_210e-256x256-74fb594_20230320/end2end.onnx", "39e858936bca0f94c09847d4e70b68a51d6c0adac61f36b457fcadb54621cd29", 1 } };

// One RTMPose model: the zip must have its hash, then tar.exe pulls the ONNX out (three folders stripped, so it lands as end2end.onnx),
// which must have its hash too before it is moved into place. Nothing unchecked is kept.
inline bool ExtractHandZip(const HandZip& z) {
    std::string zip;
    DWORD code = 0;
    if (!HttpsRequest(L"GET", RTM_HOST, z.path, L"", "", zip, code, nullptr, 64u << 20) || code != 200 || Sha256Hex(zip) != z.zipSha) return false;
    const std::wstring dir = WhistleDir(), zipFile = dir + L"\\hand_zip.tmp", extracted = dir + L"\\models\\end2end.onnx";
    bool ok = WriteBytesFile(zipFile, zip);
    zip.clear();
    if (ok) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        const std::wstring tar = std::wstring(sys) + L"\\tar.exe"; // full path, so nothing else named tar is run
        std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zipFile + L"\" -C \"" + dir + L"\\models\" --strip-components=3 " + z.member;
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(tar.c_str(), cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 30000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        std::string onnx;
        ok = ReadBytesFile(extracted, onnx) && Sha256Hex(onnx) == z.onnxSha && MoveFileExW(extracted.c_str(), HandModel(HAND_RTMPOSE, z.index).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    DeleteFileW(extracted.c_str()); // a no-op once moved; a rejected file goes
    DeleteFileW(zipFile.c_str());
    return ok;
}

inline std::atomic<int> g_handDownload[HAND_ENGINE_COUNT] = {}; // per engine: 0 idle, 1 downloading, 2 the last download failed

// Downloads one hand engine's models on a thread; g_handDownload[e] tells how it went
inline void HandDownload(int e) {
    if (g_handDownload[e].exchange(1) == 1) return;
    std::thread([e] {
        static std::mutex one; // one download at a time: both engines write the models folder
        std::lock_guard<std::mutex> lock(one);
        bool ok = true;
        DWORD code = 0;
        SHCreateDirectoryExW(NULL, (WhistleDir() + L"\\models").c_str(), NULL);
        if (e == HAND_MEDIAPIPE) { // no runtime: each ONNX file is accepted only with its published hash
            for (int i = 0; i < 2 && ok; ++i) {
                std::string onnx;
                ok = HttpsRequest(L"GET", WHISTLE_HOST, CAMERA_MODEL_PATH[i], L"", "", onnx, code, nullptr, 64u << 20) && code == 200 &&
                     Sha256Hex(onnx) == CAMERA_MODEL_SHA256[i] && WriteBytesFile(CameraModel(i), onnx);
            }
        } else {
            for (const HandZip& z : RTM_ZIPS) ok = ok && ExtractHandZip(z);
        }
        g_handDownload[e] = ok ? 0 : 2;
    }).detach();
}

inline std::atomic<int> g_cactusDownload[CACTUS_COUNT] = {}; // per model: 0 idle, 1 downloading, 2 the last download failed

// Downloads model m, and the runtime when it is not there yet, on a thread; g_cactusDownload[m] tells how it went.
inline void CactusDownload(int m) {
    if (m == CACTUS_CAMERA) return HandDownload(g_settings.handEngine);
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
enum : ULONG_PTR { WHISTLE_READY = 0x57480001, WHISTLE_TEXT = 0x57480002, WHISTLE_ERROR = 0x57480003, WHISTLE_ALERT = 0x57480004,
                          WHISTLE_GESTURE = 0x57480005 }; // GESTURE: camera only, UTF-8 emoji, a tab, then the label

// from: the sender's model (CACTUS_*), which the notch reads from wParam
inline void WhistleSend(HWND to, ULONG_PTR kind, const std::string& utf8, WPARAM from = 0) {
    COPYDATASTRUCT cd = { kind, (DWORD)utf8.size(), (PVOID)utf8.data() };
    SendMessageTimeoutW(to, WM_COPYDATA, from, (LPARAM)&cd, SMTO_ABORTIFHUNG, 2000, NULL);
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
    int model;                          // which model Start runs; change it only while stopped
    int startedHand = HAND_MEDIAPIPE;   // the camera's hand engine when Start ran it: a change restarts the camera

    explicit WhistleEngine(int m = CACTUS_WHISTLE) : model(m) {}

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
        const wchar_t* flag = model == CACTUS_CAMERA ? L"\" --camera " : model == CACTUS_MIRAGE ? L"\" --mirage " : L"\" --whistle ";
        std::wstring cmd = L"\"" + std::wstring(exe) + flag + std::to_wstring((unsigned long long)(UINT_PTR)notch);
        STARTUPINFOW si = { sizeof si };
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(exe, cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            process = pi.hProcess;
            startedHand = g_settings.handEngine;
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
        } else if (model == CACTUS_CAMERA) {
            if (kind == WHISTLE_TEXT || kind == WHISTLE_ALERT) text = Wide(utf8); // plain text: the latest status or countdown
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
        // the camera worker can end while holding the left button (killed or failed mid-drag): let it go
        if (model == CACTUS_CAMERA && GetAsyncKeyState(VK_LBUTTON) < 0) {
            INPUT up = {};
            up.type = INPUT_MOUSE;
            up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &up, sizeof up);
        }
        CloseHandle(process);
        process = NULL;
        ready = false;
        pending.clear();
    }
};
