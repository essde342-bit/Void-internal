// Void Injector — запуск через Steam, все библиотеки, честная проверка результата.
#include <Windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "advapi32.lib")

static void ok(const std::string& m){ std::cout<<"[OK] "<<m<<"\n"; }
static void info(const std::string& m){ std::cout<<"[..] "<<m<<"\n"; }
static void warn(const std::string& m){ std::cout<<"[!!] "<<m<<"\n"; }
static void err(const std::string& m){ std::cout<<"[XX] "<<m<<"\n"; }

static std::wstring exe_dir() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr,p,MAX_PATH);
    std::wstring s(p); return s.substr(0,s.find_last_of(L"\\/")+1);
}
static std::string narrow(const std::wstring& w) {
    if(w.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),nullptr,0,nullptr,nullptr);
    std::string s(n,0);
    WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),&s[0],n,nullptr,nullptr);
    return s;
}
static std::wstring steam_root() {
    wchar_t buf[MAX_PATH]={}; DWORD sz=sizeof(buf);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",
                    RRF_RT_REG_SZ,nullptr,buf,&sz)!=ERROR_SUCCESS) return {};
    std::wstring s(buf);
    for(auto& c:s) if(c==L'/') c=L'\\';
    return s;
}
static const wchar_t* CS2_REL =
    L"\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\cs2.exe";

// FIX: ищем во ВСЕХ библиотеках Steam через libraryfolders.vdf
static std::wstring find_cs2() {
    std::wstring root=steam_root();
    if(root.empty()) return {};
    std::wstring direct=root+CS2_REL;
    if(GetFileAttributesW(direct.c_str())!=INVALID_FILE_ATTRIBUTES) return direct;

    std::ifstream vdf(root+L"\\steamapps\\libraryfolders.vdf");
    std::string line;
    while(std::getline(vdf,line)) {
        size_t p=line.find("\"path\"");
        if(p==std::string::npos) continue;
        size_t q1=line.find('"',p+6); if(q1==std::string::npos) continue;
        size_t q2=line.find('"',q1+1); if(q2==std::string::npos) continue;
        std::string path=line.substr(q1+1,q2-q1-1);
        std::wstring w(path.begin(),path.end());
        for(auto& c:w) if(c==L'/') c=L'\\';
        std::string unesc;
        for(size_t i=0;i<w.size();i++){ if(w[i]==L'\\'&&i+1<w.size()&&w[i+1]==L'\\'){i++;} unesc+=(char)w[i]; }
        std::wstring clean(unesc.begin(),unesc.end());
        std::wstring cand=clean+CS2_REL;
        if(GetFileAttributesW(cand.c_str())!=INVALID_FILE_ATTRIBUTES) return cand;
    }
    return {};
}

