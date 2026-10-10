#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "backend.h"
#include "login.h"

#include <windows.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>
#include <ctime>
#include <thread>
#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <regex>
#include <set>
#include <functional>
#include <algorithm>

#include "json.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace backend {

std::mutex logMutex;
std::vector<LogEntry> logLines;
long long logTotal = 0;

std::atomic<bool> watching{ false };
std::atomic<int> instanceCount{ 0 };

std::mutex adaptersMutex;
std::vector<NetworkAdapterInfo> adapters;
std::atomic<int> defaultAdapterIndex{ -1 };

std::mutex browserCookieMutex;
std::vector<BrowserCookieStatus> browserCookieStatus;
std::atomic<bool> browserCookieScanning{ false };
std::atomic<bool> browserCookieScanned{ false };

std::wstring g_exeDir;
static std::thread g_watchThread;
static std::chrono::steady_clock::time_point g_startTime;
static std::mutex g_robloxCookieFileMutex;
static HANDLE g_robloxCookieFileHandle = INVALID_HANDLE_VALUE;
static HANDLE g_multiRobloxMutex = nullptr;

void Log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(logMutex);
    char stamp[16] = "";
    time_t now = std::time(nullptr);
    if (std::tm* t = std::localtime(&now)) strftime(stamp, sizeof(stamp), "%H:%M:%S", t);
    logLines.push_back({ stamp, msg });
    ++logTotal;
    if (logLines.size() > 400) logLines.erase(logLines.begin(), logLines.begin() + 100);
}

void ClearLog() {
    std::lock_guard<std::mutex> lock(logMutex);
    logLines.clear();
}

static std::string Narrow(const std::wstring& s) { return std::string(s.begin(), s.end()); }

static std::wstring LocalAppDataPath() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return std::wstring(buf, len);

    len = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return std::wstring(buf, len) + L"\\AppData\\Local";

    return L"";
}

static std::wstring RobloxCookieFilePath() {
    std::wstring local = LocalAppDataPath();
    if (local.empty()) return L"";
    return local + L"\\Roblox\\LocalStorage\\RobloxCookies.dat";
}

static std::string LastWin32ErrorString(DWORD err) {
    LPSTR msg = nullptr;
    DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, (LPSTR)&msg, 0, nullptr);
    std::string result = len && msg ? std::string(msg, len) : "error " + std::to_string(err);
    if (msg) LocalFree(msg);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' ')) result.pop_back();
    return result;
}

static bool ScrubAndLockRobloxCookieFile(const char* reason, bool logSuccess = true) {
    std::lock_guard<std::mutex> lock(g_robloxCookieFileMutex);

    std::wstring path = RobloxCookieFilePath();
    if (path.empty()) {
        Log("[!] Could not resolve RobloxCookies.dat path.");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) {
        Log("[!] Failed to create Roblox LocalStorage folder: " + ec.message());
        return false;
    }

    if (g_robloxCookieFileHandle == INVALID_HANDLE_VALUE) {
        g_robloxCookieFileHandle = CreateFileW(path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (g_robloxCookieFileHandle == INVALID_HANDLE_VALUE) {
            Log("[!] Could not lock RobloxCookies.dat (" + std::string(reason) + "): " +
                LastWin32ErrorString(GetLastError()));
            return false;
        }
    }

    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(g_robloxCookieFileHandle, zero, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(g_robloxCookieFileHandle)) {
        Log("[!] Could not scrub RobloxCookies.dat (" + std::string(reason) + "): " +
            LastWin32ErrorString(GetLastError()));
        return false;
    }

    FlushFileBuffers(g_robloxCookieFileHandle);
    if (logSuccess) Log("[v] RobloxCookies.dat scrubbed and locked (" + std::string(reason) + ").");
    return true;
}

static void ReleaseRobloxCookieFileLock() {
    std::lock_guard<std::mutex> lock(g_robloxCookieFileMutex);
    if (g_robloxCookieFileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_robloxCookieFileHandle);
        g_robloxCookieFileHandle = INVALID_HANDLE_VALUE;
    }
}

static std::atomic<bool> g_ownsRobloxMutex{ false };

static void HoldMultiRobloxMutex() {
    if (g_multiRobloxMutex) return;
    g_multiRobloxMutex = CreateMutexW(nullptr, FALSE, L"ROBLOX_singletonMutex");
    if (!g_multiRobloxMutex) {
        Log("[!] Could not create ROBLOX_singletonMutex: " + LastWin32ErrorString(GetLastError()));
        return;
    }
    HANDLE h = g_multiRobloxMutex;
    std::thread([h]() {
        bool warned = false;
        for (;;) {
            DWORD r = WaitForSingleObject(h, 0);
            if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
                g_ownsRobloxMutex = true;
                Log(warned ? "[v] Took back ROBLOX_singletonMutex - multi-instance launching works again."
                           : "[v] Holding ROBLOX_singletonMutex for multi-instance launching.");
                for (;;) Sleep(INFINITE);
            }
            if (!warned) {
                warned = true;
                Log("[i] A running Roblox client holds ROBLOX_singletonMutex; it will be taken over as soon as that client closes.");
            }
            Sleep(250);
        }
    }).detach();
}

static void ReleaseMultiRobloxMutex() {
    if (!g_multiRobloxMutex) return;
    CloseHandle(g_multiRobloxMutex);
    g_multiRobloxMutex = nullptr;
}

bool IsElevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return elevated;
}

bool RelaunchAsAdmin() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

static std::wstring SelfExePath() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path, n);
}

void Init(const std::wstring& exeDir) {
    g_exeDir = exeDir;
    g_startTime = std::chrono::steady_clock::now();
    LoadAccounts();
    LoadPlaceId();
    LoadSavedPlaces();
    LoadSavedPrivateServers();
    LoadActivePrivateServer();
    LoadJoinBestServer();
    LoadLastServer();
    LoadArrangeSettings();
    StartAutoArrangeWatcher();
    LoadRobloxBuilds();
    LoadCustomFont();
    LoadFpsCaps();
    LoadFastFlags();
    LoadServerHistory();
    LoadPlaytime();
    LoadDiscordSettings();
    std::thread(PrepareRobloxInstalls).detach();
    { std::error_code ec; std::filesystem::remove(SelfExePath() + L".old", ec); }
    std::thread(StartupUpdateCheck).detach();
    ScrubAndLockRobloxCookieFile("startup");
    HoldMultiRobloxMutex();
}

void Shutdown() {
    StopWatching();
    ReleaseRobloxCookieFileLock();
    ReleaseMultiRobloxMutex();
}

static std::vector<DWORD> FindPidsByName(const wchar_t* exeName) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) pids.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

static std::string RunCaptureOutput(const std::wstring& exe, const std::wstring& args) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead, hWrite;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return "";
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);

    std::string output;
    if (ok) {
        char buf[4096];
        DWORD read = 0;
        while (ReadFile(hRead, buf, sizeof(buf), &read, nullptr) && read > 0) {
            output.append(buf, read);
        }
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hRead);
    return output;
}

static std::wstring FindHandleExe() {
    std::wstring p1 = g_exeDir + L"\\handle64.exe";
    std::wstring p2 = g_exeDir + L"\\handle.exe";
    if (std::filesystem::exists(p1)) return p1;
    if (std::filesystem::exists(p2)) return p2;
    return L"";
}

static std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

static std::mutex g_handleWorkMutex;
static std::atomic<long long> g_quietUntilMs{ 0 };

static long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void EnableDebugPrivilege() {
    static bool done = false;
    if (done) return;
    done = true;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(token);
}

int CloseSingletonEventsIn(unsigned long pid) {
    typedef LONG(NTAPI* NtQueryObjectFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static NtQueryObjectFn queryObject = (NtQueryObjectFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject");
    if (!queryObject) return -1;
    EnableDebugPrivilege();
    HANDLE proc = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!proc) return -1;

    struct UnicodeString { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
    static const wchar_t kName[] = L"ROBLOX_singletonEvent";
    const size_t nameLen = wcslen(kName);
    DWORD total = 0;
    if (!GetProcessHandleCount(proc, &total)) total = 4096;
    int closed = 0;
    DWORD seen = 0;
    std::vector<BYTE> buf(2048);
    HANDLE self = GetCurrentProcess();
    for (ULONG_PTR value = 4; value <= 0x40000 && seen < total; value += 4) {
        if (WaitForSingleObject(proc, 0) != WAIT_TIMEOUT) break;
        HANDLE dup = nullptr;
        if (!DuplicateHandle(proc, (HANDLE)value, self, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
        ++seen;
        ULONG got = 0;
        bool isEvent = false;
        if (queryObject(dup, 2, buf.data(), (ULONG)buf.size(), &got) >= 0) {
            auto* type = (UnicodeString*)buf.data();
            isEvent = type->Buffer && type->Length == 5 * sizeof(wchar_t) && wcsncmp(type->Buffer, L"Event", 5) == 0;
        }
        bool match = false;
        if (isEvent && queryObject(dup, 1, buf.data(), (ULONG)buf.size(), &got) >= 0) {
            auto* name = (UnicodeString*)buf.data();
            size_t len = name->Length / sizeof(wchar_t);
            match = name->Buffer && len >= nameLen && wcsncmp(name->Buffer + len - nameLen, kName, nameLen) == 0;
        }
        CloseHandle(dup);
        if (match && DuplicateHandle(proc, (HANDLE)value, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE)) ++closed;
    }
    CloseHandle(proc);
    return closed;
}

static bool ProcessAlive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

static int CloseSingletonsWithHandleExe(const std::wstring& handleExe, DWORD pid) {
    static const std::regex kSingletonRe(
        R"(([0-9A-Fa-f]+):\s+Event\b.*ROBLOX_singletonEvent)",
        std::regex::icase);
    if (handleExe.empty() || !ProcessAlive(pid)) return 0;
    static bool accepted = false;
    if (!accepted) { accepted = true; RunCaptureOutput(handleExe, L"-accepteula"); }
    int closed = 0;
    std::string raw = RunCaptureOutput(handleExe, L"-p " + std::to_wstring(pid) + L" -a -nobanner");
    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("ROBLOX_singletonEvent") == std::string::npos) continue;
        std::smatch m;
        if (!std::regex_search(line, m, kSingletonRe) || !ProcessAlive(pid)) continue;
        std::string closeOut = RunCaptureOutput(handleExe, L"-c " + Widen(m[1].str()) + L" -p " + std::to_wstring(pid) + L" -y -nobanner");
        std::string lowered = closeOut;
        for (char& c : lowered) c = (char)tolower((unsigned char)c);
        if (lowered.find("error") == std::string::npos && lowered.find("access is denied") == std::string::npos &&
            lowered.find("could not") == std::string::npos) ++closed;
    }
    return closed;
}

static int CloseRobloxSingletonsOnce(const std::wstring& handleExe, const std::vector<DWORD>& pids) {
    std::lock_guard<std::mutex> work(g_handleWorkMutex);
    if (NowMs() < g_quietUntilMs.load()) return 0;
    int closed = 0;
    for (DWORD pid : pids) {
        int n = CloseSingletonEventsIn(pid);
        if (n < 0) n = CloseSingletonsWithHandleExe(handleExe, pid);
        if (n > 0) {
            closed += n;
            Log("[v] Singleton lock released for process " + std::to_string(pid) + " - another instance can open now.");
        }
    }
    return closed;
}

void CloseRobloxSingletonsNow() {
    auto pids = FindPidsByName(L"RobloxPlayerBeta.exe");
    if (pids.empty()) {
        Log("[i] No Roblox instances running - nothing to unlock.");
        return;
    }
    Log("[i] Closing Roblox singleton lock(s) so another instance can launch...");
    CloseRobloxSingletonsOnce(FindHandleExe(), pids);
}

static void WatcherLoop() {
    std::wstring handleExe = FindHandleExe();
    Log("[i] Watching for RobloxPlayerBeta.exe ...");
    int lastCount = -1;
    std::set<DWORD> lastPids;
    std::map<DWORD, int> pending;

    while (watching) {
        auto pids = FindPidsByName(L"RobloxPlayerBeta.exe");

        if ((int)pids.size() != lastCount) {
            lastCount = (int)pids.size();
            instanceCount = lastCount;
            if (!pids.empty()) Log("[i] Found " + std::to_string(pids.size()) + " Roblox process(es)");
            else Log("[i] Roblox closed, waiting...");
        }

        std::set<DWORD> current(pids.begin(), pids.end());
        for (DWORD pid : current)
            if (lastPids.find(pid) == lastPids.end()) pending[pid] = 20;
        for (auto it = pending.begin(); it != pending.end();) {
            if (!current.count(it->first)) { it = pending.erase(it); continue; }
            int closed = CloseRobloxSingletonsOnce(handleExe, { it->first });
            if (closed > 0 || --it->second <= 0) it = pending.erase(it);
            else ++it;
        }
        lastPids.swap(current);

        for (int i = 0; i < 20 && watching; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Log("[i] Watcher stopped.");
}

void StartWatching() {
    if (watching) return;
    watching = true;
    g_watchThread = std::thread(WatcherLoop);
}

void StopWatching() {
    if (!watching) return;
    watching = false;
    if (g_watchThread.joinable()) g_watchThread.join();
}

void LaunchNewInstance() {
    Log("[i] Launching a new Roblox instance...");
    PrepareRobloxInstalls();
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"roblox-player:", nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) Log("[!] Could not launch via roblox-player: protocol. Is Roblox installed?");
    else Log("[v] Launch requested. If the singleton lock blocks it, the watcher will clear it automatically.");
}

void KillAllRobloxInstances() {
    std::lock_guard<std::mutex> work(g_handleWorkMutex);
    g_quietUntilMs = NowMs() + 4000;
    auto pids = FindPidsByName(L"RobloxPlayerBeta.exe");
    if (pids.empty()) {
        Log("[i] No Roblox instances running.");
        return;
    }
    int closed = 0;
    std::vector<HANDLE> dying;
    for (DWORD pid : pids) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (h) {
            if (TerminateProcess(h, 0)) { ++closed; dying.push_back(h); }
            else CloseHandle(h);
        }
    }
    for (size_t i = 0; i < dying.size(); i += MAXIMUM_WAIT_OBJECTS) {
        DWORD n = (DWORD)std::min<size_t>(MAXIMUM_WAIT_OBJECTS, dying.size() - i);
        WaitForMultipleObjects(n, dying.data() + i, TRUE, 6000);
    }
    for (HANDLE h : dying) CloseHandle(h);
    g_quietUntilMs = NowMs() + 2500;
    Log("[v] Closed " + std::to_string(closed) + " of " + std::to_string(pids.size()) + " Roblox instance(s).");
    if (!g_ownsRobloxMutex.load())
        Log("[i] Waiting to take back ROBLOX_singletonMutex so new launches can run side by side...");
}

std::atomic<bool> uiForeground{ true };

int CountRobloxProcesses(bool force) {
    static int cached = 0;
    static auto lastCheck = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    auto now = std::chrono::steady_clock::now();
    auto interval = uiForeground.load() ? std::chrono::milliseconds(800) : std::chrono::milliseconds(5000);
    if (force || now - lastCheck > interval) {
        cached = (int)FindPidsByName(L"RobloxPlayerBeta.exe").size();
        lastCheck = now;
    }
    return cached;
}

float GetCpuUsagePercent() {
    static bool first = true;
    static ULARGE_INTEGER lastIdle{}, lastKernel{}, lastUser{};
    static float cached = 0.0f;
    static auto lastCheck = std::chrono::steady_clock::now() - std::chrono::seconds(2);

    auto now = std::chrono::steady_clock::now();
    if (!first && now - lastCheck < std::chrono::milliseconds(800)) return cached;

    FILETIME idleFt, kernelFt, userFt;
    if (!GetSystemTimes(&idleFt, &kernelFt, &userFt)) return cached;
    lastCheck = now;

    ULARGE_INTEGER idle, kernel, user;
    idle.LowPart = idleFt.dwLowDateTime; idle.HighPart = idleFt.dwHighDateTime;
    kernel.LowPart = kernelFt.dwLowDateTime; kernel.HighPart = kernelFt.dwHighDateTime;
    user.LowPart = userFt.dwLowDateTime; user.HighPart = userFt.dwHighDateTime;

    if (!first) {
        ULONGLONG total = (kernel.QuadPart - lastKernel.QuadPart) + (user.QuadPart - lastUser.QuadPart);
        ULONGLONG idleDelta = idle.QuadPart - lastIdle.QuadPart;
        if (total > 0) cached = (float)(total - idleDelta) * 100.0f / (float)total;
    }
    first = false;
    lastIdle = idle; lastKernel = kernel; lastUser = user;
    return cached;
}

float GetMemoryUsagePercent() {
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (!GlobalMemoryStatusEx(&mem)) return 0.0f;
    return (float)mem.dwMemoryLoad;
}

std::string GetUptimeString() {
    long long secs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - g_startTime).count();
    long long h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", h, m, s);
    return buf;
}

typedef struct sqlite3 sqlite3;
typedef int  (__cdecl* pfn_sqlite3_open)(const char*, sqlite3**);
typedef int  (__cdecl* pfn_sqlite3_close)(sqlite3*);
typedef int  (__cdecl* pfn_sqlite3_exec)(sqlite3*, const char*, int(*)(void*, int, char**, char**), void*, char**);
typedef void (__cdecl* pfn_sqlite3_free)(void*);
typedef int  (__cdecl* pfn_sqlite3_changes)(sqlite3*);
typedef const char* (__cdecl* pfn_sqlite3_errmsg)(sqlite3*);

struct SqliteDynApi {
    HMODULE hmod = nullptr;
    pfn_sqlite3_open open = nullptr;
    pfn_sqlite3_close close = nullptr;
    pfn_sqlite3_exec exec = nullptr;
    pfn_sqlite3_free free_fn = nullptr;
    pfn_sqlite3_changes changes = nullptr;
    pfn_sqlite3_errmsg errmsg = nullptr;
};
static thread_local std::string g_lastCookieError;

static SqliteDynApi loadSqliteDynApi() {
    SqliteDynApi api;
    auto bindSymbols = [&](HMODULE mod) -> bool {
        api.hmod = mod;
        api.open = reinterpret_cast<pfn_sqlite3_open>(GetProcAddress(api.hmod, "sqlite3_open"));
        api.close = reinterpret_cast<pfn_sqlite3_close>(GetProcAddress(api.hmod, "sqlite3_close"));
        api.exec = reinterpret_cast<pfn_sqlite3_exec>(GetProcAddress(api.hmod, "sqlite3_exec"));
        api.free_fn = reinterpret_cast<pfn_sqlite3_free>(GetProcAddress(api.hmod, "sqlite3_free"));
        api.changes = reinterpret_cast<pfn_sqlite3_changes>(GetProcAddress(api.hmod, "sqlite3_changes"));
        api.errmsg = reinterpret_cast<pfn_sqlite3_errmsg>(GetProcAddress(api.hmod, "sqlite3_errmsg"));
        return api.open && api.close && api.exec && api.free_fn && api.changes && api.errmsg;
        };

    const char* directDlls[] = { "winsqlite3.dll", "sqlite3.dll", "mozsqlite3.dll" };
    for (const char* dll : directDlls) {
        HMODULE mod = LoadLibraryA(dll);
        if (mod && bindSymbols(mod)) return api;
        if (mod) FreeLibrary(mod);
    }

    std::vector<std::filesystem::path> roots;
    if (const char* p = std::getenv("LOCALAPPDATA")) {
        roots.emplace_back(std::string(p) + "\\Google\\Chrome\\Application");
        roots.emplace_back(std::string(p) + "\\Microsoft\\Edge\\Application");
        roots.emplace_back(std::string(p) + "\\Mozilla Firefox");
    }
    if (const char* p = std::getenv("ProgramFiles")) {
        roots.emplace_back(std::string(p) + "\\Google\\Chrome\\Application");
        roots.emplace_back(std::string(p) + "\\Microsoft\\Edge\\Application");
        roots.emplace_back(std::string(p) + "\\Mozilla Firefox");
    }
    if (const char* p = std::getenv("ProgramFiles(x86)")) {
        roots.emplace_back(std::string(p) + "\\Google\\Chrome\\Application");
        roots.emplace_back(std::string(p) + "\\Microsoft\\Edge\\Application");
        roots.emplace_back(std::string(p) + "\\Mozilla Firefox");
    }

    for (const auto& root : roots) {
        if (!std::filesystem::exists(root)) continue;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
            it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string fname = it->path().filename().string();
            if (fname != "sqlite3.dll" && fname != "mozsqlite3.dll") continue;
            HMODULE mod = LoadLibraryA(it->path().string().c_str());
            if (!mod) continue;
            if (bindSymbols(mod)) return api;
            FreeLibrary(mod);
        }
    }
    return {};
}

