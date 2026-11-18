// Void Injector — stable console launcher + injector. Launches CS2, injects dll, verifies result.
#include <Windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

static void ok(const std::string& m)  { std::cout << "[OK] " << m << "\n"; }
static void info(const std::string& m){ std::cout << "[..] " << m << "\n"; }
static void warn(const std::string& m){ std::cout << "[!!] " << m << "\n"; }
static void err(const std::string& m) { std::cout << "[XX] " << m << "\n"; }

static std::wstring exe_dir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring find_cs2() {
    wchar_t steam[MAX_PATH] = {};
    DWORD sz = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, steam, &sz) == ERROR_SUCCESS) {
        std::wstring base(steam);
        for (auto& c : base) if (c == L'/') c = L'\\';
        std::wstring p = base + L"\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return {};
}

static DWORD find_pid(const wchar_t* name) {
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool module_loaded(DWORD pid, const std::wstring& modname) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, modname.c_str()) == 0) { found = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static DWORD launch_cs2(const std::wstring& path) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + path + L"\"";
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pi.dwProcessId;
}

// FIX: verify LoadLibraryW result via remote thread exit code
static bool inject(DWORD pid, const std::wstring& dll) {
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) { err("OpenProcess failed err=" + std::to_string(GetLastError())); return false; }

    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { CloseHandle(proc); err("VirtualAllocEx failed"); return false; }

    if (!WriteProcessMemory(proc, mem, dll.c_str(), bytes, nullptr)) {
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE); CloseHandle(proc);
        err("WriteProcessMemory failed"); return false;
    }

    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"),
        mem, 0, nullptr);
    if (!th) {
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE); CloseHandle(proc);
        err("CreateRemoteThread failed"); return false;
    }
    WaitForSingleObject(th, 15000);
    DWORD exitcode = 0;
    GetExitCodeThread(th, &exitcode);
    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(th); CloseHandle(proc);
    if (exitcode == 0) { err("LoadLibraryW returned NULL (dll missing deps?)"); return false; }
    return true;
}

static void flow(bool launch) {
    std::wstring dllpath = exe_dir() + L"void_internal.dll";
    if (GetFileAttributesW(dllpath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err("dll not found next to injector: " + narrow(dllpath));
        return;
    }

    DWORD pid = find_pid(L"cs2.exe");
    if (launch && !pid) {
        std::wstring cs2 = find_cs2();
        if (cs2.empty()) { err("cs2.exe not found via Steam registry"); return; }
        info("launching CS2 ...");
        pid = launch_cs2(cs2);
        if (!pid) { err("CreateProcess failed err=" + std::to_string(GetLastError())); return; }
        ok("CS2 started pid=" + std::to_string(pid));
        info("waiting 2500 ms for game init ...");
        Sleep(2500);
    } else if (!pid) {
        info("waiting for cs2.exe (30s) ...");
        for (int i = 0; i < 30 && !pid; i++) { pid = find_pid(L"cs2.exe"); if (!pid) Sleep(1000); }
        if (!pid) { err("cs2.exe not found"); return; }
    }

    if (module_loaded(pid, L"void_internal.dll")) { warn("dll already injected - skip"); return; }

    info("injecting void_internal.dll ...");
    if (inject(pid, dllpath)) ok("injected. RIGHT SHIFT = menu, END = unload");
    else err("injection failed. run as Administrator.");
}

int main() {
    std::cout << "=== VOID INJECTOR (stable) ===\n";
    for (;;) {
        std::cout << "\n 1) Launch CS2 + Inject\n 2) Inject into running CS2\n 3) Exit\n > ";
        std::string inp;
        if (!std::getline(std::cin, inp)) break;
        if (inp == "1") flow(true);
        else if (inp == "2") flow(false);
        else if (inp == "3") break;
        else warn("unknown option");
    }
    std::cout << "void out.\n";
    return 0;
}
