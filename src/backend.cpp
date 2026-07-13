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

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace backend {

std::mutex logMutex;
std::vector<LogEntry> logLines;

std::atomic<bool> watching{ false };
std::atomic<int> instanceCount{ 0 };

std::mutex adaptersMutex;
std::vector<NetworkAdapterInfo> adapters;
std::atomic<int> defaultAdapterIndex{ -1 };

std::mutex browserCookieMutex;
std::vector<BrowserCookieStatus> browserCookieStatus;
std::atomic<bool> browserCookieScanning{ false };
std::atomic<bool> browserCookieScanned{ false };

static std::wstring g_exeDir;
static std::thread g_watchThread;
static std::chrono::steady_clock::time_point g_startTime;
static std::mutex g_robloxCookieFileMutex;
static HANDLE g_robloxCookieFileHandle = INVALID_HANDLE_VALUE;
static HANDLE g_multiRobloxMutex = nullptr;

void Log(const std::string& msg) {
    (void)msg;
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

static void HoldMultiRobloxMutex() {
    if (g_multiRobloxMutex) return;

    g_multiRobloxMutex = CreateMutexW(nullptr, TRUE, L"ROBLOX_singletonMutex");
    if (!g_multiRobloxMutex) {
        Log("[!] Could not create ROBLOX_singletonMutex: " + LastWin32ErrorString(GetLastError()));
        return;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("[!] ROBLOX_singletonMutex already exists; multi-instance may depend on the watcher.");
    } else {
        Log("[v] Holding ROBLOX_singletonMutex for multi-instance launching.");
    }
}

static void ReleaseMultiRobloxMutex() {
    if (!g_multiRobloxMutex) return;
    ReleaseMutex(g_multiRobloxMutex);
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

void Init(const std::wstring& exeDir) {
    g_exeDir = exeDir;
    g_startTime = std::chrono::steady_clock::now();
    LoadAccounts();
    LoadPlaceId();
    ScrubAndLockRobloxCookieFile("startup");
    HoldMultiRobloxMutex();
}

void Shutdown() {
    StopWatching();
    ReleaseRobloxCookieFileLock();
    ReleaseMultiRobloxMutex();
}

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Multi-instance watcher
// ---------------------------------------------------------------------------

// One pass over every running RobloxPlayerBeta.exe: finds its
// ROBLOX_singletonEvent handle (if any) via handle64.exe and closes it, so
// another instance can be launched alongside it. Returns false only on a
// hard failure (handle64.exe missing) - not finding any locks isn't a
// failure, it just means nothing needed closing.
static bool CloseRobloxSingletonsOnce(const std::wstring& handleExe, const std::vector<DWORD>& pids) {
    // Same match the proven RobloxMulti.ps1 uses: require the handle *type*
    // to be an Event and pull out the validated hex handle id. The old parse
    // just grabbed everything before the first colon, which quietly failed
    // whenever handle64's column spacing/banner lines differed from what it
    // expected - so the singleton on already-open processes never got closed.
    static const std::regex kSingletonRe(
        R"(([0-9A-Fa-f]+):\s+Event\b.*ROBLOX_singletonEvent)",
        std::regex::icase);

    for (DWORD pid : pids) {
        std::wstring args = L"-p " + std::to_wstring(pid) + L" -a -nobanner";
        std::string raw = RunCaptureOutput(handleExe, args);

        std::istringstream stream(raw);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.find("ROBLOX_singletonEvent") == std::string::npos) continue;
            std::smatch m;
            if (!std::regex_search(line, m, kSingletonRe)) continue;
            std::string handleId = m[1].str();

            Log("[!] Singleton event found (handle " + handleId + ", pid " + std::to_string(pid) + ") - closing it...");
            std::wstring closeArgs = L"-c " + Widen(handleId) + L" -p " + std::to_wstring(pid) + L" -y -nobanner";
            std::string closeOut = RunCaptureOutput(handleExe, closeArgs);
            // handle64 needs to open the target process with enough rights to
            // close a handle inside it; without elevation it prints an error
            // and the lock survives, which looks like "it just doesn't work".
            std::string lowered = closeOut;
            for (char& c : lowered) c = (char)tolower((unsigned char)c);
            if (lowered.find("error") != std::string::npos ||
                lowered.find("access is denied") != std::string::npos ||
                lowered.find("could not") != std::string::npos) {
                Log("[!] Failed to close the singleton - try running Vels Multi Tool as Administrator.");
            } else {
                Log("[v] Done - you can open another instance now");
            }
        }
    }
    return true;
}

// Closes any current Roblox singleton locks right now, once, without
// starting the continuous background watcher. This is what the header's
// "Multi Instance" button runs - call from a worker thread.
void CloseRobloxSingletonsNow() {
    std::wstring handleExe = FindHandleExe();
    if (handleExe.empty()) {
        Log("[!] handle64.exe not found next to the exe. Place it in the same folder.");
        return;
    }
    RunCaptureOutput(handleExe, L"-accepteula");
    auto pids = FindPidsByName(L"RobloxPlayerBeta.exe");
    if (pids.empty()) {
        Log("[i] No Roblox instances running - nothing to unlock.");
        return;
    }
    Log("[i] Closing Roblox singleton lock(s) so another instance can launch...");
    CloseRobloxSingletonsOnce(handleExe, pids);
}

static void WatcherLoop() {
    std::wstring handleExe = FindHandleExe();
    if (handleExe.empty()) {
        Log("[!] handle64.exe not found next to the exe. Place it in the same folder.");
        watching = false;
        return;
    }

    RunCaptureOutput(handleExe, L"-accepteula");
    Log("[i] Watching for RobloxPlayerBeta.exe ...");
    int lastCount = -1;

    while (watching) {
        auto pids = FindPidsByName(L"RobloxPlayerBeta.exe");

        if ((int)pids.size() != lastCount) {
            lastCount = (int)pids.size();
            instanceCount = lastCount;
            if (!pids.empty()) Log("[i] Found " + std::to_string(pids.size()) + " Roblox process(es)");
            else Log("[i] Roblox closed, waiting...");
        }

        CloseRobloxSingletonsOnce(handleExe, pids);

        for (int i = 0; i < 10 && watching; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"roblox-player:", nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) Log("[!] Could not launch via roblox-player: protocol. Is Roblox installed?");
    else Log("[v] Launch requested. If the singleton lock blocks it, the watcher will clear it automatically.");
}

void KillAllRobloxInstances() {
    auto pids = FindPidsByName(L"RobloxPlayerBeta.exe");
    if (pids.empty()) {
        Log("[i] No Roblox instances running.");
        return;
    }
    int closed = 0;
    for (DWORD pid : pids) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (h) {
            if (TerminateProcess(h, 0)) ++closed;
            CloseHandle(h);
        }
    }
    Log("[v] Closed " + std::to_string(closed) + " of " + std::to_string(pids.size()) + " Roblox instance(s).");
}

// ---------------------------------------------------------------------------
// Live dashboard stats - throttled/cached since the UI polls these every frame
// ---------------------------------------------------------------------------
int CountRobloxProcesses(bool force) {
    static int cached = 0;
    static auto lastCheck = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    auto now = std::chrono::steady_clock::now();
    if (force || now - lastCheck > std::chrono::milliseconds(800)) {
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
        // Kernel time as reported by GetSystemTimes already includes idle time.
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

// ---------------------------------------------------------------------------
// Cookie clearing
// ---------------------------------------------------------------------------
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

// Chrome/Edge/Opera/Firefox all keep their cookie DB in WAL journal mode
// while the browser is running, so a just-written cookie often lives only in
// the "-wal" sidecar file, not yet checkpointed into the main DB file. A scan
// that only copies the main file silently sees zero rows for any cookie
// written since the last checkpoint - which is exactly the case where the
// user is actively signed in right now. Copy the sidecars alongside the main
// file (same naming convention SQLite itself uses) so the copy can recover
// them; this is best-effort since the sidecars may legitimately not exist
// (e.g. the browser already checkpointed and closed cleanly).
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

// Read-only: counts roblox.com cookie rows without modifying the browser's DB.
// Queries a throwaway copy so a locked/in-use Cookies file doesn't block the scan.
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
    // See countRobloxRows: without the WAL sidecar, a cookie written since
    // the browser's last checkpoint isn't in the main file we just copied,
    // so DELETE here would silently miss it and "Clear Cookies" would leave
    // the live session cookie intact.
    copyDbWithWalSidecars(dbPath, tmp);

    sqlite3* db = nullptr;
    if (sq.open(tmp.string().c_str(), &db) != SQLITE_OK) {
        if (db) sq.close(db);
        removeDbWithWalSidecars(tmp);
        return -1;
    }

    // Forcing DELETE journal mode checkpoints the WAL we just copied in back
    // into the main file and drops the -wal/-shm sidecars, so the single
    // `tmp` file renamed over dbPath below is the complete, consistent result.
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
    // std::system() shells out through cmd.exe, which briefly flashes a
    // console window since this app has none of its own. RunCaptureOutput
    // runs taskkill directly with CREATE_NO_WINDOW instead.
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

// Read-only inventory of which supported browsers are installed and whether they
// currently hold a roblox.com cookie. Never touches the live DB - always queries a
// throwaway copy - so it's safe to run automatically (e.g. every time the Cookie
// Cleaner tab is opened) without interrupting a running browser.
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
                // The profile directory exists but we found no "Cookies" DB anywhere
                // under it - that's not the same as "confirmed no cookies", it means
                // the scan itself couldn't locate anything to check.
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

void ClearRobloxCookieFileAndBrowsers() {
    Log("[i] Starting cookie cleanup...");
    ClearRobloxCookieFile();
    ClearBrowserCookies();
    Log("[v] Cookie cleanup finished.");
}

// ---------------------------------------------------------------------------
// MAC spoofing
// ---------------------------------------------------------------------------
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

// Returns the IP_ADAPTER_INFO::Index for the adapter matching this registry
// NetCfgInstanceID subkey, or -1 if not found / not bound to TCP/IP.
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

// Interface index Windows would use to reach the public internet right now
// (the live default route), or -1 if there isn't one.
static int getDefaultRouteIfIndex() {
    DWORD bestIfIndex = 0;
    IPAddr dest = 0x08080808; // 8.8.8.8 - byte-symmetric, so byte order doesn't matter
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

// ---------------------------------------------------------------------------
// Account manager: WinHTTP calls to Roblox's API + DPAPI-encrypted local store
// ---------------------------------------------------------------------------
std::mutex accountsMutex;
std::vector<RobloxAccount> accounts;

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

// Two-step CSRF dance: first POST gets rejected with the token in a response
// header, second POST (with that token) actually succeeds.
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

void SaveAccounts() {
    std::lock_guard<std::mutex> lock(accountsMutex);
    std::string buf;
    buf.append(kAccountsFileMagicV3, sizeof(kAccountsFileMagicV3));
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
    bool hasPassword = false, hasAlias = false;
    if (buf.size() >= sizeof(kAccountsFileMagicV3) &&
        memcmp(buf.data(), kAccountsFileMagicV3, sizeof(kAccountsFileMagicV3)) == 0) {
        hasPassword = true;
        hasAlias = true;
        pos = sizeof(kAccountsFileMagicV3);
    } else if (buf.size() >= sizeof(kAccountsFileMagicV2) &&
        memcmp(buf.data(), kAccountsFileMagicV2, sizeof(kAccountsFileMagicV2)) == 0) {
        hasPassword = true;
        pos = sizeof(kAccountsFileMagicV2);
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

void LaunchAccountIntoPlace(int index, long long placeId) {
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

    std::string placeLauncherUrl = "https://assetgame.roblox.com/game/PlaceLauncher.ashx?request=RequestGame"
        "&browserTrackerId=" + browserTrackerId + "&placeId=" + std::to_string(placeId) + "&isPlayTogetherGame=false";

    std::string uri = "roblox-player:1+launchmode:play+gameinfo:" + ticket +
        "+launchtime:" + std::to_string(launchTime) +
        "+placelauncherurl:" + UrlEncode(placeLauncherUrl) +
        "+browsertrackerid:" + browserTrackerId +
        "+robloxLocale:en_us+gameLocale:en_us+channel:+LaunchExp:InApp";

    int beforeCount = (int)FindPidsByName(L"RobloxPlayerBeta.exe").size();
    std::wstring wuri(uri.begin(), uri.end());
    HINSTANCE r = ShellExecuteW(nullptr, L"open", wuri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    bool launched = (INT_PTR)r > 32 && WaitForRobloxProcessCountAbove(beforeCount, 5000);
    if (!launched && !LaunchRobloxDirect(ticket, placeLauncherUrl)) {
        ScrubAndLockRobloxCookieFile(("after failed launch for " + account.username).c_str(), false);
        return;
    }

    Log("[v] Launched " + account.username + " into " + std::to_string(placeId) + ".");
    AddActivity("Launched Roblox", account.username);
    SchedulePostLaunchCookieScrub(account.username);
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

static std::mutex g_gmtimeMutex; // std::gmtime returns a pointer to shared static storage

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

void FetchPlaceInfo(long long placeId, const std::string& cookie) {
    if (placeId <= 0) return;

    std::string name;
    long long visits = -1, favorites = -1, universeId = 0;
    std::vector<unsigned char> icon;

    std::string creator;
    HttpResponse detailsResp = HttpRequest(L"games.roblox.com",
        L"/v1/games/multiget-place-details?placeIds=" + std::to_wstring(placeId), L"GET", cookie, {}, "");
    if (detailsResp.ok && detailsResp.status == 200) {
        name = ExtractJsonStringField(detailsResp.body, "name");
        creator = ExtractJsonStringField(detailsResp.body, "builder");
        universeId = ExtractJsonLongField(detailsResp.body, "universeId");
    }

    if (universeId > 0) {
        std::wstring uid = std::to_wstring(universeId);
        HttpResponse gamesResp = HttpRequest(L"games.roblox.com", L"/v1/games?universeIds=" + uid, L"GET", "", {}, "");
        if (gamesResp.ok && gamesResp.status == 200) visits = ExtractJsonLongField(gamesResp.body, "visits");

        HttpResponse favResp = HttpRequest(L"games.roblox.com", L"/v1/games/" + uid + L"/favorites/count", L"GET", "", {}, "");
        if (favResp.ok && favResp.status == 200) favorites = ExtractJsonLongField(favResp.body, "favoritesCount");
    }

    HttpResponse iconResp = HttpRequest(L"thumbnails.roblox.com",
        L"/v1/places/gameicons?placeIds=" + std::to_wstring(placeId) + L"&size=150x150&format=png&isCircular=false",
        L"GET", "", {}, "");
    if (iconResp.ok && iconResp.status == 200) {
        std::string imageUrl = ExtractJsonStringField(iconResp.body, "imageUrl");
        if (!imageUrl.empty()) icon = DownloadBinary(imageUrl);
    }

    std::lock_guard<std::mutex> lock(placeInfoMutex);
    placeInfo.placeId = placeId;
    placeInfo.name = name.empty() ? ("Place " + std::to_string(placeId)) : name;
    placeInfo.creator = creator;
    placeInfo.visits = visits;
    placeInfo.favorites = favorites;
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

} // namespace backend