static std::vector<std::filesystem::path> findFiles(const std::filesystem::path& root, const std::string& filename) {
    std::vector<std::filesystem::path> results;
    if (!std::filesystem::exists(root)) return results;
    std::error_code ec;
    for (auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename().string() == filename)
            results.push_back(entry.path());
    }
    return results;
}

static void copyDbWithWalSidecars(const std::filesystem::path& srcMain, const std::filesystem::path& dstMain) {
    std::error_code ec;
    for (const char* suffix : { "-wal", "-shm" }) {
        std::filesystem::path src = srcMain.string() + suffix;
        if (std::filesystem::exists(src, ec)) {
            std::filesystem::copy_file(src, dstMain.string() + suffix, std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
}

static void removeDbWithWalSidecars(const std::filesystem::path& mainPath) {
    std::error_code ec;
    std::filesystem::remove(mainPath, ec);
    std::filesystem::remove(mainPath.string() + "-wal", ec);
    std::filesystem::remove(mainPath.string() + "-shm", ec);
}

static int countRobloxRows(const std::filesystem::path& dbPath, bool chromium) {
    static const int SQLITE_OK = 0;
    static SqliteDynApi sq = loadSqliteDynApi();
    if (!sq.hmod) return -1;

    std::filesystem::path tmp = dbPath.string() + ".multitool_scan";
    std::error_code ec;
    std::filesystem::copy_file(dbPath, tmp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return -1;
    copyDbWithWalSidecars(dbPath, tmp);

    sqlite3* db = nullptr;
    if (sq.open(tmp.string().c_str(), &db) != SQLITE_OK) {
        if (db) sq.close(db);
        removeDbWithWalSidecars(tmp);
        return -1;
    }

    const char* sql = chromium
        ? "SELECT COUNT(*) FROM cookies WHERE host_key = 'roblox.com' OR host_key = '.roblox.com' OR host_key LIKE '%.roblox.com';"
        : "SELECT COUNT(*) FROM moz_cookies WHERE host = 'roblox.com' OR host = '.roblox.com' OR host LIKE '%.roblox.com' OR baseDomain = 'roblox.com';";

    int count = -1;
    auto callback = [](void* userdata, int argc, char** argv, char**) -> int {
        if (argc > 0 && argv[0]) *static_cast<int*>(userdata) = std::atoi(argv[0]);
        return 0;
        };
    char* errMsg = nullptr;
    int rc = sq.exec(db, sql, callback, &count, &errMsg);
    if (!chromium && rc != SQLITE_OK && errMsg && std::string(errMsg).find("no such column: baseDomain") != std::string::npos) {
        sq.free_fn(errMsg);
        errMsg = nullptr;
        const char* fallback = "SELECT COUNT(*) FROM moz_cookies WHERE host = 'roblox.com' OR host = '.roblox.com' OR host LIKE '%.roblox.com';";
        rc = sq.exec(db, fallback, callback, &count, &errMsg);
    }
    if (errMsg) sq.free_fn(errMsg);
    sq.close(db);
    removeDbWithWalSidecars(tmp);
    return rc == SQLITE_OK ? count : -1;
}

static int deleteRobloxRows(const std::filesystem::path& dbPath, bool chromium) {
    static const int SQLITE_OK = 0;
    static SqliteDynApi sq = loadSqliteDynApi();
    g_lastCookieError.clear();
    if (!sq.hmod) {
        g_lastCookieError = "SQLite runtime not found.";
        return -1;
    }

    std::filesystem::path tmp = dbPath.string() + ".multitool_tmp";
    std::error_code ec;
    std::filesystem::copy_file(dbPath, tmp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        g_lastCookieError = "copy_file failed: " + ec.message();
        return -1;
    }
    copyDbWithWalSidecars(dbPath, tmp);

    sqlite3* db = nullptr;
    if (sq.open(tmp.string().c_str(), &db) != SQLITE_OK) {
        if (db) sq.close(db);
        removeDbWithWalSidecars(tmp);
        return -1;
    }

    sq.exec(db, "PRAGMA journal_mode=DELETE;", nullptr, nullptr, nullptr);

    const char* sql = chromium
        ? "DELETE FROM cookies WHERE host_key = 'roblox.com' OR host_key = '.roblox.com' OR host_key LIKE '%.roblox.com';"
        : "DELETE FROM moz_cookies WHERE host = 'roblox.com' OR host = '.roblox.com' OR host LIKE '%.roblox.com' OR baseDomain = 'roblox.com';";

    char* errMsg = nullptr;
    int rc = sq.exec(db, sql, nullptr, nullptr, &errMsg);
    if (!chromium && rc != SQLITE_OK && errMsg && std::string(errMsg).find("no such column: baseDomain") != std::string::npos) {
        sq.free_fn(errMsg);
        errMsg = nullptr;
        const char* fallback = "DELETE FROM moz_cookies WHERE host = 'roblox.com' OR host = '.roblox.com' OR host LIKE '%.roblox.com';";
        rc = sq.exec(db, fallback, nullptr, nullptr, &errMsg);
    }
    if (errMsg) sq.free_fn(errMsg);

    int rows = -1;
    if (rc == SQLITE_OK) rows = sq.changes(db);
    sq.close(db);

    if (rows >= 0) {
        std::filesystem::rename(tmp, dbPath, ec);
        if (ec) {
            std::filesystem::copy_file(tmp, dbPath, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tmp, ec);
        }
        std::error_code ignored;
        std::filesystem::remove(dbPath.string() + "-wal", ignored);
        std::filesystem::remove(dbPath.string() + "-shm", ignored);
    } else {
        removeDbWithWalSidecars(tmp);
    }
    return rows;
}

void ClearBrowserCookies() {
    const char* localAppData = std::getenv("LOCALAPPDATA");
    const char* appData = std::getenv("APPDATA");
    if (!localAppData || !appData) {
        Log("[!] Could not read LOCALAPPDATA/APPDATA environment variables.");
        return;
    }

    Log("[i] Closing browsers to release cookie database locks...");
    RunCaptureOutput(L"taskkill", L"/F /T /IM chrome.exe /IM msedge.exe /IM firefox.exe /IM opera.exe /IM opera_gx.exe /IM crashpad_handler.exe");

    struct ChromiumBrowser { std::string name, profileRoot; };
    std::vector<ChromiumBrowser> chromiumBrowsers = {
        { "Google Chrome",  std::string(localAppData) + "\\Google\\Chrome\\User Data" },
        { "Microsoft Edge", std::string(localAppData) + "\\Microsoft\\Edge\\User Data" },
        { "Opera GX",       std::string(appData) + "\\Opera Software\\Opera GX Stable" },
        { "Opera",          std::string(appData) + "\\Opera Software\\Opera Stable" },
    };

    for (auto& browser : chromiumBrowsers) {
        if (!std::filesystem::exists(browser.profileRoot)) continue;
        auto cookieFiles = findFiles(browser.profileRoot, "Cookies");
        if (cookieFiles.empty()) {
            Log("[!] " + browser.name + ": no Cookies DB found.");
            continue;
        }
        int totalRows = 0, totalFailed = 0;
        for (auto& cf : cookieFiles) {
            int r = deleteRobloxRows(cf, true);
            if (r >= 0) totalRows += r;
            else ++totalFailed;
        }
        if (totalFailed == 0) Log("[v] " + browser.name + ": roblox.com cookies cleared (" + std::to_string(totalRows) + " rows).");
        else Log("[!] " + browser.name + ": " + std::to_string(totalFailed) + " profile(s) failed - close the browser and retry.");
    }

    std::filesystem::path ffProfiles(std::string(appData) + "\\Mozilla\\Firefox\\Profiles");
    if (std::filesystem::exists(ffProfiles)) {
        auto cookieFiles = findFiles(ffProfiles, "cookies.sqlite");
        if (cookieFiles.empty()) {
            Log("[!] Firefox: no cookies.sqlite found.");
        } else {
            int totalRows = 0, totalFailed = 0;
            for (auto& cf : cookieFiles) {
                int r = deleteRobloxRows(cf, false);
                if (r >= 0) totalRows += r;
                else ++totalFailed;
            }
            if (totalFailed == 0) Log("[v] Firefox: roblox.com cookies cleared (" + std::to_string(totalRows) + " rows).");
            else Log("[!] Firefox: " + std::to_string(totalFailed) + " profile(s) failed - close Firefox and retry.");
        }
    }
}

void ScanBrowserCookies() {
    browserCookieScanning = true;

    const char* localAppData = std::getenv("LOCALAPPDATA");
    const char* appData = std::getenv("APPDATA");

    std::vector<BrowserCookieStatus> results;

    struct ChromiumBrowser { std::string name, profileRoot; };
    std::vector<ChromiumBrowser> chromiumBrowsers;
    if (localAppData) {
        chromiumBrowsers.push_back({ "Google Chrome",  std::string(localAppData) + "\\Google\\Chrome\\User Data" });
        chromiumBrowsers.push_back({ "Microsoft Edge", std::string(localAppData) + "\\Microsoft\\Edge\\User Data" });
    }
    if (appData) {
        chromiumBrowsers.push_back({ "Opera",    std::string(appData) + "\\Opera Software\\Opera Stable" });
        chromiumBrowsers.push_back({ "Opera GX", std::string(appData) + "\\Opera Software\\Opera GX Stable" });
    }

    for (auto& browser : chromiumBrowsers) {
        BrowserCookieStatus status;
        status.name = browser.name;
        status.installed = std::filesystem::exists(browser.profileRoot);
        if (status.installed) {
            auto cookieFiles = findFiles(browser.profileRoot, "Cookies");
            if (cookieFiles.empty()) {
                status.scanFailed = true;
                Log("[!] " + browser.name + ": profile found but no Cookies DB located under " + browser.profileRoot);
            }
            for (auto& cf : cookieFiles) {
                int n = countRobloxRows(cf, true);
                if (n > 0) status.count += n;
                else if (n < 0) {
                    status.scanFailed = true;
                    Log("[!] " + browser.name + ": could not read " + cf.string() + " (locked, missing sqlite runtime, or copy failed).");
                }
            }
            status.found = status.count > 0;
        }
        results.push_back(status);
    }

    {
        BrowserCookieStatus status;
        status.name = "Firefox";
        if (appData) {
            std::filesystem::path ffProfiles(std::string(appData) + "\\Mozilla\\Firefox\\Profiles");
            status.installed = std::filesystem::exists(ffProfiles);
            if (status.installed) {
                auto cookieFiles = findFiles(ffProfiles, "cookies.sqlite");
                if (cookieFiles.empty()) {
                    status.scanFailed = true;
                    Log("[!] Firefox: profile found but no cookies.sqlite located under " + ffProfiles.string());
                }
                for (auto& cf : cookieFiles) {
                    int n = countRobloxRows(cf, false);
                    if (n > 0) status.count += n;
                    else if (n < 0) {
                        status.scanFailed = true;
                        Log("[!] Firefox: could not read " + cf.string() + " (locked, missing sqlite runtime, or copy failed).");
                    }
                }
                status.found = status.count > 0;
            }
        }
        results.push_back(status);
    }

    {
        std::lock_guard<std::mutex> lock(browserCookieMutex);
        browserCookieStatus = std::move(results);
    }
    browserCookieScanned = true;
    browserCookieScanning = false;
}

void ClearRobloxCookieFile() {
    ScrubAndLockRobloxCookieFile("manual cleanup");
}

bool RobloxCookieFileHasData() {
    std::lock_guard<std::mutex> lock(g_robloxCookieFileMutex);
    if (g_robloxCookieFileHandle != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size = {};
        return GetFileSizeEx(g_robloxCookieFileHandle, &size) && size.QuadPart > 0;
    }

    std::wstring path = RobloxCookieFilePath();
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec && std::filesystem::file_size(path, ec) > 0 && !ec;
}

static void ClearBootstrapperCookieFiles() {
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData) return;

    for (const char* launcher : { "Bloxstrap", "Fishstrap" }) {
        std::filesystem::path root = std::filesystem::path(localAppData) / launcher;
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;

        int wiped = 0;
        for (const auto& datPath : findFiles(root, "RobloxCookies.dat")) {
            std::ofstream f(datPath, std::ios::binary | std::ios::trunc);
            if (f) ++wiped;
        }
        if (wiped > 0) Log("[v] " + std::string(launcher) + ": cleared " + std::to_string(wiped) + " RobloxCookies.dat file(s).");
        else Log("[i] " + std::string(launcher) + ": installed, no RobloxCookies.dat found.");
    }
}

void ClearRobloxCookieFiles() {
    Log("[i] Clearing Roblox cookie files...");
    ClearRobloxCookieFile();
    ClearBootstrapperCookieFiles();
    Log("[v] Roblox cookies cleared.");
}

static std::string generateRandomMac() {
    std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> dist(0x00, 0xFF);
    std::ostringstream mac;
    mac << std::hex << std::uppercase << std::setfill('0');
    mac << std::setw(2) << 0x02;
    for (int i = 1; i < 6; ++i) mac << std::setw(2) << dist(rng);
    return mac.str();
}

static std::string regQueryString(HKEY hKey, const std::string& valueName) {
    char buffer[512];
    DWORD bufferSize = sizeof(buffer);
    DWORD type = REG_SZ;
    if (RegQueryValueExA(hKey, valueName.c_str(), nullptr, &type, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS) {
        return std::string(buffer);
    }
    return "";
}

static std::string formatMacDisplay(const BYTE* bytes) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');
    for (int i = 0; i < 6; ++i) {
        if (i > 0) ss << ":";
        ss << std::setw(2) << (int)bytes[i];
    }
    return ss.str();
}

static std::string getCurrentMac(const std::string& adapterId) {
    std::string regPath = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}\\" + adapterId;
    HKEY key;
    std::string netCfgId;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
        netCfgId = regQueryString(key, "NetCfgInstanceID");
        RegCloseKey(key);
    }
    if (netCfgId.empty()) return "(unknown)";

    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);
    if (GetAdaptersInfo(reinterpret_cast<IP_ADAPTER_INFO*>(buf.data()), &bufLen) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
    }
    if (GetAdaptersInfo(reinterpret_cast<IP_ADAPTER_INFO*>(buf.data()), &bufLen) == NO_ERROR) {
        IP_ADAPTER_INFO* adapter = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
        while (adapter) {
            std::string adapterGuid = adapter->AdapterName;
            auto strip = [](std::string s) {
                if (!s.empty() && s.front() == '{') s = s.substr(1);
                if (!s.empty() && s.back() == '}') s = s.substr(0, s.size() - 1);
                for (auto& c : s) c = (char)toupper((unsigned char)c);
                return s;
                };
            if (strip(adapterGuid) == strip(netCfgId)) return formatMacDisplay(adapter->Address);
            adapter = adapter->Next;
        }
    }
    return "(could not read)";
}

