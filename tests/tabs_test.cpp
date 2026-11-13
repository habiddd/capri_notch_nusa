// g++ -std=c++17 -Wall tests/tabs_test.cpp -o %TEMP%\tabs_test.exe -static -lpdh -lpowrprof && %TEMP%\tabs_test.exe
#include <windows.h>
#include <cassert>
#include <cstring>
#include <string>
#include "../src/tabs.h"

static bool Text(const char* a, const char* b) { return strcmp(a, b) == 0; }

static std::string Replace(std::string s, const std::string& from, const std::string& to) {
    size_t p = s.find(from);
    assert(p != std::string::npos);
    return s.replace(p, from.size(), to);
}

static const char SAMPLE[] = "{\"latitude\":40.4,\"longitude\":-3.7,\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\",\"temperature_2m\":\"°C\",\"relative_humidity_2m\":\"%\",\"apparent_temperature\":\"°C\",\"weather_code\":\"wmo code\",\"wind_speed_10m\":\"km/h\"},\"current\":{\"time\":\"2026-11-13T14:30\",\"interval\":900,\"temperature_2m\":12.3,\"relative_humidity_2m\":71,\"apparent_temperature\":10.9,\"weather_code\":3,\"wind_speed_10m\":14.2},\"daily_units\":{\"time\":\"iso8601\",\"temperature_2m_max\":\"°C\",\"temperature_2m_min\":\"°C\"},\"daily\":{\"time\":[\"2026-11-13\"],\"temperature_2m_max\":[15.8],\"temperature_2m_min\":[7.1]}}";

static int ParseStr(const std::string& s, CapriWeather& w) { return capri_weather_parse(s.data(), s.size(), &w); }

static void TestCpuPercent() {
    assert(CapriCpuPercent(0, 100, 50, 200) == 50.0f); // busy share of delta
    assert(CapriCpuPercent(10, 100, 10, 100) == 0.0f); // no time passed
    assert(CapriCpuPercent(0, 100, 300, 200) == 100.0f); // clamped
}

static void TestSpringSettles() {
    float x = 0.0f, v = 0.0f, max = 0.0f;
    for (int i = 0; i < 125; i++) {
        capri_spring(&x, &v, 100.0f, 0.016f);
        assert(!std::isnan(x) && !std::isnan(v));
        max = std::max(max, x);
    }
    assert(x == 100.0f);
    assert(v == 0.0f);
    assert(max < 110.0f);
}

static void TestSpringIgnoresBadDt() {
    float x = 10.0f, v = 0.0f;
    capri_spring(&x, &v, 100.0f, std::nanf(""));
    assert(x == 10.0f && v == 0.0f);
}

static void TestSysSample() {
    CapriSys s = {};
    s.cpu = -1.0f;
    capri_sys_sample(&s, false, false);
    assert(s.cpu == 0.0f); // first call has no previous sample
    assert(s.cpuTemp == -1.0f && s.gpuTemp == -1.0f && s.batteryW == 0.0f && !s.batteryWKnown); // not asked for
    capri_sys_sample(&s, true, true);
    assert(s.cpu >= 0.0f && s.cpu <= 100.0f);
    assert(s.memTotalGb > 0.0f && s.memUsedGb <= s.memTotalGb);
    assert(s.diskTotalGb > 0.0f);
    assert(s.battery == -1 || (s.battery >= 0 && s.battery <= 100));
    // -1 = Windows offers none; otherwise a sane temperature and a battery rate far below 400 W
    assert(s.cpuTemp == -1.0f || (s.cpuTemp >= 0.0f && s.cpuTemp <= 125.0f));
    assert(s.gpuTemp == -1.0f || (s.gpuTemp >= 0.0f && s.gpuTemp <= 125.0f));
    assert(std::fabs(s.batteryW) < 400.0f);
    assert(s.batteryWKnown || s.batteryW == 0.0f); // an unknown rate is reported as unknown, not as 0 W from a read
}

static void TestWeatherParsesSample() {
    CapriWeather w = {};
    assert(ParseStr(SAMPLE, w) == 1);
    assert(fabsf(w.temp - 12.3f) < 1e-4f);
    assert(fabsf(w.feels - 10.9f) < 1e-4f);
    assert(fabsf(w.wind - 14.2f) < 1e-4f);
    assert(fabsf(w.tMax - 15.8f) < 1e-4f);
    assert(fabsf(w.tMin - 7.1f) < 1e-4f);
    assert(w.humidity == 71 && w.code == 3);
}

static void TestWeatherRejectsBadInput() {
    CapriWeather w = {};
    assert(ParseStr("{}", w) == 0);
    std::string s = SAMPLE;
    assert(ParseStr(s.substr(0, s.find("\"daily\"")), w) == 0); // daily block cut off
    assert(ParseStr(Replace(s, "12.3", "-"), w) == 0);
    const char bad[] = { (char)0xff, (char)0xfe };
    assert(capri_weather_parse(bad, 2, &w) == 0);
}

static void TestWeatherNegativeTemperature() {
    CapriWeather w = {};
    assert(ParseStr(Replace(SAMPLE, "12.3", "-4.5"), w) == 1);
    assert(fabsf(w.temp + 4.5f) < 1e-4f);
}

static void TestWeatherText() {
    assert(Text(capri_weather_text(0), "Despejado"));
    assert(Text(capri_weather_text(63), "Lluvia"));
    assert(Text(capri_weather_text(95), "Tormenta"));
    assert(Text(capri_weather_text(1000), "—"));
}

int main() {
    TestCpuPercent();
    TestSpringSettles();
    TestSpringIgnoresBadDt();
    TestSysSample();
    TestWeatherParsesSample();
    TestWeatherRejectsBadInput();
    TestWeatherNegativeTemperature();
    TestWeatherText();
    return 0;
}
