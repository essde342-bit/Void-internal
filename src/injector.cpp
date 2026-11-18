// Void Injector — custom dark UI
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <commdlg.h>
#include <windowsx.h>
#include <TlHelp32.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cwchar>
#include <mutex>
#include <ctime>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {
constexpr int WIN_W = 740;
constexpr int WIN_H = 455;
constexpr UINT WM_APP_UPDATE = WM_APP + 10;

constexpr COLORREF BG       = RGB(11, 13, 19);
constexpr COLORREF PANEL    = RGB(19, 22, 31);
constexpr COLORREF PANEL2   = RGB(25, 29, 40);
constexpr COLORREF BORDER   = RGB(43, 48, 64);
constexpr COLORREF TEXT     = RGB(241, 243, 250);
constexpr COLORREF MUTED    = RGB(139, 147, 166);
constexpr COLORREF ACCENT   = RGB(151, 112, 255);
constexpr COLORREF ACCENT_H = RGB(169, 137, 255);
constexpr COLORREF GREEN    = RGB(78, 210, 150);
constexpr COLORREF RED      = RGB(255, 103, 117);

HWND g_hwnd = nullptr;
HFONT g_font_logo = nullptr;
HFONT g_font_title = nullptr;
HFONT g_font_body = nullptr;
HFONT g_font_small = nullptr;
HFONT g_font_button = nullptr;
bool g_busy = false;
bool g_hover_inject = false;
bool g_hover_download = false;
int g_page = 0; // 0 Home, 1 Logs
std::wstring g_status = L"Ready when you are";
std::wstring g_detail = L"CS2 will launch automatically if it is not running.";
int g_progress = 0;

int g_state = 0; // 0 neutral, 1 success, 2 error

std::wstring exe_dir(); // used by the log path helper below
std::mutex g_log_mutex;
std::wstring log_path() { return exe_dir() + L"last_run.txt"; }

void log_event(const std::wstring& message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::wofstream file(log_path(), std::ios::app);
    if (!file) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    file << L"[" << now.wYear << L"-";
    file.width(2); file.fill(L'0'); file << now.wMonth << L"-";
    file.width(2); file << now.wDay << L" ";
    file.width(2); file << now.wHour << L":";
    file.width(2); file << now.wMinute << L":";
    file.width(2); file << now.wSecond << L"] ";
    file << message << L"\r\n";
}

void begin_log() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::wofstream file(log_path(), std::ios::trunc);
    if (!file) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    file << L"VOID Injector — last run\\r\\n";
    file << L"Started: " << now.wYear << L"-" << now.wMonth << L"-"
         << now.wDay << L" " << now.wHour << L":" << now.wMinute
         << L":" << now.wSecond << L"\r\n";
    file << L"----------------------------------------\\r\\n";
}


struct UiUpdate {
    std::wstring status;
    std::wstring detail;
    int progress;
    int state;
    bool finished;
};

void post_update(const std::wstring& status, const std::wstring& detail,
                 int progress = -1, int state = 0, bool finished = false) {
    if (!g_hwnd) return;
    auto* update = new UiUpdate{status, detail, progress, state, finished};
    if (!PostMessageW(g_hwnd, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(update)))
        delete update;
}

std::wstring exe_dir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    const size_t pos = value.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"" : value.substr(0, pos + 1);
}

std::wstring steam_root() {
    wchar_t buf[MAX_PATH]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    std::wstring path(buf);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
}

constexpr const wchar_t* CS2_REL =
    L"\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe";

std::wstring find_cs2() {
    const std::wstring root = steam_root();
    if (root.empty()) return {};

    const std::wstring direct = root + CS2_REL;
    if (GetFileAttributesW(direct.c_str()) != INVALID_FILE_ATTRIBUTES) {
        log_event(L"CS2 executable found in default Steam library.");
        return direct;
    }

    std::ifstream vdf(root + L"\\steamapps\\libraryfolders.vdf");
    std::string line;
    while (std::getline(vdf, line)) {
        const size_t p = line.find("\"path\"");
        if (p == std::string::npos) continue;
        const size_t q1 = line.find('"', p + 6);
        if (q1 == std::string::npos) continue;
        const size_t q2 = line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;

        std::wstring path(line.begin() + q1 + 1, line.begin() + q2);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        std::wstring clean;
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == L'\\' && i + 1 < path.size() && path[i + 1] == L'\\') ++i;
            clean.push_back(path[i]);
        }
        const std::wstring candidate = clean + CS2_REL;
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return {};
}