static int getAdapterIfIndex(const std::string& adapterId) {
    std::string regPath = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}\\" + adapterId;
    HKEY key;
    std::string netCfgId;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
        netCfgId = regQueryString(key, "NetCfgInstanceID");
        RegCloseKey(key);
    }
    if (netCfgId.empty()) return -1;

    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);
    if (GetAdaptersInfo(reinterpret_cast<IP_ADAPTER_INFO*>(buf.data()), &bufLen) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
    }
    if (GetAdaptersInfo(reinterpret_cast<IP_ADAPTER_INFO*>(buf.data()), &bufLen) == NO_ERROR) {
        IP_ADAPTER_INFO* adapter = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
        while (adapter) {
            std::string adapterGuid = adapter->AdapterName;
            auto strip = [](std::string s) {
                if (!s.empty() && s.front() == '{') s = s.substr(1);
                if (!s.empty() && s.back() == '}') s = s.substr(0, s.size() - 1);
                for (auto& c : s) c = (char)toupper((unsigned char)c);
                return s;
                };
            if (strip(adapterGuid) == strip(netCfgId)) return (int)adapter->Index;
            adapter = adapter->Next;
        }
    }
    return -1;
}

static int getDefaultRouteIfIndex() {
    DWORD bestIfIndex = 0;
    IPAddr dest = 0x08080808;
    if (GetBestInterface(dest, &bestIfIndex) != NO_ERROR) return -1;
    return (int)bestIfIndex;
}

static std::vector<NetworkAdapterInfo> listNetworkAdapters() {
    std::vector<NetworkAdapterInfo> result;
    const std::string classKeyPath = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";

    HKEY classKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, classKeyPath.c_str(), 0, KEY_READ, &classKey) != ERROR_SUCCESS) return result;

    for (int i = 0; ; ++i) {
        std::ostringstream subkeyName;
        subkeyName << std::setfill('0') << std::setw(4) << i;

        HKEY adapterKey;
        if (RegOpenKeyExA(classKey, subkeyName.str().c_str(), 0, KEY_READ, &adapterKey) != ERROR_SUCCESS) break;

        std::string driverDesc = regQueryString(adapterKey, "DriverDesc");
        std::string netCfgId = regQueryString(adapterKey, "NetCfgInstanceID");

        if (!driverDesc.empty() && !netCfgId.empty()) {
            std::string connNamePath =
                "SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\" +
                netCfgId + "\\Connection";

            std::string connectionName = driverDesc;
            HKEY connKey;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, connNamePath.c_str(), 0, KEY_READ, &connKey) == ERROR_SUCCESS) {
                std::string name = regQueryString(connKey, "Name");
                if (!name.empty()) connectionName = name;
                RegCloseKey(connKey);
            }

            std::string lower = driverDesc;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);

            bool skip = false;
            for (const char* kw : { "virtual", "loopback", "bluetooth", "wan miniport", "tap-windows", "pseudo" }) {
                if (lower.find(kw) != std::string::npos) { skip = true; break; }
            }
            if (!skip) {
                NetworkAdapterInfo info;
                info.id = subkeyName.str();
                info.description = driverDesc;
                info.connectionName = connectionName;
                info.currentMac = getCurrentMac(info.id);
                result.push_back(info);
            }
        }
        RegCloseKey(adapterKey);
    }
    RegCloseKey(classKey);

    int defaultIfIndex = getDefaultRouteIfIndex();
    if (defaultIfIndex != -1) {
        for (auto& info : result) {
            if (getAdapterIfIndex(info.id) == defaultIfIndex) info.isActive = true;
        }
    }
    return result;
}

static void changeMacAddress(const std::string& adapterId, const std::string& macAddress) {
    std::string path = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}\\" + adapterId;
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_WRITE, &key) != ERROR_SUCCESS) {
        throw std::runtime_error("Failed to open registry key for writing.");
    }
    LONG result = RegSetValueExA(key, "NetworkAddress", 0, REG_SZ,
        (const BYTE*)macAddress.c_str(), (DWORD)(macAddress.size() + 1));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) throw std::runtime_error("Failed to write NetworkAddress to registry.");
}

static void clearMacAddressOverride(const std::string& adapterId) {
    std::string path = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}\\" + adapterId;
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_WRITE, &key) != ERROR_SUCCESS) {
        throw std::runtime_error("Failed to open registry key for writing.");
    }
    RegDeleteValueA(key, "NetworkAddress");
    RegCloseKey(key);
}

static void restartNetworkAdapter(const std::string& connectionName) {
    std::string disableCmd = "netsh interface set interface \"" + connectionName + "\" admin=disable";
    if (std::system(disableCmd.c_str()) != 0) throw std::runtime_error("Failed to disable network adapter.");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::string enableCmd = "netsh interface set interface \"" + connectionName + "\" admin=enable";
    if (std::system(enableCmd.c_str()) != 0) throw std::runtime_error("Failed to enable network adapter.");
}

void RefreshAdapters() {
    auto fresh = listNetworkAdapters();
    int activeIdx = -1;
    for (int i = 0; i < (int)fresh.size(); ++i) {
        if (fresh[i].isActive) { activeIdx = i; break; }
    }
    std::lock_guard<std::mutex> lock(adaptersMutex);
    adapters = std::move(fresh);
    defaultAdapterIndex = activeIdx;
    if (adapters.empty()) Log("[!] No usable network adapters found.");
}

void SpoofAdapter(int index) {
    NetworkAdapterInfo adapter;
    {
        std::lock_guard<std::mutex> lock(adaptersMutex);
        if (index < 0 || index >= (int)adapters.size()) {
            Log("[!] Select an adapter from the list first.");
            return;
        }
        adapter = adapters[index];
    }
    try {
        std::string oldMac = getCurrentMac(adapter.id);
        std::string newMac = generateRandomMac();
        changeMacAddress(adapter.id, newMac);
        Log("[i] Restarting adapter \"" + adapter.connectionName + "\"...");
        restartNetworkAdapter(adapter.connectionName);
        Log("[v] MAC spoofed: " + oldMac + " -> " + getCurrentMac(adapter.id));
    } catch (const std::exception& e) {
        Log(std::string("[!] Error spoofing MAC: ") + e.what());
    }
    RefreshAdapters();
}

void RestoreAdapter(int index) {
    NetworkAdapterInfo adapter;
    {
        std::lock_guard<std::mutex> lock(adaptersMutex);
        if (index < 0 || index >= (int)adapters.size()) {
            Log("[!] Select an adapter from the list first.");
            return;
        }
        adapter = adapters[index];
    }
    try {
        clearMacAddressOverride(adapter.id);
        Log("[i] Restarting adapter \"" + adapter.connectionName + "\"...");
        restartNetworkAdapter(adapter.connectionName);
        Log("[v] MAC restored to hardware default: " + getCurrentMac(adapter.id));
    } catch (const std::exception& e) {
        Log(std::string("[!] Error restoring MAC: ") + e.what());
    }
    RefreshAdapters();
}

std::mutex accountsMutex;
std::vector<RobloxAccount> accounts;

std::mutex launchedMutex;
std::map<long long, unsigned long> launchedPids;

static std::map<long long, std::string> launchedTrackers;

static std::wstring GetProcessCommandLine(DWORD pid) {
    using NtQIP = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static NtQIP query = (NtQIP)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    if (!query) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    std::wstring out;
    ULONG need = 0;
    const ULONG ProcessCommandLineInformation = 60;
    query(h, ProcessCommandLineInformation, nullptr, 0, &need);
    if (need > 0 && need < (1u << 20)) {
        std::vector<BYTE> buf(need);
        if (query(h, ProcessCommandLineInformation, buf.data(), need, &need) >= 0) {
            struct UStr { USHORT Length, MaximumLength; PWSTR Buffer; };
            auto* s = reinterpret_cast<UStr*>(buf.data());
            if (s->Buffer && s->Length) out.assign(s->Buffer, s->Length / sizeof(wchar_t));
        }
    }
    CloseHandle(h);
    return out;
}

static std::map<long long, std::chrono::steady_clock::time_point> launchedAt;

static std::map<DWORD, DWORD> RobloxParents() {
    std::map<DWORD, DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"RobloxPlayerBeta.exe") == 0) out[pe.th32ProcessID] = pe.th32ParentProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

void PruneLaunchedPids() {
    auto parents = RobloxParents();
    std::vector<DWORD> alive;
    for (auto& kv : parents) alive.push_back(kv.first);
    std::set<DWORD> aliveSet(alive.begin(), alive.end());

    static std::map<DWORD, std::wstring> cmdCache;
    for (auto it = cmdCache.begin(); it != cmdCache.end();) {
        if (!aliveSet.count(it->first)) it = cmdCache.erase(it);
        else ++it;
    }

    std::lock_guard<std::mutex> lk(launchedMutex);
    std::set<DWORD> claimed;
    for (auto& kv : launchedPids) if (aliveSet.count((DWORD)kv.second)) claimed.insert((DWORD)kv.second);

    for (auto it = launchedPids.begin(); it != launchedPids.end();) {
        DWORD pid = (DWORD)it->second;
        if (aliveSet.count(pid)) { ++it; continue; }
        DWORD heir = 0;
        for (DWORD p : alive) {
            if (claimed.count(p)) continue;
            DWORD up = parents[p];
            for (int depth = 0; depth < 3 && up; ++depth) {
                if (up == pid) { heir = p; break; }
                auto pp = parents.find(up);
                up = pp == parents.end() ? 0 : pp->second;
            }
            if (heir) break;
        }
        if (heir) { it->second = heir; claimed.insert(heir); ++it; }
        else it = launchedPids.erase(it);
    }

    std::set<DWORD> taken;
    for (auto& kv : launchedPids) taken.insert((DWORD)kv.second);
    for (auto& kv : launchedTrackers) {
        if (launchedPids.count(kv.first) || kv.second.empty()) continue;
        std::wstring tracker(kv.second.begin(), kv.second.end());
        for (DWORD pid : alive) {
            if (taken.count(pid)) continue;
            auto c = cmdCache.find(pid);
            if (c == cmdCache.end()) {
                std::wstring cmd = GetProcessCommandLine(pid);
                if (cmd.empty()) continue;
                c = cmdCache.emplace(pid, std::move(cmd)).first;
            }
            if (c->second.find(tracker) != std::wstring::npos) {
                launchedPids[kv.first] = pid;
                taken.insert(pid);
                break;
            }
        }
    }

    auto now = std::chrono::steady_clock::now();
    std::vector<long long> missing;
    for (auto& kv : launchedAt)
        if (!launchedPids.count(kv.first) && now - kv.second < std::chrono::minutes(3)) missing.push_back(kv.first);
    if (missing.size() == 1) {
        std::vector<DWORD> spare;
        for (DWORD pid : alive) {
            if (taken.count(pid)) continue;
            auto c = cmdCache.find(pid);
            if (c == cmdCache.end()) {
                std::wstring cmd = GetProcessCommandLine(pid);
                if (!cmd.empty()) c = cmdCache.emplace(pid, std::move(cmd)).first;
            }
            if (c != cmdCache.end() && c->second.find(L"--launch-to-tray") != std::wstring::npos) continue;
            spare.push_back(pid);
        }
        if (spare.size() == 1) launchedPids[missing[0]] = spare[0];
    }
}

static void AssignLaunchedPid(long long userId, std::set<DWORD> before) {
    std::wstring tracker;
    {
        std::lock_guard<std::mutex> lk(launchedMutex);
        auto t = launchedTrackers.find(userId);
        if (t != launchedTrackers.end()) tracker.assign(t->second.begin(), t->second.end());
    }
    for (int i = 0; i < 24; ++i) {
        auto now = FindPidsByName(L"RobloxPlayerBeta.exe");
        DWORD found = 0;
        std::set<DWORD> owned;
        {
            std::lock_guard<std::mutex> lk(launchedMutex);
            for (auto& kv : launchedPids) if (kv.first != userId) owned.insert((DWORD)kv.second);
        }
        for (DWORD p : now) {
            if (before.count(p) || owned.count(p)) continue;
            std::wstring cmd = tracker.empty() ? L"" : GetProcessCommandLine(p);
            if (!cmd.empty() && cmd.find(tracker) == std::wstring::npos) continue;
            found = p;
            break;
        }
        if (found) {
            std::lock_guard<std::mutex> lk(launchedMutex);
            if (!launchedPids.count(userId)) launchedPids[userId] = found;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

std::mutex placeInfoMutex;
PlaceInfo placeInfo;

std::mutex activityMutex;
std::vector<ActivityEntry> activityLog;

std::mutex systemStatusMutex;
SystemStatus systemStatus;

std::atomic<long long> savedPlaceId{ 0 };

struct HttpResponse {
    int status = 0;
    std::string body;
    bool ok = false;
};

static std::wstring QueryHeader(HINTERNET hRequest, const wchar_t* headerName) {
    DWORD size = 0;
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM, headerName, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (size == 0) return L"";
    std::wstring buf;
    buf.resize(size / sizeof(wchar_t));
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CUSTOM, headerName, buf.data(), &size, WINHTTP_NO_HEADER_INDEX)) return L"";
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return buf;
}

static std::wstring QueryRawHeaders(HINTERNET hRequest) {
    DWORD size = 0;
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (size == 0) return L"";

    std::wstring buf;
    buf.resize(size / sizeof(wchar_t));
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        buf.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return L"";
    }
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return buf;
}

static std::string ExtractSecurityCookieFromHeaders(const std::wstring& headers) {
    const std::wstring needle = L".ROBLOSECURITY=";
    size_t pos = headers.find(needle);
    if (pos == std::wstring::npos) return "";
    pos += needle.size();

    size_t end = headers.find(L';', pos);
    if (end == std::wstring::npos) end = headers.find(L"\r\n", pos);
    if (end == std::wstring::npos) end = headers.size();

    std::wstring value = headers.substr(pos, end - pos);
    return std::string(value.begin(), value.end());
}

static bool LooksLikeRobloxSecurityCookie(const std::string& value) {
    return value.size() > 100 && value.find("WARNING:-DO-NOT-SHARE-THIS") != std::string::npos;
}

static HttpResponse HttpRequest(const std::wstring& host, const std::wstring& path, const std::wstring& method,
    const std::string& cookie, const std::vector<std::pair<std::wstring, std::wstring>>& extraHeaders,
    const std::string& body, std::wstring* outCsrf = nullptr, std::wstring* outAuthTicket = nullptr,
    std::string* outSecurityCookie = nullptr) {
    HttpResponse result;

    HINTERNET hSession = WinHttpOpen(L"VelsMultiTool/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    std::wstring headerBlock = L"Cookie: .ROBLOSECURITY=" + std::wstring(cookie.begin(), cookie.end()) + L"\r\n";
    for (auto& h : extraHeaders) headerBlock += h.first + L": " + h.second + L"\r\n";
    WinHttpAddRequestHeaders(hRequest, headerBlock.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL sendOk = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.empty() ? nullptr : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);

    if (sendOk && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0, size = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX);
        result.status = (int)statusCode;
        result.ok = true;

        if (outCsrf) *outCsrf = QueryHeader(hRequest, L"x-csrf-token");
        if (outAuthTicket) *outAuthTicket = QueryHeader(hRequest, L"rbx-authentication-ticket");
        if (outSecurityCookie) *outSecurityCookie = ExtractSecurityCookieFromHeaders(QueryRawHeaders(hRequest));

        DWORD available = 0;
        while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
            std::vector<char> buf(available);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, buf.data(), available, &read)) break;
            result.body.append(buf.data(), read);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

