#pragma once
#include <windows.h>
#include <pdh.h>
#include <powrprof.h>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

// Native helpers for the notch tabs: system stats (CPU, GPU, battery), spring animation and weather parsing.

// Temperatures in C, -1 when not asked for or Windows offers none. batteryW: watts, + going into the battery, - coming out, 0 idle or none.
struct CapriSys { float cpu; float memUsedGb, memTotalGb; float diskFreeGb, diskTotalGb; int battery; int charging; float cpuTemp, gpuTemp; float batteryW; bool batteryWKnown; bool onAc; };
struct CapriWeather { float temp, feels, wind, tMax, tMin; int humidity, code; };

inline unsigned long long CapriFtToU64(const FILETIME& f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }
inline float CapriGb(unsigned long long bytes) { return (float)(bytes / 1073741824.0); }

// Busy share of the time that passed between two samples, clamped to 0..100. The first sample has no previous one.
inline float CapriCpuPercent(unsigned long long prevBusy, unsigned long long prevTotal, unsigned long long busy, unsigned long long total) {
    unsigned long long dTotal = total > prevTotal ? total - prevTotal : 0;
    if (dTotal == 0) return 0.0f;
    unsigned long long dBusy = busy > prevBusy ? busy - prevBusy : 0;
    return (float)std::clamp(100.0 * dBusy / dTotal, 0.0, 100.0);
}

// Hottest ACPI thermal zone in C through the performance counters (no admin). The query stays open; a counter that
// cannot be added is never tried again.
inline float CapriCpuTemp() {
    static PDH_HQUERY q = NULL;
    static PDH_HCOUNTER c = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (PdhOpenQueryW(NULL, 0, &q) != ERROR_SUCCESS) return -1.0f;
        if (PdhAddEnglishCounterW(q, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &c) != ERROR_SUCCESS) {
            PdhCloseQuery(q);
            q = NULL;
        }
    }
    if (!q || PdhCollectQueryData(q) != ERROR_SUCCESS) return -1.0f;
    DWORD size = 0, n = 0;
    PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &n, NULL);
    if (size == 0) return -1.0f;
    std::vector<BYTE> buf(size);
    auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &n, items) != ERROR_SUCCESS) return -1.0f;
    float hottest = -1.0f;
    for (DWORD i = 0; i < n; ++i) {
        float t = (float)(items[i].FmtValue.doubleValue / 10.0 - 273.15); // the counter is in tenths of kelvin
        if (t > 0 && t > hottest) hottest = t;
    }
    return hottest;
}

// D3DKMT is not in MinGW's headers (WDDM 2.4+, what Task Manager reads); layouts as the SDK's, called through GetProcAddress.
struct CapriKmtAdapter { UINT hAdapter; LUID luid; ULONG sources; BOOL precise; };
struct CapriKmtEnum { ULONG count; CapriKmtAdapter* adapters; };
struct CapriKmtQuery { UINT hAdapter; UINT type; void* data; UINT size; };
struct CapriKmtClose { UINT hAdapter; };
struct CapriKmtPerf { ULONG physicalIndex; ULONGLONG memFreq, maxMemFreq, maxMemFreqOC, memBandwidth, pcieBandwidth; ULONG fanRpm, power, temperature; UCHAR powerState; };