DWORD find_pid(const wchar_t* name) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

bool module_loaded(DWORD pid, const wchar_t* module) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool found = false;
        if (Module32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szModule, module) == 0) {
                    found = true;
                    break;
                }
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return found;
    }
    return false;
}

bool launch_via_steam() {
    const std::wstring root = steam_root();
    if (root.empty()) {
        log_event(L"ERROR: Steam install path was not found in the current-user registry.");
        return false;
    }

    const std::wstring steam_exe = root + L"\\steam.exe";
    if (GetFileAttributesW(steam_exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log_event(L"ERROR: steam.exe was not found at the registered Steam path.");
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + steam_exe + L"\" -applaunch 730";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');

    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, root.c_str(), &si, &pi)) {
        const DWORD err = GetLastError();
        log_event(L"ERROR: CreateProcessW for Steam failed. Win32 error=" + std::to_wstring(err));
        return false;
    }
    log_event(L"Steam launch request accepted (AppID 730).");

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Uses the same standard LoadLibrary-based DLL loading method as the original injector.
bool inject_existing_dll(DWORD pid, const std::wstring& dll, std::wstring& error) {
    log_event(L"Attempting standard Windows DLL load. CS2 PID=" + std::to_wstring(pid));
    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!process) {
        const DWORD err = GetLastError();
        log_event(L"ERROR: OpenProcess failed. Win32 error=" + std::to_wstring(err));
        error = L"Could not open CS2 process. Try running the launcher as administrator.";
        return false;
    }

    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote_path = VirtualAllocEx(process, nullptr, bytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        const DWORD err = GetLastError();
        log_event(L"ERROR: VirtualAllocEx failed. Win32 error=" + std::to_wstring(err));
        error = L"Could not allocate memory in CS2 (error " +
                std::to_wstring(err) + L").";
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remote_path, dll.c_str(), bytes, nullptr)) {
        const DWORD err = GetLastError();
        log_event(L"ERROR: WriteProcessMemory failed. Win32 error=" + std::to_wstring(err));
        error = L"Could not write the DLL path into CS2.";
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = load_library ? CreateRemoteThread(
        process, nullptr, 0, load_library, remote_path, 0, nullptr) : nullptr;

    if (!thread) {
        const DWORD err = GetLastError();
        log_event(L"ERROR: CreateRemoteThread failed. Win32 error=" + std::to_wstring(err));
        error = L"Windows could not start the DLL loader thread.";
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 20000);
    DWORD exit_code = 0;
    if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &exit_code);

    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(process);

    if (wait != WAIT_OBJECT_0 || exit_code == 0) {
        log_event(L"ERROR: DLL load thread did not report success. Wait result=" +
                  std::to_wstring(wait) + L", thread exit code=" + std::to_wstring(exit_code));
        error = L"Windows did not confirm that the DLL loaded successfully.";
        return false;
    }
    return true;
}