static std::string ExtractJsonStringField(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\":\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return "";
    pos += pat.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static long long ExtractJsonLongField(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\":";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return 0;
    pos += pat.size();
    try { return std::stoll(json.substr(pos)); } catch (...) { return 0; }
}

static std::string UrlEncode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << (char)c;
        } else {
            out << '%' << std::setw(2) << (int)c;
        }
    }
    return out.str();
}

static std::string GetAuthTicket(const std::string& cookie) {
    std::wstring csrf;
    HttpResponse csrfResp = HttpRequest(L"auth.roblox.com", L"/v1/authentication-ticket/", L"POST", cookie,
        { {L"Referer", L"https://www.roblox.com/"}, {L"Content-Type", L"application/x-www-form-urlencoded"} }, "", &csrf, nullptr);

    if (csrf.empty()) {
        Log("[!] Could not obtain a CSRF token (status " + std::to_string(csrfResp.status) + ") - the cookie may be invalid or expired.");
        return "";
    }

    std::wstring ticket;
    HttpResponse ticketResp = HttpRequest(L"auth.roblox.com", L"/v1/authentication-ticket/", L"POST", cookie,
        { {L"Referer", L"https://www.roblox.com/"}, {L"X-CSRF-TOKEN", csrf}, {L"Content-Type", L"application/x-www-form-urlencoded"} }, "", nullptr, &ticket);

    if (ticket.empty()) {
        Log("[!] Auth ticket request failed (status " + std::to_string(ticketResp.status) + ").");
    }

    return std::string(ticket.begin(), ticket.end());
}

static bool RefreshAccountCookieInPlace(RobloxAccount& account) {
    std::wstring csrf;
    HttpRequest(L"auth.roblox.com", L"/v1/authentication-ticket/", L"POST", account.cookie,
        { {L"Referer", L"https://www.roblox.com/"} }, "", &csrf, nullptr);

    if (csrf.empty()) {
        Log("[!] Could not refresh " + account.username + ": failed to get CSRF token.");
        return false;
    }

    std::string newCookie;
    HttpResponse resp = HttpRequest(L"www.roblox.com", L"/authentication/signoutfromallsessionsandreauthenticate",
        L"POST", account.cookie,
        { {L"Referer", L"https://www.roblox.com/"}, {L"X-CSRF-TOKEN", csrf}, {L"Content-Type", L"application/x-www-form-urlencoded"} },
        "", nullptr, nullptr, &newCookie);

    if (!resp.ok || resp.status != 200 || !LooksLikeRobloxSecurityCookie(newCookie)) {
        Log("[!] Could not refresh " + account.username + "'s saved cookie before launch.");
        return false;
    }

    account.cookie = newCookie;
    Log("[v] Refreshed saved cookie for " + account.username + ".");
    return true;
}

static bool RefreshStoredAccountCookie(int index, RobloxAccount& account) {
    RobloxAccount refreshed = account;
    if (!RefreshAccountCookieInPlace(refreshed)) return false;

    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) return false;
        if (accounts[index].userId != account.userId) return false;
        accounts[index].cookie = refreshed.cookie;
        account.cookie = refreshed.cookie;
    }

    SaveAccounts();
    return true;
}

static std::wstring FindRobloxPlayerExe() {
    PrepareRobloxInstalls();
    {
        std::lock_guard<std::mutex> lock(robloxBuildMutex);
        if (!robloxBuild.activeVersion.empty()) {
            std::filesystem::path exe = std::filesystem::path(g_exeDir) / "Builds" /
                robloxBuild.activeVersion / "RobloxPlayerBeta.exe";
            std::error_code ec;
            if (std::filesystem::exists(exe, ec)) return exe.wstring();
        }
    }

    std::vector<std::filesystem::path> roots;
    std::wstring local = LocalAppDataPath();
    if (!local.empty()) roots.emplace_back(local + L"\\Roblox\\Versions");
    if (const char* pf86 = std::getenv("ProgramFiles(x86)")) roots.emplace_back(std::string(pf86) + "\\Roblox\\Versions");
    if (const char* pf = std::getenv("ProgramFiles")) roots.emplace_back(std::string(pf) + "\\Roblox\\Versions");

    std::filesystem::path best;
    std::filesystem::file_time_type bestTime{};
    for (const auto& root : roots) {
        if (!std::filesystem::exists(root)) continue;
        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec || !entry.is_directory(ec)) continue;
            std::filesystem::path exe = entry.path() / "RobloxPlayerBeta.exe";
            if (!std::filesystem::exists(exe, ec)) continue;
            auto t = std::filesystem::last_write_time(exe, ec);
            if (best.empty() || (!ec && t > bestTime)) {
                best = exe;
                bestTime = t;
            }
        }
    }
    return best.wstring();
}

static bool LaunchRobloxDirect(const std::string& ticket, const std::string& placeLauncherUrl) {
    std::wstring exe = FindRobloxPlayerExe();
    if (exe.empty()) {
        Log("[!] Could not find RobloxPlayerBeta.exe for direct launch fallback.");
        return false;
    }

    std::wstring args = L"--app -t " + Widen(ticket) + L" -j \"" + Widen(placeLauncherUrl) + L"\"";
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (!ok) {
        Log("[!] Direct Roblox launch failed: " + LastWin32ErrorString(GetLastError()));
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static bool WaitForRobloxProcessCountAbove(int countBefore, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if ((int)FindPidsByName(L"RobloxPlayerBeta.exe").size() > countBefore) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

static void SchedulePostLaunchCookieScrub(const std::string& username) {
    std::thread([username]() {
        ScrubAndLockRobloxCookieFile(("post launch for " + username).c_str(), false);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        ScrubAndLockRobloxCookieFile(("delayed post launch for " + username).c_str(), false);
        std::this_thread::sleep_for(std::chrono::seconds(12));
        ScrubAndLockRobloxCookieFile(("final post launch for " + username).c_str(), false);
    }).detach();
}

static std::wstring AccountsFilePath() { return g_exeDir + L"\\accounts.dat"; }
static constexpr char kAccountsFileMagicV2[8] = { 'V', 'M', 'T', 'A', 'C', 'C', 'T', '2' };
static constexpr char kAccountsFileMagicV3[8] = { 'V', 'M', 'T', 'A', 'C', 'C', 'T', '3' };
static constexpr char kAccountsFileMagicV4[8] = { 'V', 'M', 'T', 'A', 'C', 'C', 'T', '4' };
static constexpr char kAccountsFileMagicV5[8] = { 'V', 'M', 'T', 'A', 'C', 'C', 'T', '5' };

void SaveAccounts() {
    std::lock_guard<std::mutex> lock(accountsMutex);
    std::string buf;
    buf.append(kAccountsFileMagicV5, sizeof(kAccountsFileMagicV5));
    uint32_t count = (uint32_t)accounts.size();
    buf.append((const char*)&count, sizeof(count));
    for (auto& a : accounts) {
        uint32_t ulen = (uint32_t)a.username.size();
        buf.append((const char*)&ulen, sizeof(ulen));
        buf.append(a.username);
        buf.append((const char*)&a.userId, sizeof(a.userId));
        uint32_t clen = (uint32_t)a.cookie.size();
        buf.append((const char*)&clen, sizeof(clen));
        buf.append(a.cookie);
        uint32_t plen = (uint32_t)a.password.size();
        buf.append((const char*)&plen, sizeof(plen));
        buf.append(a.password);
        uint32_t alen = (uint32_t)a.alias.size();
        buf.append((const char*)&alen, sizeof(alen));
        buf.append(a.alias);
        buf.push_back(a.priority ? 1 : 0);
        uint32_t glen = (uint32_t)a.group.size();
        buf.append((const char*)&glen, sizeof(glen));
        buf.append(a.group);
    }

    DATA_BLOB dataIn = { (DWORD)buf.size(), (BYTE*)buf.data() };
    DATA_BLOB dataOut = {};
    if (!CryptProtectData(&dataIn, L"VelsMultiTool accounts", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        Log("[!] Failed to encrypt accounts.dat (CryptProtectData failed).");
        return;
    }

    std::ofstream f(AccountsFilePath().c_str(), std::ios::binary | std::ios::trunc);
    if (f) f.write((const char*)dataOut.pbData, dataOut.cbData);
    LocalFree(dataOut.pbData);
}

void LoadAccounts() {
    std::wstring path = AccountsFilePath();
    if (!std::filesystem::exists(path)) return;

    std::ifstream f(path.c_str(), std::ios::binary);
    std::string encrypted((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (encrypted.empty()) return;

    DATA_BLOB dataIn = { (DWORD)encrypted.size(), (BYTE*)encrypted.data() };
    DATA_BLOB dataOut = {};
    if (!CryptUnprotectData(&dataIn, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        Log("[!] Failed to decrypt accounts.dat - it may belong to a different Windows user.");
        return;
    }

    std::string buf((const char*)dataOut.pbData, dataOut.cbData);
    LocalFree(dataOut.pbData);

    std::vector<RobloxAccount> loaded;
    size_t pos = 0;
    bool hasPassword = false, hasAlias = false, hasPriority = false, hasGroup = false;
    if (buf.size() >= sizeof(kAccountsFileMagicV5) &&
        memcmp(buf.data(), kAccountsFileMagicV5, sizeof(kAccountsFileMagicV5)) == 0) {
        hasPassword = hasAlias = hasPriority = hasGroup = true;
        pos = sizeof(kAccountsFileMagicV5);
    } else if (buf.size() >= sizeof(kAccountsFileMagicV4) &&
        memcmp(buf.data(), kAccountsFileMagicV4, sizeof(kAccountsFileMagicV4)) == 0) {
        hasPassword = hasAlias = hasPriority = true;
        pos = sizeof(kAccountsFileMagicV4);
    } else if (buf.size() >= sizeof(kAccountsFileMagicV3) &&
        memcmp(buf.data(), kAccountsFileMagicV3, sizeof(kAccountsFileMagicV3)) == 0) {
        hasPassword = true;
        hasAlias = true;
        pos = sizeof(kAccountsFileMagicV3);
    } else if (buf.size() >= sizeof(kAccountsFileMagicV2) &&
        memcmp(buf.data(), kAccountsFileMagicV2, sizeof(kAccountsFileMagicV2)) == 0) {
        hasPassword = true;
        pos = sizeof(kAccountsFileMagicV2);
    }

    if (!hasPriority) {
        std::error_code bec;
        std::filesystem::copy_file(path, path + L".v3.bak", std::filesystem::copy_options::skip_existing, bec);
    }

    auto readU32 = [&](uint32_t& v) -> bool {
        if (pos + sizeof(uint32_t) > buf.size()) return false;
        memcpy(&v, buf.data() + pos, sizeof(uint32_t));
        pos += sizeof(uint32_t);
        return true;
        };

    uint32_t count = 0;
    if (!readU32(count)) return;

    for (uint32_t i = 0; i < count; ++i) {
        RobloxAccount a;
        uint32_t ulen = 0;
        if (!readU32(ulen) || pos + ulen > buf.size()) break;
        a.username = buf.substr(pos, ulen); pos += ulen;

        if (pos + sizeof(long long) > buf.size()) break;
        memcpy(&a.userId, buf.data() + pos, sizeof(long long)); pos += sizeof(long long);

        uint32_t clen = 0;
        if (!readU32(clen) || pos + clen > buf.size()) break;
        a.cookie = buf.substr(pos, clen); pos += clen;

        if (hasPassword) {
            uint32_t plen = 0;
            if (!readU32(plen) || pos + plen > buf.size()) break;
            a.password = buf.substr(pos, plen); pos += plen;
        }

        if (hasAlias) {
            uint32_t alen = 0;
            if (!readU32(alen) || pos + alen > buf.size()) break;
            a.alias = buf.substr(pos, alen); pos += alen;
        }

        if (hasPriority) {
            if (pos + 1 > buf.size()) break;
            a.priority = buf[pos] != 0; pos += 1;
        }

        if (hasGroup) {
            uint32_t glen = 0;
            if (!readU32(glen) || pos + glen > buf.size()) break;
            a.group = buf.substr(pos, glen); pos += glen;
        }

        loaded.push_back(std::move(a));
    }

    std::lock_guard<std::mutex> lock(accountsMutex);
    accounts = std::move(loaded);
}

bool AddAccountFromCookie(const std::string& cookie, const std::string& password) {
    HttpResponse resp = HttpRequest(L"users.roblox.com", L"/v1/users/authenticated", L"GET", cookie, {}, "");
    if (!resp.ok || resp.status != 200) {
        Log("[!] That cookie doesn't look valid (Roblox rejected it).");
        return false;
    }

    RobloxAccount account;
    account.username = ExtractJsonStringField(resp.body, "name");
    account.userId = ExtractJsonLongField(resp.body, "id");
    account.cookie = cookie;
    account.password = password;

    if (account.username.empty()) {
        Log("[!] Could not read account info from Roblox's response.");
        return false;
    }

    bool updated = false;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        for (auto& existing : accounts) {
            if ((account.userId != 0 && existing.userId == account.userId) ||
                (!existing.username.empty() && existing.username == account.username)) {
                existing.userId = account.userId;
                existing.cookie = account.cookie;
                if (!account.password.empty()) existing.password = account.password;
                updated = true;
                break;
            }
        }
        if (!updated) accounts.push_back(account);
    }
    SaveAccounts();
    Log(updated ? "[i] Refreshed saved account: " + account.username : "[v] Added account: " + account.username);
    AddActivity(updated ? "Refreshed Account" : "Account Added", account.username);
    return true;
}

void RemoveAccount(int index) {
    std::string removedName;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) return;
        removedName = accounts[index].username;
        accounts.erase(accounts.begin() + index);
    }
    SaveAccounts();
    Log("[i] Removed account: " + removedName);
    AddActivity("Account Removed", removedName);
}

void SetAccountPassword(int index, const std::string& password) {
    std::string accountName;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) return;
        accounts[index].password = password;
        accountName = accounts[index].username;
    }
    SaveAccounts();
    Log("[v] Saved password for " + accountName);
}

void SetAccountAlias(int index, const std::string& alias) {
    std::string accountName;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) return;
        accounts[index].alias = alias;
        accountName = accounts[index].username;
    }
    SaveAccounts();
    Log("[v] Saved alias for " + accountName);
}

void SetAccountGroup(int index, const std::string& group) {
    std::string accountName;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) return;
        accounts[index].group = group;
        accountName = accounts[index].username;
    }
    SaveAccounts();
    Log(group.empty() ? "[i] Removed " + accountName + " from its group."
                      : "[v] Added " + accountName + " to \"" + group + "\".");
}

void MoveAccount(int from, int to) {
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        int n = (int)accounts.size();
        if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
        RobloxAccount moved = std::move(accounts[from]);
        accounts.erase(accounts.begin() + from);
        accounts.insert(accounts.begin() + to, std::move(moved));
        bool abovePriority = to > 0 && accounts[to - 1].priority;
        bool belowPriority = to + 1 < n && accounts[to + 1].priority;
        if (belowPriority) accounts[to].priority = true;
        else if (to > 0 && !abovePriority) accounts[to].priority = false;
    }
    SaveAccounts();
}

void SetAccountPriority(int index, bool priority) {
    std::string accountName;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) return;
        if (accounts[index].priority == priority) return;
        accounts[index].priority = priority;
        accountName = accounts[index].username;
        std::stable_partition(accounts.begin(), accounts.end(),
            [](const RobloxAccount& a) { return a.priority; });
    }
    SaveAccounts();
    Log(priority ? "[v] Prioritised " + accountName + "." : "[i] Removed priority from " + accountName + ".");
}

