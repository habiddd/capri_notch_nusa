#pragma once
#include "types.h"

class PomodoroManager {
public:
    PomodoroState state;

    void Toggle() {
        state.isRunning = !state.isRunning;
    }

    void Reset() {
        state.isRunning = false;
        state.isBreak = false;
        state.totalSeconds = 25 * 60;
        state.remainingSeconds = state.totalSeconds;
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
                state.totalSeconds = 5 * 60; // 5 min short break
            } else {
                state.totalSeconds = 25 * 60; // 25 min work
            }
            state.remainingSeconds = state.totalSeconds;
            MessageBeep(MB_ICONASTERISK);
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