void run_inject() {
    log_event(L"Run started.");
    const std::wstring dll = exe_dir() + L"void_internal.dll";
    log_event(L"Expected DLL path: " + dll);
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log_event(L"ERROR: void_internal.dll is missing beside the injector.");
        post_update(L"DLL file not found",
                    L"Place void_internal.dll in the same folder as this launcher.",
                    0, 2, true);
        return;
    }

    DWORD pid = find_pid(L"cs2.exe");
    if (pid) log_event(L"CS2 already running. PID=" + std::to_wstring(pid));
    if (!pid) {
        log_event(L"CS2 process not found; checking Steam installation.");
        post_update(L"Looking for Counter-Strike 2",
                    L"Checking your Steam installation...",
                    8, 0);

        const std::wstring game_path = find_cs2();
        if (game_path.empty()) {
            log_event(L"ERROR: CS2 executable not found in detected Steam libraries.");
            post_update(L"CS2 installation not found",
                        L"Install CS2 through Steam, then try again.",
                        0, 2, true);
            return;
        }

        post_update(L"Launching Counter-Strike 2",
                    L"Steam is opening the game. Please keep this window open.",
                    14, 0);

        if (!launch_via_steam()) {
            log_event(L"ERROR: Could not start CS2 through Steam.");
            post_update(L"Could not launch Steam",
                        L"Start Steam once, sign in, and press INJECT again.",
                        0, 2, true);
            return;
        }

        for (int i = 0; i < 90 && !pid; ++i) {
            Sleep(1000);
            pid = find_pid(L"cs2.exe");
            const int progress = 16 + (i * 48 / 90);
            if (i % 5 == 0)
                post_update(L"Waiting for Counter-Strike 2",
                            L"Steam is starting the game...",
                            progress, 0);
        }

        if (!pid) {
            log_event(L"ERROR: cs2.exe process did not appear within 90 seconds.");
            post_update(L"CS2 did not start",
                        L"The game process was not detected within 90 seconds.",
                        0, 2, true);
            return;
        }

        log_event(L"CS2 process detected. PID=" + std::to_wstring(pid));
        post_update(L"Preparing the game",
                    L"CS2 is running. Waiting for its startup to finish...",
                    72, 0);
        Sleep(5000);
    } else {
        post_update(L"Counter-Strike 2 detected",
                    L"The game is already running. Preparing the requested action...",
                    65, 0);
    }

    if (module_loaded(pid, L"void_internal.dll")) {
        log_event(L"void_internal.dll is already listed in the target process modules.");
        post_update(L"Already loaded",
                    L"void_internal.dll is already present in the CS2 process.",
                    100, 1, true);
        return;
    }

    post_update(L"Loading component",
                L"Windows is loading void_internal.dll into the game process...",
                86, 0);

    std::wstring error;
    if (!inject_existing_dll(pid, dll, error)) {
        log_event(L"FAILED: " + error);
        post_update(L"Load failed", error, 0, 2, true);
        return;
    }

    log_event(L"SUCCESS: Windows reported the DLL loader thread completed successfully.");
    post_update(L"Load completed",
                L"Windows reported that the DLL was loaded. You can return to the game.",
                100, 1, true);
}

HFONT make_font(int height, int weight) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
}

void rounded(HDC dc, const RECT& r, COLORREF color, COLORREF outline, int radius = 14) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, outline);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void text(HDC dc, int x, int y, int width, int height,
          const std::wstring& value, HFONT font, COLORREF color,
          UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    RECT r{x, y, x + width, y + height};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, value.c_str(), -1, &r, flags);
    SelectObject(dc, old);
}