static void LaunchAccountInternal(int index, long long placeId, const std::string& linkCode, const std::string& gameId = "") {
    RobloxAccount account;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) {
            Log("[!] Select an account first.");
            return;
        }
        account = accounts[index];
    }

    if (placeId <= 0) {
        Log("[!] Enter a valid Place ID to launch into.");
        return;
    }

    ScrubAndLockRobloxCookieFile(("before launch for " + account.username).c_str(), false);
    ApplyFpsCapFor(account.userId);

    std::string ticket = GetAuthTicket(account.cookie);
    if (ticket.empty()) {
        Log("[!] Failed to get an auth ticket - the saved cookie may have expired. Re-add the account.");
        ScrubAndLockRobloxCookieFile(("after failed launch for " + account.username).c_str(), false);
        return;
    }

    std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> dist(100000, 999999);
    std::string browserTrackerId = std::to_string(dist(rng)) + std::to_string(dist(rng));
    long long launchTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string placeLauncherUrl;
    if (!linkCode.empty()) {
        placeLauncherUrl = "https://assetgame.roblox.com/game/PlaceLauncher.ashx?request=RequestPrivateGame"
            "&browserTrackerId=" + browserTrackerId + "&placeId=" + std::to_string(placeId) +
            "&linkCode=" + UrlEncode(linkCode) + "&accessCode=&isPlayTogetherGame=false";
    } else if (!gameId.empty()) {
        placeLauncherUrl = "https://assetgame.roblox.com/game/PlaceLauncher.ashx?request=RequestGameJob"
            "&browserTrackerId=" + browserTrackerId + "&placeId=" + std::to_string(placeId) +
            "&gameId=" + UrlEncode(gameId) + "&isPlayTogetherGame=false";
    } else {
        placeLauncherUrl = "https://assetgame.roblox.com/game/PlaceLauncher.ashx?request=RequestGame"
            "&browserTrackerId=" + browserTrackerId + "&placeId=" + std::to_string(placeId) + "&isPlayTogetherGame=false";
    }

    std::string uri = "roblox-player:1+launchmode:play+gameinfo:" + ticket +
        "+launchtime:" + std::to_string(launchTime) +
        "+placelauncherurl:" + UrlEncode(placeLauncherUrl) +
        "+browsertrackerid:" + browserTrackerId +
        "+robloxLocale:en_us+gameLocale:en_us+channel:+LaunchExp:InApp";

    std::set<DWORD> pidsBefore;
    { auto v = FindPidsByName(L"RobloxPlayerBeta.exe"); pidsBefore.insert(v.begin(), v.end()); }
    {
        std::lock_guard<std::mutex> lk(launchedMutex);
        launchedTrackers[account.userId] = browserTrackerId;
        launchedAt[account.userId] = std::chrono::steady_clock::now();
        launchedPids.erase(account.userId);
    }

    bool pinnedBuild;
    { std::lock_guard<std::mutex> lock(robloxBuildMutex); pinnedBuild = !robloxBuild.activeVersion.empty(); }
    if (pinnedBuild) {
        if (!LaunchRobloxDirect(ticket, placeLauncherUrl)) {
            ScrubAndLockRobloxCookieFile(("after failed launch for " + account.username).c_str(), false);
            return;
        }
        std::thread(AssignLaunchedPid, account.userId, pidsBefore).detach();
        if (!gameId.empty()) SaveLastServer(placeId, gameId);
        OnAccountLaunched(account.userId, placeId, gameId, linkCode);
        Log("[v] Launched " + account.username + " into " + std::to_string(placeId) +
            (linkCode.empty() ? (gameId.empty() ? "." : " (chosen server).") : " (private server)."));
        AddActivity(linkCode.empty() ? "Launched Roblox" : "Launched into private server", account.username);
        SchedulePostLaunchCookieScrub(account.username);
        return;
    }

    int beforeCount = (int)pidsBefore.size();
    PrepareRobloxInstalls();
    std::wstring wuri(uri.begin(), uri.end());
    HINSTANCE r = ShellExecuteW(nullptr, L"open", wuri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    bool launched = (INT_PTR)r > 32 && WaitForRobloxProcessCountAbove(beforeCount, 5000);
    if (!launched && !LaunchRobloxDirect(ticket, placeLauncherUrl)) {
        ScrubAndLockRobloxCookieFile(("after failed launch for " + account.username).c_str(), false);
        return;
    }

    std::thread(AssignLaunchedPid, account.userId, pidsBefore).detach();
    if (!gameId.empty()) SaveLastServer(placeId, gameId);
    OnAccountLaunched(account.userId, placeId, gameId, linkCode);
    Log("[v] Launched " + account.username + " into " + std::to_string(placeId) +
        (linkCode.empty() ? (gameId.empty() ? "." : " (chosen server).") : " (private server)."));
    AddActivity(linkCode.empty() ? "Launched Roblox" : "Launched into private server", account.username);
    SchedulePostLaunchCookieScrub(account.username);
}

void LaunchAccountIntoPlace(int index, long long placeId) {
    LaunchAccountInternal(index, placeId, "");
}

void LaunchAccountIntoServer(int index, long long placeId, const std::string& gameId) {
    LaunchAccountInternal(index, placeId, "", gameId);
}

void LaunchAccountIntoPrivateServer(int index, long long placeId, const std::string& linkCode) {
    if (linkCode.empty()) { LaunchAccountInternal(index, placeId, ""); return; }
    LaunchAccountInternal(index, placeId, linkCode);
}

void OpenAccountWeb(int index) {
    RobloxAccount account;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) {
            Log("[!] Select an account first.");
            return;
        }
        account = accounts[index];
    }

    if (account.cookie.empty()) {
        Log("[!] This account has no saved cookie.");
        return;
    }

    Log("[i] Opening web session for " + account.username + "...");
    login::OpenAccountWebSession(g_exeDir, account.cookie, account.userId, account.username);
    AddActivity("Opened Web", account.username);
}

static bool LaunchRobloxClientDirect(const std::string& ticket) {
    std::wstring exe = FindRobloxPlayerExe();
    if (exe.empty()) return false;
    std::wstring cmd = L"\"" + exe + L"\" --app -t " + Widen(ticket);
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    std::wstring workDir = std::filesystem::path(exe).parent_path().wstring();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void LaunchAccountClient(int index) {
    RobloxAccount account;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) { Log("[!] Select an account first."); return; }
        account = accounts[index];
    }

    ScrubAndLockRobloxCookieFile(("before client launch for " + account.username).c_str(), false);
    ApplyFpsCapFor(account.userId);

    std::string ticket = GetAuthTicket(account.cookie);
    if (ticket.empty()) {
        Log("[!] Could not sign " + account.username + " in - the saved cookie may have expired. Re-add the account.");
        ScrubAndLockRobloxCookieFile(("after failed client launch for " + account.username).c_str(), false);
        return;
    }

    std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> dist(100000, 999999);
    std::string browserTrackerId = std::to_string(dist(rng)) + std::to_string(dist(rng));
    long long launchTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string uri = "roblox-player:1+launchmode:app+gameinfo:" + ticket +
        "+launchtime:" + std::to_string(launchTime) +
        "+browsertrackerid:" + browserTrackerId +
        "+robloxLocale:en_us+gameLocale:en_us+channel:+LaunchExp:InApp";

    bool pinnedBuild;
    { std::lock_guard<std::mutex> lock(robloxBuildMutex); pinnedBuild = !robloxBuild.activeVersion.empty(); }

    bool launched = false;
    if (pinnedBuild) {
        launched = LaunchRobloxClientDirect(ticket);
    } else {
        int beforeCount = (int)FindPidsByName(L"RobloxPlayerBeta.exe").size();
        PrepareRobloxInstalls();
        std::wstring wuri(uri.begin(), uri.end());
        HINSTANCE r = ShellExecuteW(nullptr, L"open", wuri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        launched = (INT_PTR)r > 32 && WaitForRobloxProcessCountAbove(beforeCount, 5000);
        if (!launched) launched = LaunchRobloxClientDirect(ticket);
    }

    if (!launched) {
        Log("[!] Could not open the Roblox client for " + account.username + ".");
        ScrubAndLockRobloxCookieFile(("after failed client launch for " + account.username).c_str(), false);
        return;
    }

    Log("[v] Opened the Roblox client signed in as " + account.username + ".");
    AddActivity("Opened Roblox client", account.username);
    SchedulePostLaunchCookieScrub(account.username);
}

std::atomic<bool> joinBestServer{ false };
std::mutex lastServerMutex;
LastServer lastServer;

std::string FindBestServer(long long placeId) {
    if (placeId <= 0) return "";
    HttpResponse resp = HttpRequest(L"games.roblox.com",
        L"/v1/games/" + std::to_wstring(placeId) + L"/servers/Public?excludeFullGames=true&limit=100",
        L"GET", "", {}, "");
    if (!resp.ok || resp.status != 200) {
        Log("[!] Could not fetch the server list (HTTP " + std::to_string(resp.status) + ").");
        return "";
    }

    json::Value root;
    if (!json::Parse(resp.body, root)) { Log("[!] Could not parse the server list."); return ""; }

    std::string best;
    double bestPing = 1e18;
    long long bestPlaying = -1;
    for (const auto& s : root["data"].a) {
        std::string id = s["id"].str();
        if (id.empty()) continue;
        long long playing = s["playing"].i64(), maxPlayers = s["maxPlayers"].i64();
        if (maxPlayers > 0 && playing >= maxPlayers) continue;
        double ping = s["ping"].type == json::Value::Number ? s["ping"].n : 1e17;
        if (ping < bestPing || (ping == bestPing && playing > bestPlaying)) {
            bestPing = ping;
            bestPlaying = playing;
            best = id;
        }
    }

    if (best.empty()) Log("[!] No joinable public servers found for " + std::to_string(placeId) + ".");
    else Log("[i] Best server ~" + std::to_string((int)(bestPing + 0.5)) + "ms ping.");
    return best;
}

static std::wstring JoinBestFilePath()  { return g_exeDir + L"\\joinbest.dat"; }
static std::wstring LastServerFilePath() { return g_exeDir + L"\\lastserver.dat"; }

void SetJoinBestServer(bool on) {
    joinBestServer = on;
    std::ofstream f(JoinBestFilePath().c_str(), std::ios::trunc);
    if (f) f << (on ? 1 : 0);
}

void LoadJoinBestServer() {
    std::ifstream f(JoinBestFilePath().c_str());
    int v = 0;
    if (f >> v) joinBestServer = (v != 0);
}

void SaveLastServer(long long placeId, const std::string& gameId) {
    {
        std::lock_guard<std::mutex> lock(lastServerMutex);
        lastServer.placeId = placeId;
        lastServer.gameId = gameId;
    }
    std::ofstream f(LastServerFilePath().c_str(), std::ios::trunc);
    if (f) f << placeId << ' ' << gameId;
}

void LoadLastServer() {
    std::ifstream f(LastServerFilePath().c_str());
    long long pid = 0;
    std::string gid;
    if (f >> pid >> gid) {
        std::lock_guard<std::mutex> lock(lastServerMutex);
        lastServer.placeId = pid;
        lastServer.gameId = gid;
    }
}

void LaunchRobloxClient() {
    std::wstring exe = FindRobloxPlayerExe();
    if (!exe.empty()) {
        std::wstring cmd = L"\"" + exe + L"\" --app";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(0);
        std::wstring workDir = std::filesystem::path(exe).parent_path().wstring();
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            Log("[v] Opened the Roblox client.");
            AddActivity("Opened Roblox client", "");
            return;
        }
    }

    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"roblox-player:1+launchmode:app", nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) Log("[!] Could not find a Roblox client. Install Roblox or download a build first.");
    else Log("[v] Opened the Roblox client.");
}

static bool ParseIsoDate(const std::string& iso, int& year, int& month, int& day) {
    if (iso.size() < 10) return false;
    try {
        year = std::stoi(iso.substr(0, 4));
        month = std::stoi(iso.substr(5, 2));
        day = std::stoi(iso.substr(8, 2));
    } catch (...) { return false; }
    return true;
}

static std::string FormatJoinDate(const std::string& iso) {
    static const char* kMonthNames[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    int y = 0, m = 0, d = 0;
    if (!ParseIsoDate(iso, y, m, d) || m < 1 || m > 12) return "";
    return std::string(kMonthNames[m - 1]) + " " + std::to_string(d) + ", " + std::to_string(y);
}

static std::mutex g_gmtimeMutex;

static std::string FormatAccountAge(const std::string& iso) {
    int y = 0, m = 0, d = 0;
    if (!ParseIsoDate(iso, y, m, d)) return "";

    std::tm nowTm{};
    {
        std::lock_guard<std::mutex> lock(g_gmtimeMutex);
        time_t now = std::time(nullptr);
        nowTm = *std::gmtime(&now);
    }
    int nowYear = nowTm.tm_year + 1900;
    int nowMonth = nowTm.tm_mon + 1;
    int nowDay = nowTm.tm_mday;

    int totalMonths = (nowYear - y) * 12 + (nowMonth - m);
    if (nowDay < d) totalMonths -= 1;
    if (totalMonths < 0) totalMonths = 0;
    int years = totalMonths / 12;
    int months = totalMonths % 12;

    std::string result;
    if (years > 0) result += std::to_string(years) + (years == 1 ? " year" : " years");
    if (months > 0) {
        if (!result.empty()) result += ", ";
        result += std::to_string(months) + (months == 1 ? " month" : " months");
    }
    if (result.empty()) result = "less than a month";
    return result;
}

void FetchAccountStats(int index) {
    RobloxAccount account;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) return;
        account = accounts[index];
    }

    long long friends = -1, followers = -1, following = -1, robux = -1;
    std::string joinDate, accountAge;
    std::wstring uid = std::to_wstring(account.userId);

    HttpResponse userResp = HttpRequest(L"users.roblox.com", L"/v1/users/" + uid, L"GET", "", {}, "");
    if (userResp.ok && userResp.status == 200) {
        std::string created = ExtractJsonStringField(userResp.body, "created");
        joinDate = FormatJoinDate(created);
        accountAge = FormatAccountAge(created);
    }

    HttpResponse friendsResp = HttpRequest(L"friends.roblox.com", L"/v1/users/" + uid + L"/friends/count", L"GET", "", {}, "");
    if (friendsResp.ok && friendsResp.status == 200) friends = ExtractJsonLongField(friendsResp.body, "count");

    HttpResponse followersResp = HttpRequest(L"friends.roblox.com", L"/v1/users/" + uid + L"/followers/count", L"GET", "", {}, "");
    if (followersResp.ok && followersResp.status == 200) followers = ExtractJsonLongField(followersResp.body, "count");

    HttpResponse followingResp = HttpRequest(L"friends.roblox.com", L"/v1/users/" + uid + L"/followings/count", L"GET", "", {}, "");
    if (followingResp.ok && followingResp.status == 200) following = ExtractJsonLongField(followingResp.body, "count");

    HttpResponse robuxResp = HttpRequest(L"economy.roblox.com", L"/v1/users/" + uid + L"/currency", L"GET", account.cookie, {}, "");
    if (robuxResp.ok && robuxResp.status == 200) robux = ExtractJsonLongField(robuxResp.body, "robux");

    std::lock_guard<std::mutex> lock(accountsMutex);
    if (index < 0 || index >= (int)accounts.size() || accounts[index].userId != account.userId) return;
    accounts[index].friendsCount = friends;
    accounts[index].followersCount = followers;
    accounts[index].followingCount = following;
    accounts[index].robuxBalance = robux;
    accounts[index].joinDate = joinDate;
    accounts[index].accountAge = accountAge;
    accounts[index].statsLoaded = true;
}

static bool SplitHttpsUrl(const std::string& url, std::wstring& host, std::wstring& path) {
    const std::string prefix = "https://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;
    std::string rest = url.substr(prefix.size());
    size_t slash = rest.find('/');
    std::string h = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string p = slash == std::string::npos ? "/" : rest.substr(slash);
    host.assign(h.begin(), h.end());
    path.assign(p.begin(), p.end());
    return true;
}

static std::vector<unsigned char> DownloadBinary(const std::string& url) {
    std::wstring host, path;
    if (!SplitHttpsUrl(url, host, path)) return {};
    HttpResponse resp = HttpRequest(host, path, L"GET", "", {}, "");
    if (!resp.ok || resp.status != 200 || resp.body.empty()) return {};
    return std::vector<unsigned char>(resp.body.begin(), resp.body.end());
}

void FetchAccountAvatar(int index) {
    long long userId = 0;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (index < 0 || index >= (int)accounts.size()) return;
        userId = accounts[index].userId;
    }
    if (userId <= 0) return;

    HttpResponse metaResp = HttpRequest(L"thumbnails.roblox.com",
        L"/v1/users/avatar-headshot?userIds=" + std::to_wstring(userId) + L"&size=150x150&format=png&isCircular=false",
        L"GET", "", {}, "");
    if (!metaResp.ok || metaResp.status != 200) return;

    std::string imageUrl = ExtractJsonStringField(metaResp.body, "imageUrl");
    if (imageUrl.empty()) return;

    std::vector<unsigned char> bytes = DownloadBinary(imageUrl);
    if (bytes.empty()) return;

    std::lock_guard<std::mutex> lock(accountsMutex);
    if (index < 0 || index >= (int)accounts.size() || accounts[index].userId != userId) return;
    accounts[index].avatarPng = std::move(bytes);
    accounts[index].avatarLoaded = true;
}

