#pragma once
#include "types.h"
#include "settings.h"

class PomodoroManager {
public:
    PomodoroState state;

    void Toggle() {
        state.isRunning = !state.isRunning;
    }

    void Reset() {
        state.isRunning = false;
        state.isBreak = false;
        state.totalSeconds = g_settings.focusMin * 60;
        state.remainingSeconds = state.totalSeconds;
    }

    // A changed duration takes effect at once while stopped; while running it waits for the next cycle (Tick reads it then).
    // An unchanged duration leaves a paused session where it was.
    void Retime() {
        int want = (state.isBreak ? g_settings.breakMin : g_settings.focusMin) * 60;
        if (state.isRunning || state.totalSeconds == want) return;
        state.totalSeconds = want;
        state.remainingSeconds = want;
    }

    void Tick() {
        if (!state.isRunning) return;

        if (state.remainingSeconds > 0) {
            state.remainingSeconds--;
        } else {
            // Timer expired, swap modes
            state.isBreak = !state.isBreak;
            if (state.isBreak) {
                state.sessionsCompleted++;
                state.totalSeconds = g_settings.breakMin * 60;
            } else {
                state.totalSeconds = g_settings.focusMin * 60;
            }
            state.remainingSeconds = state.totalSeconds;
            if (g_settings.pomoSound) MessageBeep(MB_ICONASTERISK);
        }
    }

    std::wstring GetFormattedTime() const {
        int m = state.remainingSeconds / 60;
        int s = state.remainingSeconds % 60;
        wchar_t buf[16];
        wsprintfW(buf, L"%02d:%02d", m, s);
        return buf;
    }
};
