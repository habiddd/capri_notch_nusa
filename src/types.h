#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <shlobj.h>

// %LOCALAPPDATA%\name, or the working directory when that folder is unknown
inline std::wstring AppDataFile(const wchar_t* name) {
    wchar_t appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appData))) return std::wstring(appData) + L"\\" + name;
    return name;
}

// UTF-8 bytes: wofstream's default "C" locale fails on any accent and truncates the rest of the file.
// Written to a temp file and swapped in, so a crash mid-write never leaves a half file.
inline void WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    std::string bytes(WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &bytes[0], (int)bytes.size(), NULL, NULL);
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp.c_str(), std::ios::binary);
        if (!(out << bytes)) return;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// False when there is no file yet.
inline bool ReadUtf8File(const std::wstring& path, std::wstring& text) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.is_open()) return false;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text.assign(MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), NULL, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), &text[0], (int)text.size());
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