bool LookupGame(long long placeId, GameLookup& out) {
    if (placeId <= 0) return false;
    HttpResponse uni = HttpRequest(L"apis.roblox.com",
        L"/universes/v1/places/" + std::to_wstring(placeId) + L"/universe", L"GET", "", {}, "");
    if (!uni.ok || uni.status != 200) return false;
    out.universeId = ExtractJsonLongField(uni.body, "universeId");
    if (out.universeId <= 0) return false;
    std::wstring uid = std::to_wstring(out.universeId);
    HttpResponse games = HttpRequest(L"games.roblox.com", L"/v1/games?universeIds=" + uid, L"GET", "", {}, "");
    if (games.ok && games.status == 200) {
        out.rootPlaceId = ExtractJsonLongField(games.body, "rootPlaceId");
        out.name = ExtractJsonStringField(games.body, "name");
    }
    HttpResponse icon = HttpRequest(L"thumbnails.roblox.com",
        L"/v1/games/icons?universeIds=" + uid + L"&size=512x512&format=Png&isCircular=false", L"GET", "", {}, "");
    if (icon.ok && icon.status == 200) out.iconUrl = ExtractJsonStringField(icon.body, "imageUrl");
    return !out.name.empty();
}

bool QueryPresence(long long userId, const std::string& cookie, long long& placeId, long long& rootPlaceId, std::string& gameId) {
    const std::string body = "{\"userIds\":[" + std::to_string(userId) + "]}";
    std::wstring csrf;
    HttpResponse r = HttpRequest(L"presence.roblox.com", L"/v1/presence/users", L"POST", cookie,
        { { L"Content-Type", L"application/json" } }, body, &csrf);
    if (r.status == 403 && !csrf.empty())
        r = HttpRequest(L"presence.roblox.com", L"/v1/presence/users", L"POST", cookie,
            { { L"Content-Type", L"application/json" }, { L"X-CSRF-TOKEN", csrf } }, body);
    if (!r.ok || r.status != 200) return false;
    placeId = ExtractJsonLongField(r.body, "placeId");
    rootPlaceId = ExtractJsonLongField(r.body, "rootPlaceId");
    gameId = ExtractJsonStringField(r.body, "gameId");
    return true;
}

void FetchPlaceInfo(long long placeId, const std::string& cookie) {
    if (placeId <= 0) return;

    std::string name, gameName, creator;
    long long visits = -1, favorites = -1, universeId = 0, playing = -1, maxPlayers = -1, rootPlaceId = 0;
    std::vector<unsigned char> icon;

    HttpResponse detailsResp = HttpRequest(L"games.roblox.com",
        L"/v1/games/multiget-place-details?placeIds=" + std::to_wstring(placeId), L"GET", cookie, {}, "");
    if (detailsResp.ok && detailsResp.status == 200) {
        name = ExtractJsonStringField(detailsResp.body, "name");
        creator = ExtractJsonStringField(detailsResp.body, "builder");
        universeId = ExtractJsonLongField(detailsResp.body, "universeId");
    }

    if (universeId <= 0) {
        HttpResponse uniResp = HttpRequest(L"apis.roblox.com",
            L"/universes/v1/places/" + std::to_wstring(placeId) + L"/universe", L"GET", "", {}, "");
        if (uniResp.ok && uniResp.status == 200) universeId = ExtractJsonLongField(uniResp.body, "universeId");
    }

    if (universeId > 0) {
        std::wstring uid = std::to_wstring(universeId);
        HttpResponse gamesResp = HttpRequest(L"games.roblox.com", L"/v1/games?universeIds=" + uid, L"GET", "", {}, "");
        if (gamesResp.ok && gamesResp.status == 200) {
            rootPlaceId = ExtractJsonLongField(gamesResp.body, "rootPlaceId");
            gameName = ExtractJsonStringField(gamesResp.body, "name");
            size_t creatorPos = gamesResp.body.find("\"creator\":");
            if (creator.empty() && creatorPos != std::string::npos)
                creator = ExtractJsonStringField(gamesResp.body.substr(creatorPos), "name");
            visits = ExtractJsonLongField(gamesResp.body, "visits");
            playing = ExtractJsonLongField(gamesResp.body, "playing");
            maxPlayers = ExtractJsonLongField(gamesResp.body, "maxPlayers");
        }

        HttpResponse favResp = HttpRequest(L"games.roblox.com", L"/v1/games/" + uid + L"/favorites/count", L"GET", "", {}, "");
        if (favResp.ok && favResp.status == 200) favorites = ExtractJsonLongField(favResp.body, "favoritesCount");

        HttpResponse gameIconResp = HttpRequest(L"thumbnails.roblox.com",
            L"/v1/games/icons?universeIds=" + uid + L"&size=512x512&format=Png&isCircular=false", L"GET", "", {}, "");
        if (gameIconResp.ok && gameIconResp.status == 200) {
            std::string imageUrl = ExtractJsonStringField(gameIconResp.body, "imageUrl");
            if (!imageUrl.empty()) icon = DownloadBinary(imageUrl);
        }
    }

    if (icon.empty()) {
        HttpResponse iconResp = HttpRequest(L"thumbnails.roblox.com",
            L"/v1/places/gameicons?placeIds=" + std::to_wstring(placeId) + L"&size=150x150&format=png&isCircular=false",
            L"GET", "", {}, "");
        if (iconResp.ok && iconResp.status == 200) {
            std::string imageUrl = ExtractJsonStringField(iconResp.body, "imageUrl");
            if (!imageUrl.empty()) icon = DownloadBinary(imageUrl);
        }
    }

    std::lock_guard<std::mutex> lock(placeInfoMutex);
    placeInfo.placeId = placeId;
    placeInfo.rootPlaceId = rootPlaceId;
    placeInfo.isSubPlace = rootPlaceId > 0 && rootPlaceId != placeId;
    placeInfo.placeName = name;
    placeInfo.name = !gameName.empty() ? gameName : (!name.empty() ? name : "Place " + std::to_string(placeId));
    placeInfo.creator = creator;
    placeInfo.visits = visits;
    placeInfo.favorites = favorites;
    placeInfo.playing = playing;
    placeInfo.maxPlayers = maxPlayers;
    if (!icon.empty()) placeInfo.iconPng = std::move(icon);
    placeInfo.loaded = true;
}

static std::wstring PlaceIdFilePath() { return g_exeDir + L"\\placeid.dat"; }

void SavePlaceId(long long placeId) {
    savedPlaceId = placeId;
    std::ofstream f(PlaceIdFilePath().c_str(), std::ios::trunc);
    if (f) f << placeId;
    {
        std::lock_guard<std::mutex> lock(placeInfoMutex);
        if (placeInfo.placeId != placeId) placeInfo = PlaceInfo{};
    }
    AddActivity("Updated Place ID", std::to_string(placeId));
}

void LoadPlaceId() {
    std::wstring path = PlaceIdFilePath();
    if (!std::filesystem::exists(path)) return;
    std::ifstream f(path.c_str());
    long long id = 0;
    if (f >> id) savedPlaceId = id;
}

std::mutex savedPlacesMutex;
std::vector<SavedPlace> savedPlaces;

static std::wstring PlacesFilePath() { return g_exeDir + L"\\places.dat"; }

static void WriteSavedPlacesLocked() {
    std::ofstream f(PlacesFilePath().c_str(), std::ios::trunc);
    if (!f) return;
    for (const auto& p : savedPlaces) f << p.id << ' ' << (p.favorite ? "+fav " : "") << p.name << '\n';
}

void LoadSavedPlaces() {
    std::lock_guard<std::mutex> lock(savedPlacesMutex);
    std::wstring path = PlacesFilePath();
    if (std::filesystem::exists(path)) {
        std::ifstream f(path.c_str());
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            long long id = 0;
            if (!(ss >> id) || id <= 0) continue;
            std::string name;
            std::getline(ss, name);
            size_t b = name.find_first_not_of(" \t\r");
            size_t e = name.find_last_not_of(" \t\r");
            name = (b == std::string::npos) ? std::string() : name.substr(b, e - b + 1);
            bool favorite = false;
            if (name == "+fav" || name.rfind("+fav ", 0) == 0) {
                favorite = true;
                name = name.size() > 5 ? name.substr(5) : std::string();
            }
            savedPlaces.push_back({ id, name, favorite });
        }
    }
    if (savedPlaces.empty()) {
        savedPlaces = {
            { 10561483644LL,     "Mystic Falls" },
            { 10561482233LL,     "New Orleans" },
            { 10561484691LL,     "Salvatore School" },
            { 123974602339071LL, "Baseplate" },
            { 4924922222LL,      "Brookhaven" },
        };
        WriteSavedPlacesLocked();
    }
}

void AddSavedPlace(long long id, const std::string& name) {
    if (id <= 0) return;
    std::lock_guard<std::mutex> lock(savedPlacesMutex);
    for (auto& p : savedPlaces) {
        if (p.id == id) { p.name = name; WriteSavedPlacesLocked(); return; }
    }
    savedPlaces.push_back({ id, name });
    WriteSavedPlacesLocked();
    AddActivity("Saved place preset", name.empty() ? std::to_string(id) : name);
}

void RemoveSavedPlace(long long id) {
    std::lock_guard<std::mutex> lock(savedPlacesMutex);
    for (size_t i = 0; i < savedPlaces.size(); ++i) {
        if (savedPlaces[i].id == id) { savedPlaces.erase(savedPlaces.begin() + i); break; }
    }
    WriteSavedPlacesLocked();
}

void SetSavedPlaceFavorite(long long id, bool favorite) {
    std::lock_guard<std::mutex> lock(savedPlacesMutex);
    for (auto& p : savedPlaces) {
        if (p.id == id) { p.favorite = favorite; WriteSavedPlacesLocked(); return; }
    }
}

std::mutex activePrivateServerMutex;
PrivateServer activePrivateServer;
std::mutex privateServersMutex;
std::vector<PrivateServer> savedPrivateServers;
std::atomic<bool> privateServerResolving{ false };

static std::wstring ActivePrivateServerFilePath() { return g_exeDir + L"\\privateserver.dat"; }
static std::wstring PrivateServersFilePath()      { return g_exeDir + L"\\privateservers.dat"; }

static bool ParsePrivateServerLine(const std::string& line, PrivateServer& out) {
    std::istringstream ss(line);
    long long id = 0;
    std::string code;
    if (!(ss >> id >> code)) return false;
    std::string name;
    std::getline(ss, name);
    size_t b = name.find_first_not_of(" \t\r");
    size_t e = name.find_last_not_of(" \t\r");
    out.placeId = id;
    out.linkCode = code;
    out.name = (b == std::string::npos) ? std::string() : name.substr(b, e - b + 1);
    return !out.linkCode.empty();
}

static void WriteSavedPrivateServersLocked() {
    std::ofstream f(PrivateServersFilePath().c_str(), std::ios::trunc);
    if (!f) return;
    for (const auto& p : savedPrivateServers) f << p.placeId << ' ' << p.linkCode << ' ' << p.name << '\n';
}

void LoadSavedPrivateServers() {
    std::lock_guard<std::mutex> lock(privateServersMutex);
    std::wstring path = PrivateServersFilePath();
    if (!std::filesystem::exists(path)) return;
    std::ifstream f(path.c_str());
    std::string line;
    while (std::getline(f, line)) {
        PrivateServer ps;
        if (ParsePrivateServerLine(line, ps)) savedPrivateServers.push_back(ps);
    }
}

void AddSavedPrivateServer(const PrivateServer& ps) {
    if (ps.linkCode.empty()) return;
    std::lock_guard<std::mutex> lock(privateServersMutex);
    for (auto& p : savedPrivateServers) {
        if (p.linkCode == ps.linkCode) { p = ps; WriteSavedPrivateServersLocked(); return; }
    }
    savedPrivateServers.push_back(ps);
    WriteSavedPrivateServersLocked();
    AddActivity("Saved private server", ps.name.empty() ? ps.linkCode : ps.name);
}

void RemoveSavedPrivateServer(const std::string& linkCode) {
    std::lock_guard<std::mutex> lock(privateServersMutex);
    for (size_t i = 0; i < savedPrivateServers.size(); ++i) {
        if (savedPrivateServers[i].linkCode == linkCode) { savedPrivateServers.erase(savedPrivateServers.begin() + i); break; }
    }
    WriteSavedPrivateServersLocked();
}

void LoadActivePrivateServer() {
    std::wstring path = ActivePrivateServerFilePath();
    if (!std::filesystem::exists(path)) return;
    std::ifstream f(path.c_str());
    std::string line;
    if (!std::getline(f, line)) return;
    PrivateServer ps;
    if (!ParsePrivateServerLine(line, ps)) return;
    std::lock_guard<std::mutex> lock(activePrivateServerMutex);
    activePrivateServer = ps;
}

void SetActivePrivateServer(const PrivateServer& ps) {
    {
        std::lock_guard<std::mutex> lock(activePrivateServerMutex);
        activePrivateServer = ps;
    }
    std::ofstream f(ActivePrivateServerFilePath().c_str(), std::ios::trunc);
    if (f) f << ps.placeId << ' ' << ps.linkCode << ' ' << ps.name << '\n';
    Log("[v] Private server set (place " + std::to_string(ps.placeId) + ").");
    AddActivity("Private server selected", ps.name.empty() ? ps.linkCode : ps.name);
}

void ClearActivePrivateServer() {
    {
        std::lock_guard<std::mutex> lock(activePrivateServerMutex);
        activePrivateServer = PrivateServer{};
    }
    std::error_code ec;
    std::filesystem::remove(ActivePrivateServerFilePath(), ec);
    Log("[i] Private server cleared - launches will join the public game.");
}

