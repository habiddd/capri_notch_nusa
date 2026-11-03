#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <shlobj.h>
#include "net.h"

// %LOCALAPPDATA%\name, or the working directory when that folder is unknown
inline std::wstring AppDataFile(const wchar_t* name) {
    wchar_t appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData))) return std::wstring(appData) + L"\\" + name;
    return name;
}

// Written to a temp file and swapped in, so a crash mid-write never leaves a half file.
inline bool WriteBytesFile(const std::wstring& path, const std::string& bytes) {
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp.c_str(), std::ios::binary);
        if (!(out << bytes)) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// False when there is no file yet.
inline bool ReadBytesFile(const std::wstring& path, std::string& bytes) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) return false;
    bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

// UTF-8 bytes: wofstream's default "C" locale fails on any accent and truncates the rest of the file.
inline void WriteUtf8File(const std::wstring& path, const std::wstring& text) { WriteBytesFile(path, Utf8(text)); }

inline bool ReadUtf8File(const std::wstring& path, std::wstring& text) {
    std::string bytes;
    if (!ReadBytesFile(path, bytes)) return false;
    text = Wide(bytes);
    return true;
}

enum NotchTab {
    TAB_MEDIA = 0,
    TAB_POMODORO = 1,
    TAB_TASKS = 2,
    TAB_CALENDAR = 3,
    TAB_AI = 4,
    TAB_COUNT
};

struct TaskItem {
    std::wstring text;
    bool completed;
    bool sub = false; // subtask of the nearest top-level task above it
};

struct PomodoroState {
    int remainingSeconds = 25 * 60;
    int totalSeconds = 25 * 60;
    bool isRunning = false;
    bool isBreak = false;
    int sessionsCompleted = 0;
};

