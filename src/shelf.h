#pragma once
#include "types.h"
#include <shobjidl.h>
#include <algorithm>
#include <sstream>
#include <thread>

#define WM_SHELF_SENT (WM_USER + 105) // the send thread's answer: wParam 1 when Quick Share took the files

#define WM_SHELF_COPIED (WM_USER + 106) // the copy thread's answer: lParam a std::vector<std::wstring>* of the copies made (the UI thread deletes it), wParam how many failed

// Files dropped on the notch are copied into `dir`; the shelf lists those copies, so the originals are never touched.
// In memory it is only their paths: a full shelf costs a few KB.
class ShelfManager {
public:
    std::vector<std::wstring> files; // newest first
    std::wstring storagePath, dir;

    explicit ShelfManager(const std::wstring& path = AppDataFile(L"capri_shelf.txt"), const std::wstring& copies = AppDataFile(L"CapriNotch\\shelf"))
        : storagePath(path), dir(copies) { Load(); }

    // A file already on the shelf moves to the top instead of repeating
    void Add(const std::wstring& path) {
        if (path.empty()) return;
        files.erase(std::remove(files.begin(), files.end(), path), files.end());
        files.insert(files.begin(), path);
        Save();
    }

    void Remove(size_t index) {
        if (index >= files.size()) return;
        // Only the shelf's own copies go to the Recycle Bin: a path kept from before copies existed is the user's original
        if (files[index].rfind(dir + L"\\", 0) == 0) {
            std::wstring from = files[index] + L'\0'; // SHFileOperation wants a double-null-terminated list
            SHFILEOPSTRUCTW op = {};
            op.wFunc = FO_DELETE;
            op.pFrom = from.c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
            SHFileOperationW(&op);
        }
        files.erase(files.begin() + index);
        Save();
    }

    void Save() {
        std::wstring all;
        for (const auto& f : files) all += f + L"\n";
        WriteUtf8File(storagePath, all);
    }

    void Load() {
        files.clear();
        std::wstring all;
        if (!ReadUtf8File(storagePath, all)) return;
        std::wistringstream lines(all);
        std::wstring line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            // a copy deleted by hand leaves the shelf
            if (!line.empty() && GetFileAttributesW(line.c_str()) != INVALID_FILE_ATTRIBUTES) files.push_back(line);
        }
    }

    static std::wstring Name(const std::wstring& path) {
        size_t s = path.find_last_of(L"\\/");
        return s == std::wstring::npos ? path : path.substr(s + 1);
    }
};

// Copies `src` into `dir` under its own name, or "name (2).ext", "name (3).ext"... when that is taken.
// Returns the copy's path; empty when it could not be copied (a folder, no space, the original gone).
inline std::wstring ShelfCopy(const std::wstring& src, const std::wstring& dir) {
    std::wstring name = ShelfManager::Name(src), ext;
    size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        ext = name.substr(dot);
        name.resize(dot);
    }
    for (int i = 1; i < 1000; ++i) {
        std::wstring dst = dir + L"\\" + name + (i > 1 ? L" (" + std::to_wstring(i) + L")" : L"") + ext;
        if (CopyFileW(src.c_str(), dst.c_str(), TRUE)) return dst;
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    return L"";
}

// Copies on a short-lived thread, so a large file freezes neither the notch nor the window it was dragged from.
inline void ShelfCopyAsync(HWND notify, std::vector<std::wstring> paths, std::wstring dir) {
    std::thread([notify, paths = std::move(paths), dir = std::move(dir)] {
        SHCreateDirectoryExW(NULL, dir.c_str(), NULL); // makes the parent folders too
        auto* made = new std::vector<std::wstring>;
        for (const auto& p : paths) {
            std::wstring copy = ShelfCopy(p, dir);
            if (!copy.empty()) made->push_back(copy);
        }
        if (!PostMessageW(notify, WM_SHELF_COPIED, paths.size() - made->size(), (LPARAM)made)) delete made;
    }).detach();
}

// Google Quick Share's "Send with Quick Share" Explorer command (the Clsid in its AppxManifest).
// shortcut: Quick Share has no public API and this id is Google's; if an update changes it the send reports failure. Upgrade to the Windows share sheet (IDataTransferManagerInterop) then.
inline constexpr CLSID CLSID_QUICK_SHARE = { 0xAD54D7CA, 0x052D, 0x4730, { 0xB0, 0xF2, 0x2B, 0xA3, 0x4D, 0x34, 0x38, 0xDC } };

// Hands the files to Quick Share, which shows its device picker and does the transfer in its own process. Runs on a
// short-lived thread (the shell wants an STA and the launch can take a second); all it allocated is freed when it ends.
inline void ShelfSend(HWND notify, std::vector<std::wstring> paths) {
    std::thread([notify, paths = std::move(paths)] {
        bool ok = false;
        if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
            std::vector<PIDLIST_ABSOLUTE> ids;
            for (const auto& p : paths)
                if (PIDLIST_ABSOLUTE id = ILCreateFromPathW(p.c_str())) ids.push_back(id);
            IShellItemArray* items = NULL;
            IExplorerCommand* cmd = NULL;
            if (!ids.empty() && SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE_ARRAY)ids.data(), &items))
                && SUCCEEDED(CoCreateInstance(CLSID_QUICK_SHARE, NULL, CLSCTX_ALL, IID_IExplorerCommand, (void**)&cmd)))
                ok = SUCCEEDED(cmd->Invoke(items, NULL));
            if (cmd) cmd->Release();
            if (items) items->Release();
            for (PIDLIST_ABSOLUTE id : ids) ILFree(id);
            CoUninitialize();
        }
        PostMessageW(notify, WM_SHELF_SENT, ok, 0);
    }).detach();
}
