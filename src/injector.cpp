// Void Injector — LoadLibrary injection into cs2.exe
#include <Windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <string>

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

static bool inject(DWORD pid, const std::string& dll) {
    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) return false;

    void* mem = VirtualAllocEx(proc, nullptr, dll.size() + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { CloseHandle(proc); return false; }

    if (!WriteProcessMemory(proc, mem, dll.c_str(), dll.size() + 1, nullptr)) {
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"),
        mem, 0, nullptr);
    if (!th) {
        VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }
    WaitForSingleObject(th, 10000);

    VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(th);
    CloseHandle(proc);
    return true;
}

int main() {
    std::cout << "=== Void Injector ===" << std::endl;

    DWORD pid = 0;
    for (int i = 0; i < 30 && !pid; i++) {
        pid = find_pid(L"cs2.exe");
        if (!pid) { std::cout << "."; Sleep(1000); }
    }
    if (!pid) {
        std::cerr << "\n[-] cs2.exe not found (waited 30s)." << std::endl;
        system("pause");
        return 1;
    }
    std::cout << "\n[+] cs2.exe pid=" << pid << std::endl;

    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    dir += "void_internal.dll";

    std::cout << "[+] injecting " << dir << std::endl;
    if (inject(pid, dir)) std::cout << "[+] done. F1-F5 toggles, END unload, RSHIFT menu." << std::endl;
    else std::cerr << "[-] inject failed, err=" << GetLastError() << " (run as admin)" << std::endl;

    system("pause");
    return 0;
}