static std::string QueryParam(const std::string& url, const std::string& key) {
    std::string pat = key + "=";
    size_t pos = url.find(pat);
    if (pos == std::string::npos) return "";
    if (pos > 0 && url[pos - 1] != '?' && url[pos - 1] != '&') return "";
    pos += pat.size();
    size_t end = url.find_first_of("&#", pos);
    return url.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

static std::string TrimCopy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string ResolveShareLink(const std::string& shareCode, const std::string& cookie) {
    std::string body = "{\"linkId\":\"" + shareCode + "\",\"linkType\":\"Server\"}";
    std::vector<std::pair<std::wstring, std::wstring>> headers = {
        { L"Referer", L"https://www.roblox.com/" },
        { L"Content-Type", L"application/json" },
    };

    std::wstring csrf;
    HttpResponse first = HttpRequest(L"apis.roblox.com", L"/sharelinks/v1/resolve-link", L"POST",
        cookie, headers, body, &csrf, nullptr);
    if (first.ok && first.status == 200) return first.body;

    if (csrf.empty()) {
        Log("[!] Could not obtain a CSRF token to resolve the share link (status " + std::to_string(first.status) + ").");
        return "";
    }
    headers.push_back({ L"X-CSRF-TOKEN", csrf });
    HttpResponse second = HttpRequest(L"apis.roblox.com", L"/sharelinks/v1/resolve-link", L"POST",
        cookie, headers, body, nullptr, nullptr);
    if (!second.ok || second.status != 200) {
        Log("[!] Share link resolve failed (status " + std::to_string(second.status) + ").");
        return "";
    }
    return second.body;
}

bool ResolvePrivateServerLink(const std::string& input, const std::string& cookie, PrivateServer& out) {
    std::string s = TrimCopy(input);
    if (s.empty()) { Log("[!] Paste a private server link first."); return false; }

    std::string direct = QueryParam(s, "privateServerLinkCode");
    if (!direct.empty()) {
        long long placeId = 0;
        size_t g = s.find("/games/");
        if (g != std::string::npos) { try { placeId = std::stoll(s.substr(g + 7)); } catch (...) {} }
        if (placeId <= 0) placeId = savedPlaceId.load();
        if (placeId <= 0) { Log("[!] Could not tell which place that private server link is for - set a Place ID first."); return false; }
        out.placeId = placeId;
        out.linkCode = direct;
        return true;
    }

    std::string code = QueryParam(s, "code");
    if (code.empty()) {
        if (s.find_first_of("/?&= ") != std::string::npos) {
            Log("[!] That does not look like a private server link.");
            return false;
        }
        code = s;
    }
    if (code.size() < 8) { Log("[!] That share code looks too short."); return false; }

    if (cookie.empty()) { Log("[!] Add an account first - share links can only be resolved while signed in."); return false; }

    privateServerResolving.store(true);
    std::string body = ResolveShareLink(code, cookie);
    privateServerResolving.store(false);
    if (body.empty()) return false;

    std::string linkCode = ExtractJsonStringField(body, "linkCode");
    long long placeId = ExtractJsonLongField(body, "placeId");
    if (linkCode.empty() || placeId <= 0) {
        Log("[!] The share link did not resolve to a private server (it may be expired or revoked).");
        return false;
    }
    std::string status = ExtractJsonStringField(body, "status");
    if (!status.empty() && status != "Valid") {
        Log("[!] Private server link status: " + status + ".");
        return false;
    }

    out.placeId = placeId;
    out.linkCode = linkCode;
    return true;
}

std::mutex robloxBuildMutex;
RobloxBuildState robloxBuild;
std::vector<std::string> downloadedBuilds;

static std::filesystem::path BuildsRoot() { return std::filesystem::path(g_exeDir) / "Builds"; }
static std::wstring ActiveBuildFilePath() { return g_exeDir + L"\\activebuild.dat"; }

struct PackageDir { const char* package; const char* dir; };
static const PackageDir kPlayerPackages[] = {
    { "RobloxApp.zip",                      ""                                              },
    { "WebView2.zip",                       ""                                              },
    { "shaders.zip",                        "shaders/"                                      },
    { "ssl.zip",                            "ssl/"                                          },
    { "content-avatar.zip",                 "content/avatar/"                               },
    { "content-configs.zip",                "content/configs/"                              },
    { "content-fonts.zip",                  "content/fonts/"                                },
    { "content-sky.zip",                    "content/sky/"                                  },
    { "content-sounds.zip",                 "content/sounds/"                               },
    { "content-textures2.zip",              "content/textures/"                             },
    { "content-models.zip",                 "content/models/"                               },
    { "content-platform-fonts.zip",         "PlatformContent/pc/fonts/"                     },
    { "content-platform-dictionaries.zip",  "PlatformContent/pc/shared_compression_dictionaries/" },
    { "content-terrain.zip",                "PlatformContent/pc/terrain/"                   },
    { "content-textures3.zip",              "PlatformContent/pc/textures/"                  },
    { "extracontent-luapackages.zip",       "ExtraContent/LuaPackages/"                     },
    { "extracontent-translations.zip",      "ExtraContent/translations/"                    },
    { "extracontent-models.zip",            "ExtraContent/models/"                          },
    { "extracontent-textures.zip",          "ExtraContent/textures/"                        },
    { "extracontent-places.zip",            "ExtraContent/places/"                          },
};

static const char* PackageDestination(const std::string& package) {
    for (const auto& p : kPlayerPackages) if (package == p.package) return p.dir;
    return nullptr;
}

static void SetBuildStatus(const std::string& text, float progress, bool busy) {
    std::lock_guard<std::mutex> lock(robloxBuildMutex);
    robloxBuild.status = text;
    robloxBuild.progress = progress;
    robloxBuild.busy = busy;
}

static void RescanDownloadedBuildsLocked() {
    downloadedBuilds.clear();
    std::error_code ec;
    if (!std::filesystem::exists(BuildsRoot(), ec)) return;
    for (auto& entry : std::filesystem::directory_iterator(BuildsRoot(), ec)) {
        if (ec || !entry.is_directory(ec)) continue;
        if (std::filesystem::exists(entry.path() / "RobloxPlayerBeta.exe", ec))
            downloadedBuilds.push_back(entry.path().filename().string());
    }
    std::sort(downloadedBuilds.begin(), downloadedBuilds.end());
}

void LoadRobloxBuilds() {
    std::string active, preferred;
    std::wstring path = ActiveBuildFilePath();
    if (std::filesystem::exists(path)) {
        std::ifstream f(path.c_str());
        std::getline(f, active);
        std::getline(f, preferred);
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
        };
        trim(active);
        trim(preferred);
        if (active == "-") active.clear();
        if (preferred.empty()) preferred = active;
    }
    std::lock_guard<std::mutex> lock(robloxBuildMutex);
    RescanDownloadedBuildsLocked();
    auto onDisk = [&](const std::string& h) {
        return !h.empty() && std::find(downloadedBuilds.begin(), downloadedBuilds.end(), h) != downloadedBuilds.end();
    };
    if (onDisk(active)) robloxBuild.activeVersion = active;
    if (onDisk(preferred)) robloxBuild.preferredVersion = preferred;
}

void SetActiveBuild(const std::string& versionHash) {
    std::string preferred;
    {
        std::lock_guard<std::mutex> lock(robloxBuildMutex);
        robloxBuild.activeVersion = versionHash;
        if (!versionHash.empty()) robloxBuild.preferredVersion = versionHash;
        preferred = robloxBuild.preferredVersion;
    }
    {
        std::ofstream f(ActiveBuildFilePath().c_str(), std::ios::trunc);
        if (f) f << (versionHash.empty() ? "-" : versionHash) << '\n' << preferred << '\n';
    }
    if (versionHash.empty()) {
        Log("[i] Using the system-installed Roblox client again.");
        AddActivity("Roblox build", "System install");
    } else {
        Log("[v] Launches will now use " + versionHash + ".");
        AddActivity("Roblox build activated", versionHash);
    }
}

void DeleteBuild(const std::string& versionHash) {
    if (versionHash.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(BuildsRoot() / versionHash, ec);
    bool wasActive;
    {
        std::lock_guard<std::mutex> lock(robloxBuildMutex);
        wasActive = (robloxBuild.activeVersion == versionHash);
        RescanDownloadedBuildsLocked();
    }
    if (wasActive) SetActiveBuild("");
    Log("[i] Removed build " + versionHash + ".");
}

void FetchWeaoVersions() {
    std::vector<std::pair<std::wstring, std::wstring>> hdrs = { { L"User-Agent", L"WEAO-3PService" } };
    HttpResponse cur = HttpRequest(L"weao.xyz", L"/api/versions/current", L"GET", "", hdrs, "");
    if (!cur.ok || cur.status != 200) {
        Log("[!] Could not reach the WEAO version API (status " + std::to_string(cur.status) + ").");
        return;
    }
    std::string live = ExtractJsonStringField(cur.body, "Windows");
    std::string liveDate = ExtractJsonStringField(cur.body, "WindowsDate");

    HttpResponse fut = HttpRequest(L"weao.xyz", L"/api/versions/future", L"GET", "", hdrs, "");
    std::string future;
    if (fut.ok && fut.status == 200) future = ExtractJsonStringField(fut.body, "Windows");
    if (future == live) future.clear();

    HttpResponse past = HttpRequest(L"weao.xyz", L"/api/versions/past", L"GET", "", hdrs, "");
    std::string prev, prevDate;
    if (past.ok && past.status == 200) {
        prev = ExtractJsonStringField(past.body, "Windows");
        prevDate = ExtractJsonStringField(past.body, "WindowsDate");
    }
    if (prev == live) prev.clear();

    std::lock_guard<std::mutex> lock(robloxBuildMutex);
    robloxBuild.liveVersion = live;
    robloxBuild.liveDate = liveDate;
    robloxBuild.futureVersion = future;
    robloxBuild.pastVersion = prev;
    robloxBuild.pastDate = prevDate;
    robloxBuild.weaoLoaded = !live.empty();
}

static bool DownloadToFile(const std::string& url, const std::filesystem::path& dest,
                           const std::function<void(float)>& onProgress = nullptr) {
    std::wstring host, path;
    if (!SplitHttpsUrl(url, host, path)) return false;

    HINTERNET hSession = WinHttpOpen(L"VelsMultiTool/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 30000, 30000, 60000, 60000);
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    bool ok = false;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            long long total = 0;
            {
                DWORD len = 0, lsz = sizeof(len);
                if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &len, &lsz, WINHTTP_NO_HEADER_INDEX))
                    total = (long long)len;
            }
            std::ofstream out(dest, std::ios::binary | std::ios::trunc);
            if (out) {
                ok = true;
                long long got = 0;
                DWORD available = 0;
                while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0) {
                    std::vector<char> buf(available);
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, buf.data(), available, &read)) { ok = false; break; }
                    out.write(buf.data(), read);
                    got += read;
                    if (onProgress && total > 0) onProgress((float)((double)got / (double)total));
                }
                out.close();
                if (ok && total > 0 && got < total) {
                    Log("[!] " + dest.filename().string() + " stopped short (" +
                        std::to_string(got) + " of " + std::to_string(total) + " bytes).");
                    ok = false;
                }
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    if (!ok) { std::error_code ec; std::filesystem::remove(dest, ec); }
    return ok;
}

static bool ExtractZip(const std::filesystem::path& zip, const std::filesystem::path& dest) {
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);

    std::wstring destArg = dest.wstring();
    while (!destArg.empty() && (destArg.back() == L'\\' || destArg.back() == L'/')) destArg.pop_back();

    std::wstring cmd = L"tar.exe -xf \"" + zip.wstring() + L"\" -C \"" + destArg + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi)) {
        Log("[!] Could not run tar.exe to unpack " + zip.filename().string() + ".");
        return false;
    }
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 10 * 60 * 1000) == WAIT_TIMEOUT) {
        Log("[!] tar.exe hung unpacking " + zip.filename().string() + " - killing it.");
        TerminateProcess(pi.hProcess, 1);
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

void DownloadRobloxBuild(std::string versionHash) {
    {
        std::lock_guard<std::mutex> lock(robloxBuildMutex);
        if (robloxBuild.busy) { Log("[!] A build download is already running."); return; }
        robloxBuild.busy = true;
    }
    struct BusyGuard {
        ~BusyGuard() { std::lock_guard<std::mutex> lock(robloxBuildMutex); robloxBuild.busy = false; }
    } guard;

    if (versionHash.empty()) {
        SetBuildStatus("Checking live version...", 0.0f, true);
        FetchWeaoVersions();
        std::lock_guard<std::mutex> lock(robloxBuildMutex);
        versionHash = robloxBuild.liveVersion;
    }
    if (versionHash.rfind("version-", 0) != 0) {
        SetBuildStatus("", 0.0f, false);
        Log("[!] That is not a Roblox version hash (they look like version-abc123...).");
        return;
    }

    std::filesystem::path buildDir = BuildsRoot() / versionHash;
    std::error_code ec;
    if (std::filesystem::exists(buildDir / "RobloxPlayerBeta.exe", ec)) {
        SetBuildStatus("", 0.0f, false);
        SetActiveBuild(versionHash);
        return;
    }

    SetBuildStatus("Fetching manifest...", 0.02f, true);
    std::string base = "https://setup.rbxcdn.com/" + versionHash + "-";
    std::vector<unsigned char> manifest = DownloadBinary(base + "rbxPkgManifest.txt");
    if (manifest.empty()) {
        SetBuildStatus("", 0.0f, false);
        Log("[!] No deployment found for " + versionHash + " - check the hash on rdd.weao.gg.");
        return;
    }

    std::vector<std::string> packages;
    {
        std::istringstream ms(std::string(manifest.begin(), manifest.end()));
        std::string line;
        while (std::getline(ms, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.size() > 4 && line.substr(line.size() - 4) == ".zip" && PackageDestination(line))
                packages.push_back(line);
        }
    }
    if (packages.empty()) {
        SetBuildStatus("", 0.0f, false);
        Log("[!] The manifest for " + versionHash + " had no player packages (wrong binary type?).");
        return;
    }

    std::filesystem::path tmpDir = buildDir / "_pkg";
    std::filesystem::create_directories(tmpDir, ec);

    for (size_t i = 0; i < packages.size(); ++i) {
        const std::string& pkg = packages[i];
        const float slice = 0.9f / (float)packages.size();
        const float base01 = 0.05f + slice * (float)i;
        std::string label = "Downloading " + pkg + " (" + std::to_string(i + 1) + "/" +
            std::to_string(packages.size()) + ")";
        SetBuildStatus(label, base01, true);

        std::filesystem::path zip = tmpDir / pkg;
        bool got = false;
        for (int attempt = 1; attempt <= 3 && !got; ++attempt) {
            std::string attemptLabel = attempt == 1 ? label : label + "  retry " + std::to_string(attempt) + "/3";
            SetBuildStatus(attemptLabel, base01, true);
            auto onProgress = [&](float f) { SetBuildStatus(attemptLabel, base01 + slice * 0.8f * f, true); };
            got = DownloadToFile(base + pkg, zip, onProgress);
            if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(800));
        }
        if (!got) {
            SetBuildStatus("", 0.0f, false);
            Log("[!] Failed to download " + pkg + " after 3 attempts - aborting.");
            std::filesystem::remove_all(buildDir, ec);
            return;
        }
        SetBuildStatus("Unpacking " + pkg + " (" + std::to_string(i + 1) + "/" +
            std::to_string(packages.size()) + ")", base01 + slice * 0.85f, true);
        if (!ExtractZip(zip, buildDir / PackageDestination(pkg))) {
            SetBuildStatus("", 0.0f, false);
            Log("[!] Failed to unpack " + pkg + " - aborting.");
            std::filesystem::remove_all(buildDir, ec);
            return;
        }
        std::filesystem::remove(zip, ec);
    }
    std::filesystem::remove_all(tmpDir, ec);

    {
        std::ofstream app(buildDir / "AppSettings.xml", std::ios::trunc);
        app << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
            << "<Settings>\r\n"
            << "\t<ContentFolder>content</ContentFolder>\r\n"
            << "\t<BaseUrl>http://www.roblox.com</BaseUrl>\r\n"
            << "</Settings>\r\n";
    }

    if (!std::filesystem::exists(buildDir / "RobloxPlayerBeta.exe", ec)) {
        SetBuildStatus("", 0.0f, false);
        Log("[!] " + versionHash + " unpacked without a player executable - removing it.");
        std::filesystem::remove_all(buildDir, ec);
        return;
    }

    { std::lock_guard<std::mutex> lock(robloxBuildMutex); RescanDownloadedBuildsLocked(); }
    SetBuildStatus("", 0.0f, false);
    Log("[v] Installed " + versionHash + ".");
    SetActiveBuild(versionHash);
}

void ForceLiveBuild() {
    SetBuildStatus("Checking live version...", 0.02f, true);
    FetchWeaoVersions();
    std::string live;
    { std::lock_guard<std::mutex> lock(robloxBuildMutex); live = robloxBuild.liveVersion; robloxBuild.busy = false; }
    if (live.empty()) {
        SetBuildStatus("", 0.0f, false);
        Log("[!] Could not determine the live version from WEAO.");
        return;
    }
    DownloadRobloxBuild(live);
}

void DownloadPreviousBuild() {
    SetBuildStatus("Checking previous version...", 0.02f, true);
    FetchWeaoVersions();
    std::string prev;
    { std::lock_guard<std::mutex> lock(robloxBuildMutex); prev = robloxBuild.pastVersion; robloxBuild.busy = false; }
    if (prev.empty()) {
        SetBuildStatus("", 0.0f, false);
        Log("[!] WEAO did not report a previous Windows version.");
        return;
    }
    DownloadRobloxBuild(prev);
}

void AddActivity(const std::string& title, const std::string& subtitle) {
    long long now = (long long)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(activityMutex);
    activityLog.insert(activityLog.begin(), ActivityEntry{ title, subtitle, now });
    if (activityLog.size() > 50) activityLog.resize(50);
}

std::string RelativeTimeString(long long unixSeconds) {
    long long now = (long long)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    long long diff = now - unixSeconds;
    if (diff < 0) diff = 0;
    if (diff < 60) return "just now";
    if (diff < 3600) { long long v = diff / 60; return std::to_string(v) + (v == 1 ? " minute ago" : " minutes ago"); }
    if (diff < 86400) { long long v = diff / 3600; return std::to_string(v) + (v == 1 ? " hour ago" : " hours ago"); }
    long long v = diff / 86400;
    return std::to_string(v) + (v == 1 ? " day ago" : " days ago");
}

void RefreshSystemStatus(int selectedAccountIndex) {
    SystemStatus s;

    HttpResponse apiResp = HttpRequest(L"users.roblox.com", L"/v1/users/1", L"GET", "", {}, "");
    s.robloxApiOk = apiResp.ok && apiResp.status == 200;

    std::string cookie;
    bool storeOk;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        if (selectedAccountIndex >= 0 && selectedAccountIndex < (int)accounts.size())
            cookie = accounts[selectedAccountIndex].cookie;
        storeOk = accounts.empty() || std::filesystem::exists(AccountsFilePath());
    }
    s.accountStoreOk = storeOk;

    if (!cookie.empty()) {
        HttpResponse authResp = HttpRequest(L"users.roblox.com", L"/v1/users/authenticated", L"GET", cookie, {}, "");
        s.authOk = authResp.ok && authResp.status == 200;
    }

    s.checked = true;
    std::lock_guard<std::mutex> lock(systemStatusMutex);
    systemStatus = s;
}

std::mutex updateMutex;
UpdateState updateState;
static std::string g_updateSha;

static const wchar_t* kUpdateRepoPath = L"/repos/x4qo/vels-roblox-multi-tool/contents?ref=main";
static const wchar_t* kUpdateCommitsPath = L"/repos/x4qo/vels-roblox-multi-tool/commits?path=VelsMultiTool.exe&per_page=1";
static const char* kUpdateExeUrl = "https://raw.githubusercontent.com/x4qo/vels-roblox-multi-tool/main/VelsMultiTool.exe";