// Hottest GPU adapter in C that answers, -1 when none does. Blocking, so capri_sys_sample reads it at most every 2 s.
inline float CapriGpuTemp() {
    typedef LONG(WINAPI * EnumFn)(CapriKmtEnum*);
    typedef LONG(WINAPI * QueryFn)(CapriKmtQuery*);
    typedef LONG(WINAPI * CloseFn)(CapriKmtClose*);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    auto enumAdapters = gdi ? (EnumFn)GetProcAddress(gdi, "D3DKMTEnumAdapters2") : NULL;
    auto queryInfo = gdi ? (QueryFn)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo") : NULL;
    auto closeAdapter = gdi ? (CloseFn)GetProcAddress(gdi, "D3DKMTCloseAdapter") : NULL;
    if (!enumAdapters || !queryInfo || !closeAdapter) return -1.0f;
    CapriKmtEnum e = { 0, NULL };
    enumAdapters(&e); // the first call only counts the adapters
    std::vector<CapriKmtAdapter> list(e.count);
    e.adapters = list.data();
    if (enumAdapters(&e) != 0) return -1.0f;
    float hottest = -1.0f;
    for (size_t i = 0; i < list.size() && i < e.count; ++i) {
        CapriKmtPerf p = {};
        CapriKmtQuery qi = { list[i].hAdapter, 62, &p, sizeof p }; // KMTQAITYPE_ADAPTERPERFDATA: temperature in tenths of a degree
        if (queryInfo(&qi) == 0 && p.temperature > 0) hottest = (std::max)(hottest, p.temperature / 10.0f);
        CapriKmtClose ca = { list[i].hAdapter };
        closeAdapter(&ca);
    }
    return hottest;
}

// cpu 0..100 since the previous call (0 on the first); battery 0..100 or -1 without battery; disk = drive C:.
// temps and power are asked for only while their card is shown (they cost a counter read and a driver call).
inline void capri_sys_sample(CapriSys* out, bool temps, bool power) {
    if (!out) return;
    static bool hasPrev = false;
    static unsigned long long prevBusy = 0, prevTotal = 0;
    static ULONGLONG gpuAt = 0;
    static float gpuLast = -1.0f;
    CapriSys s = {};
    s.battery = -1;
    s.cpuTemp = s.gpuTemp = -1.0f;

    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        unsigned long long total = CapriFtToU64(kernel) + CapriFtToU64(user); // kernel time includes idle
        unsigned long long idleTicks = CapriFtToU64(idle);
        unsigned long long busy = total > idleTicks ? total - idleTicks : 0;
        if (hasPrev) s.cpu = CapriCpuPercent(prevBusy, prevTotal, busy, total);
        hasPrev = true;
        prevBusy = busy;
        prevTotal = total;
    }

    MEMORYSTATUSEX m = {};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) {
        s.memTotalGb = CapriGb(m.ullTotalPhys);
        s.memUsedGb = CapriGb(m.ullTotalPhys > m.ullAvailPhys ? m.ullTotalPhys - m.ullAvailPhys : 0);
    }

    ULARGE_INTEGER freeBytes = {}, totalBytes = {};
    if (GetDiskFreeSpaceExW(L"C:\\", NULL, &totalBytes, &freeBytes)) {
        s.diskFreeGb = CapriGb(freeBytes.QuadPart);
        s.diskTotalGb = CapriGb(totalBytes.QuadPart);
    }

    SYSTEM_POWER_STATUS p = {};
    if (GetSystemPowerStatus(&p)) {
        if ((p.BatteryFlag & 128) == 0 && p.BatteryLifePercent != 255) s.battery = p.BatteryLifePercent;
        s.charging = (p.BatteryFlag & 8) != 0; // BATTERY_FLAG_CHARGING
        s.onAc = p.ACLineStatus == 1;
    }

    if (temps) {
        s.cpuTemp = CapriCpuTemp();
        ULONGLONG now = GetTickCount64();
        if (!gpuAt || now - gpuAt >= 2000) {
            gpuLast = CapriGpuTemp();
            gpuAt = now;
        }
        s.gpuTemp = gpuLast;
    }

    if (power) {
        SYSTEM_BATTERY_STATE b = {};
        // Rate is a signed count of mW in a DWORD; 0x80000000 = BATTERY_UNKNOWN_RATE (not in MinGW's headers)
        if (CallNtPowerInformation(SystemBatteryState, NULL, 0, &b, sizeof b) == 0 && b.BatteryPresent && b.Rate != 0x80000000u) {
            s.batteryW = (LONG)b.Rate / 1000.0f;
            s.batteryWKnown = true;
        }
    }

    *out = s;
}

