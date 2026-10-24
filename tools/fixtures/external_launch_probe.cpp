// Test-only child process. Never packaged into the mod or standalone manager.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

static bool UnderRoot(const std::wstring& entry, const std::wstring& root) {
    std::wstring value = entry;
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    if (value.size() < root.size() || _wcsnicmp(value.c_str(), root.c_str(), root.size()))
        return false;
    return value.size() == root.size() || value[root.size()] == L'\\' || value[root.size()] == L'/';
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || std::wcscmp(argv[1], L"--expected-clean-root")) return 2;
    const DWORD dllSize = GetDllDirectoryW(0, nullptr);
    std::vector<wchar_t> dll(dllSize + 1, 0);
    if (dllSize) GetDllDirectoryW(static_cast<DWORD>(dll.size()), dll.data());
    const DWORD pathSize = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::vector<wchar_t> path(pathSize + 1, 0);
    if (pathSize) GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size()));
    bool inheritedRoot = false;
    const std::wstring values(path.data()), root(argv[2]);
    size_t offset = 0;
    while (offset <= values.size()) {
        const size_t end = values.find(L';', offset);
        if (UnderRoot(values.substr(offset, end == std::wstring::npos ? end : end - offset), root))
            inheritedRoot = true;
        if (end == std::wstring::npos) break;
        offset = end + 1;
    }
    const bool dllEmpty = dll[0] == 0;
    std::printf("{\"dll_directory_empty\":%s,\"path_contains_bundle\":%s}\n",
                dllEmpty ? "true" : "false", inheritedRoot ? "true" : "false");
    return dllEmpty && !inheritedRoot ? 0 : 1;
}
