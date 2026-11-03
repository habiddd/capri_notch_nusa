#pragma once
#include "types.h"
#include <sstream>

class TodoManager {
public:
    std::vector<TaskItem> tasks;
    std::wstring storagePath;

    TodoManager() {
        storagePath = AppDataFile(L"capri_tasks.txt");
        if (!Load()) { // first run only: an emptied list stays empty
            tasks.push_back({L"Completar configuracion de Capri Notch", false});
            tasks.push_back({L"Probar el Pomodoro de 25 minutos", false});
            tasks.push_back({L"Configurar API de IA para el companero", false});
        }
    }

    // Appends a task, or a subtask after the last subtask of top-level task `parent`. Returns where it went.
    size_t Add(const std::wstring& text, size_t parent = SIZE_MAX) {
        if (text.empty()) return SIZE_MAX;
        size_t at = tasks.size();
        bool sub = parent < tasks.size() && !tasks[parent].sub;
        if (sub) for (at = parent + 1; at < tasks.size() && tasks[at].sub; ++at) {}
        tasks.insert(tasks.begin() + at, {text, false, sub});
        Save();
        return at;
    }

    // Subtasks of top-level task `index`: done and total
    std::pair<int, int> SubProgress(size_t index) const {
        int done = 0, total = 0;
        for (size_t i = index + 1; i < tasks.size() && tasks[i].sub; ++i, ++total) done += tasks[i].completed;
        return { done, total };
    }

    void Toggle(size_t index) {
        if (index < tasks.size()) {
            bool done = tasks[index].completed = !tasks[index].completed;
            // A done task means all its subtasks are done: completing it completes them, reopening a subtask reopens it
            if (!tasks[index].sub && done)
                for (size_t i = index + 1; i < tasks.size() && tasks[i].sub; ++i) tasks[i].completed = true;
            if (tasks[index].sub && !done)
                for (size_t i = index; i-- > 0;) if (!tasks[i].sub) { tasks[i].completed = false; break; }
            Save();
        }
    }

    void Remove(size_t index) {
        if (index < tasks.size()) {
            size_t end = index + 1;
            if (!tasks[index].sub) while (end < tasks.size() && tasks[end].sub) ++end; // a task takes its subtasks along
            tasks.erase(tasks.begin() + index, tasks.begin() + end);
            Save();
        }
    }

    void Save() {
        std::wstring all;
        for (const auto& t : tasks) all += (t.completed ? L"1" : L"0") + std::wstring(t.sub ? L">|" : L"|") + t.text + L"\n";
        WriteUtf8File(storagePath, all);
    }

    // False when there is no file yet. Reads older CRLF files too.
    bool Load() {
        tasks.clear();
        std::wstring all;
        if (!ReadUtf8File(storagePath, all)) return false;
        std::wistringstream lines(all);
        std::wstring line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.size() >= 3 && line[1] == L'|') tasks.push_back({line.substr(2), line[0] == L'1'});
            else if (line.size() >= 4 && line[1] == L'>' && line[2] == L'|') tasks.push_back({line.substr(3), line[0] == L'1', true});
        }
        return true;
    }
};

