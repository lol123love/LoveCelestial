#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <cstring>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

// MADE BY LOL123LOVE :p

static const char* kLogPath = "C:\\Users\\Public\\celestial_launcher.log";

static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 2;

    HANDLE h = CreateFileA(kLogPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, NULL);
    WriteFile(h, "\r\n", 2, &w, NULL);
    CloseHandle(h);
}

static bool ResolveDllPath(wchar_t* outPath, size_t outLen)
{
    outPath[0] = 0;
    DWORD n = GetModuleFileNameW(NULL, outPath, (DWORD)outLen);
    if (n == 0 || n >= outLen) { Log("[err] GetModuleFileNameW"); return false; }
    wchar_t* slash = wcsrchr(outPath, L'\\');
    if (!slash) { Log("[err] no backslash"); return false; }
    *(slash + 1) = 0;
    if (wcslen(outPath) + 32 >= outLen) { Log("[err] buffer small"); return false; }
    wcscat_s(outPath, outLen, L"LoveCelestial.dll");

    if (GetFileAttributesW(outPath) == INVALID_FILE_ATTRIBUTES) {
        Log("[err] LoveCelestial.dll not found: %ls", outPath);
        return false;
    }
    Log("[resolve] LoveCelestial.dll = %ls", outPath);
    return true;
}

static void ResolveDefaultExe(wchar_t* outPath, size_t outLen)
{
    outPath[0] = 0;
    wchar_t localAppData[MAX_PATH] = {};
    DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (!got || got >= MAX_PATH) { Log("[resolve] no LOCALAPPDATA"); return; }
    Log("[resolve] LOCALAPPDATA = %ls", localAppData);

    const wchar_t* candidates[] = {
        L"\\Celestial\\celestial.exe",
        L"\\Celestial\\Celestial.exe",
        L"\\Programs\\Celestial\\celestial.exe",
        L"\\Programs\\celestial\\celestial.exe",
        L"\\celestial\\celestial.exe",
    };
    for (auto c : candidates) {
        swprintf_s(outPath, outLen, L"%s%s", localAppData, c);
        if (GetFileAttributesW(outPath) != INVALID_FILE_ATTRIBUTES) {
            Log("[resolve] found default exe: %ls", outPath);
            return;
        }
    }
    outPath[0] = 0;
    Log("[resolve] no candidate found");
}

static bool InjectDll(DWORD pid, const wchar_t* dllPath)
{
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) { Log("[err] OpenProcess err=%lu", GetLastError()); return false; }

    SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(hProc, NULL, bytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        Log("[err] VirtualAllocEx err=%lu", GetLastError());
        CloseHandle(hProc); return false;
    }

    if (!WriteProcessMemory(hProc, remote, dllPath, bytes, NULL)) {
        Log("[err] WriteProcessMemory err=%lu", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE); CloseHandle(hProc); return false;
    }

    FARPROC loadLib = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!loadLib) {
        Log("[err] GetProcAddress");
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE); CloseHandle(hProc); return false;
    }

    HANDLE th = CreateRemoteThread(hProc, NULL, 0,
        (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
    if (!th) {
        Log("[err] CreateRemoteThread err=%lu", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE); CloseHandle(hProc); return false;
    }

    DWORD wait = WaitForSingleObject(th, 15000);
    DWORD exitCode = 0;
    if (wait != WAIT_TIMEOUT) {
        GetExitCodeThread(th, &exitCode);
        Log("[inject] LoadLibraryW -> 0x%llX", (unsigned long long)exitCode);
    }
    else {
        Log("[warn] inject timed out");
        exitCode = 1;
    }

    CloseHandle(th);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return exitCode != 0;
}

int main(int argc, char** argv)
{
    CreateDirectoryA("C:\\Users\\Public", NULL);
    Log("========== celestial_launcher ==========");

    wchar_t exePath[MAX_PATH] = {};
    bool    hideUI = true;
    bool    noHide = false;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            MessageBoxA(NULL,
                "celestial_launcher\n\n"
                "Usage:\n"
                "  celestial_launcher [exe_path] [--exe path] [--no-hide]\n\n"
                "  exe_path    Path to celestial.exe\n"
                "              (default: %LOCALAPPDATA%\\Celestial\\celestial.exe)\n"
                "  --exe path  Same as positional, explicit form\n"
                "  --no-hide   Do not hide the game window\n\n"
                "LoveCelestial.dll must sit next to celestial_launcher.exe.\n"
                "Launcher exits after inject — Celestial runs independently.",
                "celestial_launcher", MB_ICONINFORMATION);
            return 0;
        }
        else if (!strcmp(argv[i], "--no-hide")) {
            noHide = true;
        }
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) {
            MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, exePath, MAX_PATH);
        }
        else if (argv[i][0] != '-') {
            if (exePath[0] == 0)
                MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, exePath, MAX_PATH);
        }
    }

    if (exePath[0] == 0) ResolveDefaultExe(exePath, MAX_PATH);

    if (exePath[0] == 0 || GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        Log("[err] Celestial.exe not found");
        MessageBoxW(NULL,
            L"Celestial.exe not found.\n\n"
            L"Tried: %LOCALAPPDATA%\\Celestial\\celestial.exe\n\n"
            L"Override with:\n"
            L"  celestial_launcher.exe --exe \"C:\\path\\to\\celestial.exe\"",
            L"Celestial Bypass", MB_ICONERROR);
        return 2;
    }
    Log("[launch] target = %ls", exePath);
    Log("[launch] noHide = %s", noHide ? "YES" : "NO");

    wchar_t dllPath[MAX_PATH] = {};
    if (!ResolveDllPath(dllPath, MAX_PATH)) {
        MessageBoxA(NULL,
            "LoveCelestial.dll must be in the same folder as celestial_launcher.exe.\n"
            "See C:\\Users\\Public\\celestial_launcher.log",
            "Celestial Bypass", MB_ICONERROR);
        return 1;
    }

    if (noHide) {
        SetEnvironmentVariableW(L"CELESTIAL_NO_HIDE", L"1");
        Log("[launch] CELESTIAL_NO_HIDE set in env (inherited by child)");
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (hideUI && !noHide) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION pi{};

    wchar_t cmdLine[MAX_PATH + 256];
    swprintf_s(cmdLine, L"\"%s\"", exePath);

    DWORD flags = CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP;

    if (!CreateProcessW(exePath, cmdLine, NULL, NULL,
        FALSE, flags, NULL, NULL, &si, &pi)) {
        Log("[err] CreateProcessW err=%lu", GetLastError());
        MessageBoxW(NULL, L"Failed to start Celestial.exe.\nSee log.", L"Error", MB_ICONERROR);
        return 3;
    }
    Log("[launch] pid = %lu (suspended)", pi.dwProcessId);

    if (!InjectDll(pi.dwProcessId, dllPath)) {
        Log("[err] injection failed — terminating game");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 4;
    }
    Log("[launch] injected — DLL holds CDP + hooks + hide thread");

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    Log("[launch] detached — Celestial is now running independently");
    Log("========== launcher exiting ==========");
    return 0;
}