// One step of a damped spring pulling *x to target. dt in seconds. Snaps (x=target, v=0) when settled.
inline void capri_spring(float* x, float* v, float target, float dt) {
    const float STIFFNESS = 320.0f;
    const float DAMPING = 30.0f;
    if (!x || !v) return;
    dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.05f) : 0.0f;
    int steps = (int)std::ceil(dt * 120.0f); // sub-steps of at most 1/120 s, keeps the integrator stable
    if (steps > 0) {
        float h = dt / steps;
        for (int i = 0; i < steps; i++) {
            *v += (-STIFFNESS * (*x - target) - DAMPING * *v) * h;
            *x += *v * h;
        }
    }
    if (std::fabs(target - *x) < 0.5f && std::fabs(*v) < 5.0f) {
        *x = target;
        *v = 0.0f;
    }
}

// Number right after `key` (e.g. "\"temperature_2m\":"); false when the key is missing or the number is malformed.
inline bool CapriNumberAfter(const std::string& s, const char* key, double& out) {
    size_t pos = s.find(key);
    if (pos == std::string::npos) return false;
    pos += strlen(key);
    while (pos < s.size() && isspace((unsigned char)s[pos])) pos++;
    size_t end = s.find_first_not_of("0123456789-+.eE", pos);
    std::string num = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    char* stop = nullptr;
    out = strtod(num.c_str(), &stop);
    return !num.empty() && stop == num.c_str() + num.size();
}

// Body of the object that starts after `head`. Open-Meteo lists `*_units` before each block with the same keys,
// so the search must start at the data block, not at the first match.
inline bool CapriSection(const std::string& s, const char* head, std::string& body) {
    size_t start = s.find(head);
    if (start == std::string::npos) return false;
    start += strlen(head);
    size_t end = s.find('}', start);
    if (end == std::string::npos) return false;
    body = s.substr(start, end - start);
    return true;
}

// Saturating like Rust's `as` cast, so a huge number from the network is not undefined behaviour.
inline int CapriToInt(double v) { return (int)std::clamp(v, -2147483648.0, 2147483647.0); }

inline int capri_weather_parse(const char* json, size_t len, CapriWeather* out) {
    if (!json || !out) return 0;
    std::string s(json, len), cur, daily;
    double temp, feels, wind, humidity, code, tMax, tMin;
    bool ok = CapriSection(s, "\"current\":{", cur) && CapriSection(s, "\"daily\":{", daily)
        && CapriNumberAfter(cur, "\"temperature_2m\":", temp)
        && CapriNumberAfter(cur, "\"apparent_temperature\":", feels)
        && CapriNumberAfter(cur, "\"wind_speed_10m\":", wind)
        && CapriNumberAfter(cur, "\"relative_humidity_2m\":", humidity)
        && CapriNumberAfter(cur, "\"weather_code\":", code)
        && CapriNumberAfter(daily, "\"temperature_2m_max\":[", tMax)
        && CapriNumberAfter(daily, "\"temperature_2m_min\":[", tMin);
    if (!ok) return 0;
    *out = { (float)temp, (float)feels, (float)wind, (float)tMax, (float)tMin, CapriToInt(humidity), CapriToInt(code) };
    return 1;
}

inline const char* capri_weather_text(int code) {
    switch (code) {
    case 0: return "Despejado";
    case 1: return "Mayormente despejado";
    case 2: return "Parcialmente nublado";
    case 3: return "Nublado";
    case 45: case 48: return "Niebla";
    case 51: case 53: case 55: case 56: case 57: return "Llovizna";
    case 61: case 63: case 65: case 66: case 67: return "Lluvia";
    case 71: case 73: case 75: case 77: return "Nieve";
    case 80: case 81: case 82: return "Chubascos";
    case 85: case 86: return "Chubascos de nieve";
    case 95: case 96: case 99: return "Tormenta";
    default: return "—";
    }
}