static void SetUpdateState(const std::string& status, const std::string& message, float progress = 0.0f) {
    std::lock_guard<std::mutex> lock(updateMutex);
    updateState.status = status;
    updateState.message = message;
    updateState.progress = progress;
}

static std::string GitBlobSha(const std::wstring& file) {
    std::error_code ec;
    auto size = std::filesystem::file_size(file, ec);
    if (ec) return "";
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return "";
    std::string out;
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
        std::string header = "blob " + std::to_string(size);
        bool ok = CryptHashData(hash, (const BYTE*)header.c_str(), (DWORD)header.size() + 1, 0) != FALSE;
        std::vector<unsigned char> buf(1 << 16);
        size_t n;
        while (ok && (n = fread(buf.data(), 1, buf.size(), f)) > 0)
            ok = CryptHashData(hash, buf.data(), (DWORD)n, 0) != FALSE;
        BYTE digest[20];
        DWORD len = sizeof(digest);
        if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
            char hex[41];
            for (DWORD i = 0; i < len; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            out.assign(hex, len * 2);
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    fclose(f);
    return out;
}

static long long ExeBuildTime(const std::wstring& file) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return 0;
    unsigned int peOffset = 0, stamp = 0;
    bool ok = fseek(f, 0x3C, SEEK_SET) == 0 && fread(&peOffset, 4, 1, f) == 1 &&
              fseek(f, (long)peOffset + 8, SEEK_SET) == 0 && fread(&stamp, 4, 1, f) == 1;
    fclose(f);
    return ok ? (long long)stamp : 0;
}

static long long RemoteExeCommitTime() {
    HttpResponse r = HttpRequest(L"api.github.com", kUpdateCommitsPath, L"GET", "",
        { { L"Accept", L"application/vnd.github+json" } }, "");
    if (!r.ok || r.status != 200) return 0;
    static const std::regex dateRe("\"date\"\\s*:\\s*\"(\\d{4})-(\\d{2})-(\\d{2})T(\\d{2}):(\\d{2}):(\\d{2})Z\"");
    std::smatch m;
    if (!std::regex_search(r.body, m, dateRe)) return 0;
    std::tm t = {};
    t.tm_year = std::stoi(m[1]) - 1900; t.tm_mon = std::stoi(m[2]) - 1; t.tm_mday = std::stoi(m[3]);
    t.tm_hour = std::stoi(m[4]); t.tm_min = std::stoi(m[5]); t.tm_sec = std::stoi(m[6]);
    return (long long)_mkgmtime(&t);
}

void CheckForUpdate() {
    {
        std::lock_guard<std::mutex> lock(updateMutex);
        if (updateState.status == "checking" || updateState.status == "downloading" || updateState.status == "ready") return;
        updateState = { "checking", "", 0.0f };
    }
    HttpResponse r = HttpRequest(L"api.github.com", kUpdateRepoPath, L"GET", "",
        { { L"Accept", L"application/vnd.github+json" } }, "");
    if (!r.ok || r.status != 200) {
        SetUpdateState("error", r.status == 403 || r.status == 429 ? "GitHub is rate-limiting this network. Try again in a few minutes."
                                : "Could not reach GitHub" + (r.status ? " (status " + std::to_string(r.status) + ")." : "."));
        return;
    }
    static const std::regex shaRe("\"name\"\\s*:\\s*\"VelsMultiTool\\.exe\"[^}]*?\"sha\"\\s*:\\s*\"([0-9a-f]{40})\"");
    std::smatch m;
    if (!std::regex_search(r.body, m, shaRe)) {
        SetUpdateState("error", "GitHub did not list a prebuilt VelsMultiTool.exe.");
        return;
    }
    std::string remote = m[1].str(), local = GitBlobSha(SelfExePath());
    if (local.empty()) { SetUpdateState("error", "Could not read this exe to compare it."); return; }
    { std::lock_guard<std::mutex> lock(updateMutex); g_updateSha = remote; }
    if (remote == local) { SetUpdateState("current", "You're on the latest build (" + local.substr(0, 7) + ")."); return; }
    long long built = ExeBuildTime(SelfExePath()), published = RemoteExeCommitTime();
    if (built > 0 && published > 0 && built > published)
        SetUpdateState("current", "This build is newer than the one on GitHub (" + remote.substr(0, 7) + ").");
    else SetUpdateState("available", "A different build is on GitHub (" + remote.substr(0, 7) + "). You have " + local.substr(0, 7) + ".");
}

bool InstallUpdate() {
    std::string expected;
    {
        std::lock_guard<std::mutex> lock(updateMutex);
        if (updateState.status != "available" || g_updateSha.empty()) return false;
        expected = g_updateSha;
        updateState = { "downloading", "", 0.0f };
    }
    std::wstring self = SelfExePath();
    std::wstring fresh = self + L".update", old = self + L".old";
    bool got = DownloadToFile(kUpdateExeUrl, fresh, [](float p) {
        std::lock_guard<std::mutex> lock(updateMutex);
        updateState.progress = p;
    });
    std::error_code ec;
    if (!got) { SetUpdateState("error", "The download failed. Check your connection and try again."); return false; }
    if (GitBlobSha(fresh) != expected) {
        std::filesystem::remove(fresh, ec);
        SetUpdateState("error", "The download did not match the build on GitHub, so it was discarded. Check again.");
        return false;
    }
    if (!MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::filesystem::remove(fresh, ec);
        SetUpdateState("error", "Could not replace the exe (is its folder write-protected?).");
        return false;
    }
    if (!MoveFileExW(fresh.c_str(), self.c_str(), 0)) {
        MoveFileExW(old.c_str(), self.c_str(), 0);
        std::filesystem::remove(fresh, ec);
        SetUpdateState("error", "Could not put the new exe in place. Nothing was changed.");
        return false;
    }
    SetUpdateState("ready", "Update installed. Restarting…", 1.0f);
    Log("[v] Update " + expected.substr(0, 7) + " installed.");
    return true;
}

std::atomic<bool> updateNotify{ true };
std::atomic<bool> updateAuto{ false };

static std::wstring UpdateSettingsFilePath() { return g_exeDir + L"\\update.dat"; }

void SetUpdateSettings(bool notify, bool autoUpdate) {
    updateNotify = notify;
    updateAuto = autoUpdate;
    std::ofstream f(UpdateSettingsFilePath().c_str(), std::ios::trunc);
    if (f) f << (notify ? 1 : 0) << ' ' << (autoUpdate ? 1 : 0);
}

void StartupUpdateCheck() {
    {
        std::ifstream f(UpdateSettingsFilePath().c_str());
        int notify = 1, autoOn = 0;
        if (f >> notify >> autoOn) { updateNotify = notify != 0; updateAuto = autoOn != 0; }
    }
    if (!updateNotify && !updateAuto) return;
    CheckForUpdate();
    std::lock_guard<std::mutex> lock(updateMutex);
    if (updateState.status == "available") updateState.prompt = true;
    else if (updateState.status == "error") updateState = {};
}

std::mutex serversMutex;
ServerBrowserState serverBrowser;
static std::atomic<int> g_serversGen{ 0 };

static HttpResponse PostJsonAs(const std::wstring& host, const std::wstring& path, const std::string& cookie,
                               const std::string& body, std::vector<std::pair<std::wstring, std::wstring>> headers = {}) {
    headers.push_back({ L"Content-Type", L"application/json" });
    std::wstring csrf;
    HttpResponse r = HttpRequest(host, path, L"POST", cookie, headers, body, &csrf);
    if (r.status == 403 && !csrf.empty()) {
        headers.push_back({ L"X-CSRF-TOKEN", csrf });
        r = HttpRequest(host, path, L"POST", cookie, headers, body);
    }
    return r;
}

static std::map<long long, std::string> DataCenterNames() {
    static std::mutex m;
    static std::map<long long, std::string> names;
    std::lock_guard<std::mutex> lock(m);
    if (!names.empty()) return names;
    HttpResponse r = HttpRequest(L"apis.rovalra.com", L"/v1/datacenters/list", L"GET", "", {}, "");
    json::Value root;
    if (!r.ok || r.status != 200 || !json::Parse(r.body, root)) return names;
    for (const auto& loc : root.a) {
        std::string city = loc["location"]["city"].str(), cc = loc["location"]["country"].str();
        if (city.empty() && cc.empty()) continue;
        std::string label = city.empty() ? cc : city + (cc.empty() ? "" : ", " + cc);
        for (const auto& id : loc["dataCenterIds"].a) names[id.i64()] = label;
    }
    return names;
}

static std::string GeoLocateIp(const std::string& ip) {
    if (ip.empty()) return "";
    HttpResponse r = HttpRequest(L"ipinfo.io", L"/" + Widen(ip) + L"/json", L"GET", "", {}, "");
    json::Value v;
    if (!r.ok || r.status != 200 || !json::Parse(r.body, v)) return "";
    std::string city = v["city"].str(), cc = v["country"].str();
    return city.empty() ? cc : city + (cc.empty() ? "" : ", " + cc);
}

static std::string ServerRegion(long long placeId, const std::string& gameId, const std::string& cookie) {
    std::string body = "{\"placeId\":" + std::to_string(placeId) + ",\"gameId\":\"" + gameId +
                       "\",\"isTeleport\":false,\"gameJoinAttemptId\":\"" + gameId + "\"}";
    HttpResponse r = PostJsonAs(L"gamejoin.roblox.com", L"/v1/join-game-instance", cookie, body,
        { { L"User-Agent", L"Roblox/WinInet" }, { L"Referer", L"https://www.roblox.com/" } });
    json::Value v;
    if (!r.ok || r.status != 200 || !json::Parse(r.body, v)) return "";
    const json::Value& js = v["joinScript"];
    long long dc = js["DataCenterId"].i64();
    auto names = DataCenterNames();
    auto it = names.find(dc);
    if (dc > 0 && it != names.end()) return it->second;
    std::string ip = js["UdpJoinEndpoints"].a.empty() ? js["MachineAddress"].str() : js["UdpJoinEndpoints"].a[0]["Address"].str();
    return GeoLocateIp(ip);
}

void LoadServerBrowser(long long placeId, bool smallestFirst, long long regionUserId) {
    int gen = ++g_serversGen;
    {
        std::lock_guard<std::mutex> lock(serversMutex);
        serverBrowser = {};
        serverBrowser.placeId = placeId;
        serverBrowser.smallestFirst = smallestFirst;
        serverBrowser.loading = true;
    }
    if (placeId <= 0) {
        std::lock_guard<std::mutex> lock(serversMutex);
        serverBrowser.loading = false;
        serverBrowser.error = "Set a Place ID first.";
        return;
    }
    std::vector<ServerRow> rows;
    std::string cursor, error;
    for (int page = 0; page < 2; ++page) {
        std::wstring path = L"/v1/games/" + std::to_wstring(placeId) + L"/servers/Public?sortOrder=" +
            (smallestFirst ? L"Asc" : L"Desc") + L"&limit=100&excludeFullGames=false" +
            (cursor.empty() ? L"" : L"&cursor=" + Widen(UrlEncode(cursor)));
        HttpResponse r = HttpRequest(L"games.roblox.com", path, L"GET", "", {}, "");
        json::Value root;
        if (!r.ok || r.status != 200 || !json::Parse(r.body, root)) {
            if (rows.empty()) error = r.status == 429 ? "Roblox is rate-limiting the server list. Try again in a moment."
                                                      : "Could not load the server list (status " + std::to_string(r.status) + ").";
            break;
        }
        for (const auto& sv : root["data"].a) {
            ServerRow row;
            row.id = sv["id"].str();
            row.playing = (int)sv["playing"].i64();
            row.maxPlayers = (int)sv["maxPlayers"].i64();
            row.ping = (int)sv["ping"].i64();
            row.fps = (int)(sv["fps"].n + 0.5);
            if (!row.id.empty()) rows.push_back(row);
        }
        cursor = root["nextPageCursor"].str();
        if (cursor.empty()) break;
    }
    if (gen != g_serversGen) return;
    {
        std::lock_guard<std::mutex> lock(serversMutex);
        serverBrowser.rows = rows;
        serverBrowser.loading = false;
        serverBrowser.error = error.empty() && rows.empty() ? "No public servers are running for this place." : error;
    }

    std::string cookie;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        for (auto& a : accounts) if (a.userId == regionUserId) cookie = a.cookie;
        if (cookie.empty() && !accounts.empty()) cookie = accounts[0].cookie;
    }
    if (cookie.empty()) return;
    for (size_t i = 0; i < rows.size() && i < 60; ++i) {
        if (gen != g_serversGen) return;
        std::string region = ServerRegion(placeId, rows[i].id, cookie);
        if (gen != g_serversGen) return;
        {
            std::lock_guard<std::mutex> lock(serversMutex);
            if (i < serverBrowser.rows.size() && serverBrowser.rows[i].id == rows[i].id) {
                serverBrowser.rows[i].region = region;
                serverBrowser.rows[i].regionTried = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
    }
}

std::mutex friendsMutex;
FriendsState friendsState;

void LoadFriends(long long userId) {
    std::string cookie;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        for (auto& a : accounts) if (a.userId == userId) cookie = a.cookie;
    }
    {
        std::lock_guard<std::mutex> lock(friendsMutex);
        friendsState = {};
        friendsState.userId = userId;
        friendsState.loading = true;
    }
    auto finish = [&](const std::string& error, std::vector<FriendRow> rows, int total, int online) {
        std::lock_guard<std::mutex> lock(friendsMutex);
        if (friendsState.userId != userId) return;
        friendsState.loading = false;
        friendsState.error = error;
        friendsState.rows = std::move(rows);
        friendsState.total = total;
        friendsState.online = online;
    };
    if (cookie.empty()) { finish("Pick an account first.", {}, 0, 0); return; }

    HttpResponse fr = HttpRequest(L"friends.roblox.com", L"/v1/users/" + std::to_wstring(userId) + L"/friends", L"GET", cookie, {}, "");
    json::Value friends;
    if (!fr.ok || fr.status != 200 || !json::Parse(fr.body, friends)) {
        finish("Could not load this account's friends (status " + std::to_string(fr.status) + ").", {}, 0, 0);
        return;
    }
    std::vector<long long> ids;
    for (const auto& f : friends["data"].a) if (f["id"].i64() > 0) ids.push_back(f["id"].i64());

    std::vector<FriendRow> rows;
    int online = 0;
    for (size_t at = 0; at < ids.size(); at += 50) {
        std::string body = "{\"userIds\":[";
        for (size_t i = at; i < ids.size() && i < at + 50; ++i) body += (i > at ? "," : "") + std::to_string(ids[i]);
        body += "]}";
        HttpResponse pr = PostJsonAs(L"presence.roblox.com", L"/v1/presence/users", cookie, body);
        json::Value pres;
        if (!pr.ok || pr.status != 200 || !json::Parse(pr.body, pres)) continue;
        for (const auto& p : pres["userPresences"].a) {
            long long type = p["userPresenceType"].i64();
            if (type != 0) ++online;
            if (type != 2) continue;
            FriendRow row;
            row.id = p["userId"].i64();
            row.game = p["lastLocation"].str();
            row.gameId = p["gameId"].str();
            row.placeId = p["placeId"].i64();
            row.rootPlaceId = p["rootPlaceId"].i64();
            rows.push_back(row);
        }
    }
    if (!rows.empty()) {
        std::string idList, body = "{\"userIds\":[";
        for (size_t i = 0; i < rows.size(); ++i) { idList += (i ? "," : "") + std::to_string(rows[i].id); }
        body += idList + "],\"excludeBannedUsers\":false}";
        HttpResponse ur = PostJsonAs(L"users.roblox.com", L"/v1/users", cookie, body);
        json::Value users;
        if (ur.ok && ur.status == 200 && json::Parse(ur.body, users))
            for (const auto& u : users["data"].a)
                for (auto& row : rows) if (row.id == u["id"].i64()) { row.name = u["name"].str(); row.display = u["displayName"].str(); }
        HttpResponse tr = HttpRequest(L"thumbnails.roblox.com",
            L"/v1/users/avatar-headshot?userIds=" + Widen(idList) + L"&size=48x48&format=Png&isCircular=false", L"GET", "", {}, "");
        json::Value thumbs;
        if (tr.ok && tr.status == 200 && json::Parse(tr.body, thumbs))
            for (const auto& t : thumbs["data"].a)
                for (auto& row : rows) if (row.id == t["targetId"].i64()) row.avatarUrl = t["imageUrl"].str();
        for (auto& row : rows) if (row.name.empty()) row.name = "User " + std::to_string(row.id);
        std::sort(rows.begin(), rows.end(), [](const FriendRow& a, const FriendRow& b) {
            if (a.gameId.empty() != b.gameId.empty()) return !a.gameId.empty();
            return a.game < b.game;
        });
    }
    finish("", std::move(rows), (int)ids.size(), online);
}

}