static DWORD find_pid(const wchar_t* name) {
    PROCESSENTRY32W pe={}; pe.dwSize=sizeof(pe);
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE) return 0;
    DWORD pid=0;
    if(Process32FirstW(snap,&pe)){
        do{ if(_wcsicmp(pe.szExeFile,name)==0){pid=pe.th32ProcessID;break;} }
        while(Process32NextW(snap,&pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool module_loaded(DWORD pid,const std::wstring& mod) {
    for(int attempt=0;attempt<5;attempt++){
        HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
        if(snap==INVALID_HANDLE_VALUE){ Sleep(200); continue; }
        MODULEENTRY32W me={}; me.dwSize=sizeof(me);
        bool found=false;
        if(Module32FirstW(snap,&me)){
            do{ if(_wcsicmp(me.szModule,mod.c_str())==0){found=true;break;} }
            while(Module32NextW(snap,&me));
        }
        CloseHandle(snap);
        return found;
    }
    return false;
}

// FIX: запускаем через Steam -applaunch 730 (нужен Steam-контекст)
static bool launch_via_steam() {
    std::wstring root=steam_root();
    if(root.empty()) return false;
    std::wstring steamexe=root+L"\\steam.exe";
    if(GetFileAttributesW(steamexe.c_str())==INVALID_FILE_ATTRIBUTES) return false;
    STARTUPINFOW si={}; si.cb=sizeof(si);
    PROCESS_INFORMATION pi={};
    std::wstring cmd=L"\""+steamexe+L"\" -applaunch 730";
    std::vector<wchar_t> buf(cmd.begin(),cmd.end()); buf.push_back(0);
    if(!CreateProcessW(nullptr,buf.data(),nullptr,nullptr,FALSE,0,nullptr,root.c_str(),&si,&pi))
        return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

static bool inject(DWORD pid,const std::wstring& dll) {
    HANDLE proc=OpenProcess(PROCESS_ALL_ACCESS,FALSE,pid);
    if(!proc){ err("OpenProcess err="+std::to_string(GetLastError())); return false; }
    size_t bytes=(dll.size()+1)*sizeof(wchar_t);
    void* mem=VirtualAllocEx(proc,nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!mem){ CloseHandle(proc); err("VirtualAllocEx failed"); return false; }
    if(!WriteProcessMemory(proc,mem,dll.c_str(),bytes,nullptr)){
        VirtualFreeEx(proc,mem,0,MEM_RELEASE); CloseHandle(proc);
        err("WriteProcessMemory failed"); return false;
    }
    HANDLE th=CreateRemoteThread(proc,nullptr,0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW"),
        mem,0,nullptr);
    if(!th){
        VirtualFreeEx(proc,mem,0,MEM_RELEASE); CloseHandle(proc);
        err("CreateRemoteThread failed"); return false;
    }
    WaitForSingleObject(th,20000);
    DWORD code=0; GetExitCodeThread(th,&code);
    VirtualFreeEx(proc,mem,0,MEM_RELEASE);
    CloseHandle(th); CloseHandle(proc);
    if(code==0){ err("LoadLibraryW returned NULL (dll not loaded)"); return false; }
    return true;
}

static void flow(bool launch) {
    std::wstring dllpath=exe_dir()+L"void_internal.dll";
    if(GetFileAttributesW(dllpath.c_str())==INVALID_FILE_ATTRIBUTES){
        err("dll not found: "+narrow(dllpath)); return;
    }
    DWORD pid=find_pid(L"cs2.exe");
    if(!pid&&launch){
        std::wstring cs2=find_cs2();
        if(cs2.empty()){ err("CS2 not found in any Steam library"); return; }
        info("CS2: "+narrow(cs2));
        info("launching via Steam -applaunch 730 ...");
        if(!launch_via_steam()){ err("failed to start Steam"); return; }
        info("waiting for cs2.exe (up to 90s) ...");
        for(int i=0;i<90&&!pid;i++){ Sleep(1000); pid=find_pid(L"cs2.exe"); }
        if(!pid){ err("cs2.exe never appeared"); return; }
        ok("cs2.exe pid="+std::to_string(pid));
        info("waiting 5s for process init ...");
        Sleep(5000);
    } else if(!pid){
        info("waiting for cs2.exe (30s) ...");
        for(int i=0;i<30&&!pid;i++){ pid=find_pid(L"cs2.exe"); if(!pid) Sleep(1000); }
        if(!pid){ err("cs2.exe not found"); return; }
    }
    if(module_loaded(pid,L"void_internal.dll")){ warn("already injected - skip"); return; }
    info("injecting ...");
    if(inject(pid,dllpath)) ok("injected. RIGHT SHIFT = menu, END = unload");
    else err("injection failed (run as Administrator)");
}

int main() {
    std::cout<<"=== VOID INJECTOR ===\n";
    for(;;){
        std::cout<<"\n 1) Launch CS2 + Inject\n 2) Inject into running CS2\n 3) Exit\n > ";
        std::string inp;
        if(!std::getline(std::cin,inp)) break;
        if(inp=="1") flow(true);
        else if(inp=="2") flow(false);
        else if(inp=="3") break;
        else warn("unknown option");
    }
    std::cout<<"void out.\n";
    return 0;
}