void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void paint(HDC dc) {
    RECT whole{0, 0, WIN_W, WIN_H};
    HBRUSH bg = CreateSolidBrush(BG);
    FillRect(dc, &whole, bg);
    DeleteObject(bg);

    // Left navigation
    RECT sidebar{0, 0, 150, WIN_H};
    HBRUSH side_brush = CreateSolidBrush(RGB(14, 16, 24));
    FillRect(dc, &sidebar, side_brush);
    DeleteObject(side_brush);
    line(dc, 149, 0, 149, WIN_H, BORDER);

    RECT logo{22, 25, 62, 65};
    rounded(dc, logo, RGB(39, 31, 65), RGB(70, 57, 111), 11);
    text(dc, 22, 25, 40, 40, L"V", g_font_logo, ACCENT_H,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(dc, 72, 25, 65, 20, L"VOID", g_font_title, TEXT);
    text(dc, 72, 46, 65, 16, L"INTERNAL", g_font_small, MUTED);
    line(dc, 18, 84, 132, 84, BORDER);

    RECT home_item{12, 105, 138, 151};
    RECT logs_item{12, 158, 138, 204};
    rounded(dc, home_item, g_page == 0 ? PANEL2 : RGB(14, 16, 24),
            g_page == 0 ? BORDER : RGB(14, 16, 24), 9);
    rounded(dc, logs_item, g_page == 1 ? PANEL2 : RGB(14, 16, 24),
            g_page == 1 ? BORDER : RGB(14, 16, 24), 9);
    text(dc, 28, 105, 100, 46, L"Home", g_font_body,
         g_page == 0 ? TEXT : MUTED);
    text(dc, 28, 158, 100, 46, L"Logs", g_font_body,
         g_page == 1 ? TEXT : MUTED);
    text(dc, 20, 420, 115, 18, L"VOID  •  CS2", g_font_small, MUTED);

    // Main content header
    text(dc, 176, 25, 400, 24, L"VOID INJECTOR", g_font_title, TEXT);
    text(dc, 177, 50, 400, 18, L"COUNTER-STRIKE 2  /  X64", g_font_small, MUTED);
    line(dc, 176, 82, 712, 82, BORDER);

    if (g_page == 1) {
        text(dc, 178, 108, 480, 34, L"Logs", g_font_title, TEXT);
        text(dc, 179, 145, 500, 38,
             L"Download the diagnostic log from your latest run.",
             g_font_body, MUTED);
        
        RECT log_card{176, 202, 712, 348};
        rounded(dc, log_card, PANEL, BORDER, 15);
        RECT file_icon{198, 224, 244, 278};
        rounded(dc, file_icon, PANEL2, BORDER, 9);
        text(dc, 198, 224, 46, 54, L"TXT", g_font_small, ACCENT_H,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, 263, 225, 390, 24, L"last_run.txt", g_font_body, TEXT);
        text(dc, 263, 252, 410, 38,
             L"Contains timestamps, detected errors and the result\nof the latest injector attempt.",
             g_font_small, MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK);

        RECT button{198, 292, 690, 338};
        rounded(dc, button, g_hover_download ? ACCENT_H : ACCENT,
                g_hover_download ? ACCENT_H : ACCENT, 11);
        text(dc, 198, 292, 492, 46, L"DOWNLOAD", g_font_button,
             RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, 178, 370, 500, 36,
             L"The file is saved locally. A new run replaces the previous log.",
             g_font_small, MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK);
        return;
    }

    text(dc, 178, 105, 510, 35, L"Everything ready?", g_font_title, TEXT);
    text(dc, 179, 141, 515, 22,
         L"One click. The launcher takes care of the startup steps.",
         g_font_body, MUTED);

    RECT card{176, 178, 712, 276};
    rounded(dc, card, PANEL, BORDER, 16);
    RECT icon{196, 198, 238, 240};
    rounded(dc, icon, PANEL2, BORDER, 10);
    text(dc, 196, 198, 42, 42, L"CS", g_font_body, TEXT,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(dc, 256, 194, 300, 22, L"COUNTER-STRIKE 2", g_font_body, TEXT);
    text(dc, 256, 218, 340, 20, L"Steam launch  •  x64 process", g_font_small, MUTED);

    const COLORREF dot = g_state == 2 ? RED : (g_state == 1 ? GREEN : ACCENT);
    HBRUSH dot_brush = CreateSolidBrush(dot);
    HGDIOBJ old_brush = SelectObject(dc, dot_brush);
    HPEN null_pen = CreatePen(PS_NULL, 0, dot);
    HGDIOBJ old_pen = SelectObject(dc, null_pen);
    Ellipse(dc, 630, 207, 640, 217);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(null_pen);
    DeleteObject(dot_brush);
    text(dc, 648, 198, 52, 27,
         g_state == 1 ? L"DONE" : (g_state == 2 ? L"ISSUE" : (g_busy ? L"ACTIVE" : L"READY")),
         g_font_small, dot, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT status_card{176, 290, 712, 365};
    rounded(dc, status_card, PANEL, BORDER, 14);
    text(dc, 195, 301, 490, 25, g_status, g_font_body, TEXT);
    text(dc, 195, 328, 495, 22, g_detail, g_font_small, MUTED);

    RECT track{195, 353, 693, 359};
    rounded(dc, track, RGB(37, 42, 55), RGB(37, 42, 55), 5);
    if (g_progress > 0) {
        RECT fill{195, 353, 195 + (498 * std::clamp(g_progress, 0, 100) / 100), 359};
        rounded(dc, fill, g_state == 2 ? RED : ACCENT,
                g_state == 2 ? RED : ACCENT, 5);
    }

    RECT button{176, 383, 712, 433};
    COLORREF button_color = g_busy ? RGB(70, 57, 105) : (g_hover_inject ? ACCENT_H : ACCENT);
    rounded(dc, button, button_color, button_color, 13);
    text(dc, 176, 383, 536, 50, g_busy ? L"PLEASE WAIT..." : L"INJECT",
         g_font_button, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(dc, 178, 437, 530, 15,
         L"Keep this launcher open until the operation finishes.",
         g_font_small, MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void download_log() {
    const std::wstring source = log_path();
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hwnd, L"No log file exists yet. Run the injector first.",
                    L"VOID Logs", MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t destination[MAX_PATH] = L"last_run.txt";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hwnd;
    dialog.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = destination;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return;

    if (!CopyFileW(source.c_str(), destination, FALSE)) {
        const DWORD error = GetLastError();
        MessageBoxW(g_hwnd,
            (L"Could not save the log. Windows error: " + std::to_wstring(error)).c_str(),
            L"VOID Logs", MB_OK | MB_ICONERROR);
        return;
    }
    MessageBoxW(g_hwnd, L"Log saved successfully.", L"VOID Logs",
                MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_font_logo = make_font(-25, FW_BOLD);
        g_font_title = make_font(-23, FW_SEMIBOLD);
        g_font_body = make_font(-15, FW_MEDIUM);
        g_font_small = make_font(-12, FW_NORMAL);
        g_font_button = make_font(-16, FW_BOLD);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC target = BeginPaint(hwnd, &ps);
        HDC memory = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, WIN_W, WIN_H);
        HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
        paint(memory);
        BitBlt(target, 0, 0, WIN_W, WIN_H, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const bool hover = (x >= 176 && x <= 712 && y >= 383 && y <= 433);
        const bool download_hover = (g_page == 1 && x >= 198 && x <= 690 && y >= 292 && y <= 338);
        if (hover != g_hover_inject || download_hover != g_hover_download) {
            g_hover_inject = hover;
            g_hover_download = download_hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hover_inject = false;
        g_hover_download = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);

        if (x >= 12 && x <= 138 && y >= 105 && y <= 151) {
            g_page = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (x >= 12 && x <= 138 && y >= 158 && y <= 204) {
            g_page = 1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_page == 1 && x >= 198 && x <= 690 && y >= 292 && y <= 338) {
            download_log();
            return 0;
        }
        if (y < 58 && x >= 650 && x < 690) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }
        if (y < 58 && x >= 690) {
            if (!g_busy) DestroyWindow(hwnd);
            return 0;
        }
        if (g_page == 0 && x >= 176 && x <= 712 && y >= 383 && y <= 433 && !g_busy) {
            begin_log();
            log_event(L"INJECT button clicked.");
            g_busy = true;
            g_state = 0;
            g_progress = 2;
            g_status = L"Starting up";
            g_detail = L"Checking files and preparing Counter-Strike 2...";
            InvalidateRect(hwnd, nullptr, FALSE);

            HANDLE thread = CreateThread(nullptr, 0,
                [](LPVOID) -> DWORD { run_inject(); return 0; },
                nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
            else {
                log_event(L"ERROR: Could not create worker thread. Win32 error=" +
                          std::to_wstring(GetLastError()));
                g_busy = false;
                g_state = 2;
                g_status = L"Could not start task";
                g_detail = L"Windows could not create the worker thread.";
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(hwnd, message, wparam, lparam);
        if (hit == HTCLIENT) {
            POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &p);
            if (p.y >= 0 && p.y < 80 && p.x >= 150 && p.x < 640) return HTCAPTION;
        }
        return hit;
    }

    case WM_APP_UPDATE: {
        auto* update = reinterpret_cast<UiUpdate*>(lparam);
        if (update) {
            g_status = update->status;
            g_detail = update->detail;
            if (update->progress >= 0) g_progress = update->progress;
            g_state = update->state;
            if (update->finished) g_busy = false;
            delete update;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_CLOSE:
        if (!g_busy) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_font_logo) DeleteObject(g_font_logo);
        if (g_font_title) DeleteObject(g_font_title);
        if (g_font_body) DeleteObject(g_font_body);
        if (g_font_small) DeleteObject(g_font_small);
        if (g_font_button) DeleteObject(g_font_button);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const wchar_t CLASS_NAME[] = L"VoidInjectorWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = CreateSolidBrush(BG);
    wc.lpszClassName = CLASS_NAME;

    if (!RegisterClassExW(&wc)) return 1;

    const int x = (GetSystemMetrics(SM_CXSCREEN) - WIN_W) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - WIN_H) / 2;
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, CLASS_NAME, L"VOID Injector",
        WS_POPUP, x, y, WIN_W, WIN_H, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) return 1;

    HRGN region = CreateRoundRectRgn(0, 0, WIN_W + 1, WIN_H + 1, 22, 22);
    SetWindowRgn(g_hwnd, region, TRUE);

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    // Initial lightweight process check.
    if (find_pid(L"cs2.exe")) {
        g_status = L"Counter-Strike 2 is already running";
        g_detail = L"Press INJECT when you are ready.";
        g_state = 1;
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (wc.hbrBackground) DeleteObject(wc.hbrBackground);
    return static_cast<int>(msg.wParam);
}
