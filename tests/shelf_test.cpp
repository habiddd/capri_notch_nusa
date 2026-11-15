// g++ -std=c++17 -Wall tests/shelf_test.cpp -o %TEMP%\shelf_test.exe -static -lole32 -lshell32 -luuid && %TEMP%\shelf_test.exe
#include <windows.h>
#include <cassert>
#include <cstdio>
#include "../src/shelf.h"

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = tmp, store = dir + L"capri_shelf_test.txt", copies = dir + L"capri_shelf_copies";
    std::wstring a = dir + L"capri_shelf_a.tmp", b = dir + L"capri_shelf_b.tmp";
    DeleteFileW(store.c_str());
    CreateDirectoryW(copies.c_str(), NULL);
    WriteUtf8File(a, L"a");
    WriteUtf8File(b, L"b");

    ShelfManager s(store, copies);
    assert(s.files.empty());
    s.Add(a);
    s.Add(b);
    s.Add(a); // already there: moves to the top, no duplicate
    s.Add(L"");
    assert(s.files.size() == 2 && s.files[0] == a && s.files[1] == b);
    assert(ShelfManager::Name(a) == L"capri_shelf_a.tmp" && ShelfManager::Name(L"solo.txt") == L"solo.txt");

    DeleteFileW(b.c_str()); // a file that is gone leaves the shelf on the next load
    ShelfManager again(store, copies);
    assert(again.files.size() == 1 && again.files[0] == a);
    again.Remove(5); // out of range: nothing happens
    again.Remove(0); // a path outside the copies folder is the user's own file: off the shelf, still on disk
    assert(ShelfManager(store, copies).files.empty() && Exists(a));

    // A drop is a copy: the original stays, a second copy of the same name gets "(2)", removing deletes only the copy
    std::wstring c1 = ShelfCopy(a, copies), c2 = ShelfCopy(a, copies);
    assert(c1 == copies + L"\\capri_shelf_a.tmp" && c2 == copies + L"\\capri_shelf_a (2).tmp" && Exists(c1) && Exists(c2) && Exists(a));
    assert(ShelfCopy(copies, copies).empty() && ShelfCopy(dir + L"capri_no_such_file.tmp", copies).empty()); // a folder, a missing file
    again.Add(c1);
    again.Remove(0);
    assert(!Exists(c1) && Exists(c2) && Exists(a) && again.files.empty());

    DeleteFileW(c2.c_str());
    RemoveDirectoryW(copies.c_str());
    DeleteFileW(a.c_str());
    DeleteFileW(store.c_str());
    puts("shelf_test ok");
    return 0;
}
