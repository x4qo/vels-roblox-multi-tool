#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <objbase.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <wincodec.h>

#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "windowscodecs.lib")

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "WebView2.h"
#include "backend.h"
#include "json.h"
#include "login.h"
#include "resource.h"

// The UI is a single HTML page served from the exe's resources on this
// virtual origin. Nothing on it ever reaches the network.
static const wchar_t* kAppOrigin = L"https://vels.example/";
static const wchar_t* kWindowTitle = L"Vels Multi Tool";
static const COLORREF kBgColor = RGB(11, 12, 14);
static COLORREF g_clientBg = kBgColor;
static const UINT_PTR kStateTimer = 1;
static const UINT_PTR kRevealTimer = 4;
static bool g_webviewShown = false;
static void RevealWebView();

// Native splash: the brand icon is painted on the window itself from the first
// frame, so the loading screen shows instantly. The animated HTML loader is
// revealed on top once WebView2 has composited it, hiding this seamlessly.
static HBITMAP g_splashBmp = nullptr;
static int g_splashW = 0, g_splashH = 0;

static const IID IID_EnvCompletedHandler  = { 0x4e8a3389, 0xc9d8, 0x4bd2, { 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d } };
static const IID IID_CtrlCompletedHandler = { 0x6c4819f3, 0xc9b7, 0x4260, { 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c } };
static const IID IID_WebMessageHandler    = { 0x57213f19, 0x00e6, 0x49fa, { 0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2 } };
static const IID IID_WebResourceHandler   = { 0xab00b74c, 0x15f1, 0x4646, { 0x80, 0xe8, 0xe7, 0x63, 0x41, 0xd2, 0x5d, 0x71 } };
static const IID IID_NewWindowHandler     = { 0xd4c185fe, 0xc81c, 0x4989, { 0x97, 0xaf, 0x2d, 0x3f, 0xa7, 0xab, 0x56, 0x51 } };
static const IID IID_Controller2          = { 0xc979903e, 0xd4ca, 0x4228, { 0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab } };

static std::wstring g_exeDir;
static HWND g_hwnd = nullptr;
static ICoreWebView2Environment* g_env = nullptr;
static ICoreWebView2Controller* g_controller = nullptr;
static ICoreWebView2* g_webview = nullptr;
static bool g_pageReady = false;
static std::string g_lastState;
static long long g_logSeen = 0;
static long long g_placeFetchKicked = 0;
static std::atomic<bool> g_loginInProgress{ false };
static std::atomic<bool> g_macBusy{ false };
static std::atomic<bool> g_cookieBusy{ false };
static std::atomic<bool> g_multiBusy{ false };

// Elevation hand-off: the non-admin window stays up while the admin copy
// loads underneath it, then the admin copy closes it.
static const UINT kMsgElevateResult = WM_APP + 1;
static const UINT_PTR kHandoffTimer = 2;
static const UINT_PTR kRetireFailsafeTimer = 3;
static std::atomic<bool> g_elevating{ false };
static bool g_retiring = false;
static HWND g_handoffFrom = nullptr;
static bool g_handoffDone = false;

template <typename TInterface, typename... TArgs>
class ComHandler final : public TInterface {
public:
    ComHandler(const IID& iid, std::function<HRESULT(TArgs...)> fn) : m_iid(iid), m_fn(std::move(fn)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, m_iid)) {
            *ppv = static_cast<TInterface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(TArgs... args) override { return m_fn(args...); }

private:
    IID m_iid;
    std::function<HRESULT(TArgs...)> m_fn;
    LONG m_ref = 1;
};

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string Narrow(const wchar_t* w) {
    if (!w || !*w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static bool LoadResourceBytes(int id, const BYTE*& data, DWORD& size) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!res) return false;
    HGLOBAL mem = LoadResource(nullptr, res);
    if (!mem) return false;
    data = static_cast<const BYTE*>(LockResource(mem));
    size = SizeofResource(nullptr, res);
    return data && size;
}

static bool CopyToClipboard(const std::string& utf8) {
    std::wstring w = Widen(utf8);
    if (!OpenClipboard(g_hwnd)) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        if (auto* dst = static_cast<wchar_t*>(GlobalLock(h))) {
            memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

static bool EnsureElevatedFor(const wchar_t* feature) {
    if (backend::IsElevated()) return true;
    std::wstring msg = std::wstring(feature) +
        L" needs administrator rights.\n\nRestart Vels Multi Tool as administrator now?";
    if (MessageBoxW(g_hwnd, msg.c_str(), kWindowTitle, MB_YESNO | MB_ICONWARNING) == IDYES) {
        if (backend::RelaunchAsAdmin()) {
            backend::Shutdown();
            ExitProcess(0);
        }
        MessageBoxW(g_hwnd, L"Could not relaunch as administrator.", kWindowTitle, MB_OK | MB_ICONERROR);
    }
    return false;
}

static std::wstring OpenCookieFileDialog(HWND owner) {
    wchar_t file[2048] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Cookie / text files\0*.txt;*.dat;*.csv;*.log\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 2048;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) return std::wstring(file);
    return L"";
}

static void ImportCookieLines(const std::string& data, const std::string& source) {
    int added = 0, tried = 0;
    size_t start = 0;
    while (start <= data.size()) {
        size_t nl = data.find('\n', start);
        std::string line = data.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        start = (nl == std::string::npos) ? data.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        line = line.substr(b);
        const std::string pfx = ".ROBLOSECURITY=";
        if (line.rfind(pfx, 0) == 0) line = line.substr(pfx.size());
        if (line.size() < 40) continue;
        tried++;
        if (backend::AddAccountFromCookie(line)) added++;
    }
    if (tried == 0) backend::Log("[!] " + source + ": no .ROBLOSECURITY cookies found.");
    else backend::Log("[i] " + source + ": added " + std::to_string(added) + " of " + std::to_string(tried) + ".");
}

static int FindUserIndexLocked(long long userId) {
    for (size_t i = 0; i < backend::accounts.size(); ++i)
        if (backend::accounts[i].userId == userId) return (int)i;
    return -1;
}

static int IndexOfUser(long long userId) {
    std::lock_guard<std::mutex> lock(backend::accountsMutex);
    return FindUserIndexLocked(userId);
}

static std::string FirstAccountCookie() {
    std::lock_guard<std::mutex> lock(backend::accountsMutex);
    return backend::accounts.empty() ? std::string() : backend::accounts[0].cookie;
}

// Resolves ids from the page to accounts that still exist, in list order,
// so priority accounts (always at the top) launch first.
static std::vector<long long> OrderedIds(const json::Value& list) {
    std::vector<std::pair<int, long long>> found;
    {
        std::lock_guard<std::mutex> lock(backend::accountsMutex);
        for (const auto& v : list.a) {
            long long id = v.i64();
            int idx = FindUserIndexLocked(id);
            if (idx >= 0) found.emplace_back(idx, id);
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    std::vector<long long> ids;
    for (auto& f : found) ids.push_back(f.second);
    return ids;
}

static void FetchPlaceAsync(long long placeId) {
    if (placeId <= 0) return;
    g_placeFetchKicked = placeId;
    std::string cookie = FirstAccountCookie();
    std::thread([placeId, cookie]() { backend::FetchPlaceInfo(placeId, cookie); }).detach();
}

static const char* B(bool v) { return v ? "true" : "false"; }
static std::string N(long long v) { return std::to_string(v); }

static std::string BuildStateJson() {
    using json::Quote;
    std::string o;
    o.reserve(8192);

    backend::PruneLaunchedPids();
    std::map<long long, unsigned long> pidMap;
    { std::lock_guard<std::mutex> lock(backend::launchedMutex); pidMap = backend::launchedPids; }

    o += "{\"type\":\"state\",\"accounts\":[";
    {
        std::lock_guard<std::mutex> lock(backend::accountsMutex);
        for (size_t i = 0; i < backend::accounts.size(); ++i) {
            RobloxAccount& a = backend::accounts[i];
            if (!a.avatarRequested && a.userId > 0) {
                a.avatarRequested = true;
                std::thread(backend::FetchAccountAvatar, (int)i).detach();
            }
            if (i) o += ',';
            o += "{\"id\":" + N(a.userId);
            o += ",\"username\":" + Quote(a.username);
            o += ",\"alias\":" + Quote(a.alias);
            o += ",\"group\":" + Quote(a.group);
            auto pit = pidMap.find(a.userId);
            o += ",\"pid\":" + N(pit == pidMap.end() ? 0 : (long long)pit->second);
            o += std::string(",\"priority\":") + B(a.priority);
            o += std::string(",\"avatar\":") + B(a.avatarLoaded && !a.avatarPng.empty());
            o += std::string(",\"hasPassword\":") + B(!a.password.empty());
            o += '}';
        }
    }
    o += ']';

    long long placeId = backend::savedPlaceId.load();
    o += ",\"placeId\":" + N(placeId);
    {
        bool loaded = false, hasIcon = false, isSub = false;
        long long root = 0, playing = -1, visits = -1, favorites = -1;
        std::string name, placeName, creator;
        {
            std::lock_guard<std::mutex> lock(backend::placeInfoMutex);
            const PlaceInfo& pi = backend::placeInfo;
            if (placeId > 0 && pi.loaded && pi.placeId == placeId) {
                loaded = true;
                hasIcon = !pi.iconPng.empty();
                isSub = pi.isSubPlace;
                root = pi.rootPlaceId;
                playing = pi.playing;
                visits = pi.visits;
                favorites = pi.favorites;
                name = pi.name;
                placeName = pi.placeName;
                creator = pi.creator;
            }
        }
        if (!loaded && placeId > 0 && g_placeFetchKicked != placeId) FetchPlaceAsync(placeId);

        o += std::string(",\"place\":{\"loaded\":") + B(loaded);
        o += std::string(",\"icon\":") + B(hasIcon);
        o += std::string(",\"isSub\":") + B(isSub);
        o += ",\"rootPlaceId\":" + N(root);
        o += ",\"name\":" + Quote(name);
        o += ",\"placeName\":" + Quote(placeName);
        o += ",\"creator\":" + Quote(creator);
        o += ",\"playing\":" + N(playing);
        o += ",\"visits\":" + N(visits);
        o += ",\"favorites\":" + N(favorites);
        o += '}';
    }

    o += ",\"savedPlaces\":[";
    {
        std::lock_guard<std::mutex> lock(backend::savedPlacesMutex);
        for (size_t i = 0; i < backend::savedPlaces.size(); ++i) {
            if (i) o += ',';
            o += "{\"id\":" + N(backend::savedPlaces[i].id) + ",\"name\":" + Quote(backend::savedPlaces[i].name) +
                 std::string(",\"favorite\":") + B(backend::savedPlaces[i].favorite) + "}";
        }
    }
    o += ']';

    backend::PrivateServer active;
    { std::lock_guard<std::mutex> lock(backend::activePrivateServerMutex); active = backend::activePrivateServer; }
    std::vector<backend::PrivateServer> savedPs;
    { std::lock_guard<std::mutex> lock(backend::privateServersMutex); savedPs = backend::savedPrivateServers; }
    std::string activeName = active.name;
    if (!active.linkCode.empty())
        for (auto& p : savedPs) if (p.linkCode == active.linkCode) { activeName = p.name; break; }

    o += ",\"ps\":{\"code\":" + Quote(active.linkCode);
    o += ",\"placeId\":" + N(active.placeId);
    o += ",\"name\":" + Quote(activeName);
    o += std::string(",\"resolving\":") + B(backend::privateServerResolving.load());
    o += "},\"savedPs\":[";
    for (size_t i = 0; i < savedPs.size(); ++i) {
        if (i) o += ',';
        o += "{\"code\":" + Quote(savedPs[i].linkCode) + ",\"placeId\":" + N(savedPs[i].placeId) +
             ",\"name\":" + Quote(savedPs[i].name) + "}";
    }
    o += ']';

    backend::RobloxBuildState bs;
    std::vector<std::string> builds;
    {
        std::lock_guard<std::mutex> lock(backend::robloxBuildMutex);
        bs = backend::robloxBuild;
        builds = backend::downloadedBuilds;
    }
    char progress[32];
    snprintf(progress, sizeof(progress), "%.3f", bs.progress);
    o += ",\"build\":{\"live\":" + Quote(bs.liveVersion);
    o += ",\"liveDate\":" + Quote(bs.liveDate);
    o += ",\"past\":" + Quote(bs.pastVersion);
    o += ",\"pastDate\":" + Quote(bs.pastDate);
    o += ",\"future\":" + Quote(bs.futureVersion);
    o += std::string(",\"weao\":") + B(bs.weaoLoaded);
    o += ",\"active\":" + Quote(bs.activeVersion);
    o += ",\"preferred\":" + Quote(bs.preferredVersion);
    o += ",\"status\":" + Quote(bs.status);
    o += std::string(",\"progress\":") + progress;
    o += std::string(",\"busy\":") + B(bs.busy);
    o += ",\"downloaded\":[";
    for (size_t i = 0; i < builds.size(); ++i) {
        if (i) o += ',';
        o += Quote(builds[i]);
    }
    o += "]}";

    o += std::string(",\"net\":{\"busy\":") + B(g_macBusy.load());
    o += std::string(",\"cookieBusy\":") + B(g_cookieBusy.load());
    o += ",\"active\":" + N(backend::defaultAdapterIndex.load());
    o += ",\"adapters\":[";
    {
        std::lock_guard<std::mutex> lock(backend::adaptersMutex);
        for (size_t i = 0; i < backend::adapters.size(); ++i) {
            const NetworkAdapterInfo& ad = backend::adapters[i];
            if (i) o += ',';
            o += "{\"name\":" + Quote(ad.connectionName) + ",\"desc\":" + Quote(ad.description) +
                 ",\"mac\":" + Quote(ad.currentMac) + std::string(",\"active\":") + B(ad.isActive) + "}";
        }
    }
    o += "]}";

    static const bool elevated = backend::IsElevated();
    o += std::string(",\"multi\":") + B(backend::watching.load());
    o += ",\"alive\":" + N(backend::CountRobloxProcesses());
    o += ",\"cpu\":" + N((long long)(backend::GetCpuUsagePercent() + 0.5f));
    o += std::string(",\"elevated\":") + B(elevated);
    o += std::string(",\"loginBusy\":") + B(g_loginInProgress.load());

    o += std::string(",\"bestServer\":") + B(backend::joinBestServer.load());
    {
        std::lock_guard<std::mutex> lock(backend::lastServerMutex);
        o += ",\"lastServer\":{\"placeId\":" + N(backend::lastServer.placeId) +
             std::string(",\"has\":") + B(backend::lastServer.placeId > 0 && !backend::lastServer.gameId.empty()) + "}";
    }
    o += '}';
    return o;
}

static void PostToPage(const std::string& jsonText) {
    if (g_webview && g_pageReady) g_webview->PostWebMessageAsJson(Widen(jsonText).c_str());
}

static void FlushLogs() {
    std::vector<LogEntry> fresh;
    {
        std::lock_guard<std::mutex> lock(backend::logMutex);
        long long total = backend::logTotal;
        long long firstIdx = total - (long long)backend::logLines.size();
        for (long long k = std::max(g_logSeen, firstIdx); k < total; ++k)
            fresh.push_back(backend::logLines[(size_t)(k - firstIdx)]);
        g_logSeen = total;
    }
    if (fresh.empty()) return;
    std::string o = "{\"type\":\"log\",\"items\":[";
    for (size_t i = 0; i < fresh.size(); ++i) {
        if (i) o += ',';
        o += "{\"time\":" + json::Quote(fresh[i].time) + ",\"text\":" + json::Quote(fresh[i].text) + "}";
    }
    o += "]}";
    PostToPage(o);
}

static void Tick() {
    if (!g_pageReady || !g_webview || g_retiring) return;
    FlushLogs();
    std::string state = BuildStateJson();
    if (state != g_lastState) {
        g_lastState = std::move(state);
        PostToPage(g_lastState);
    }
}

static void HandlePageMessage(const std::string& text) {
    json::Value m;
    if (!json::Parse(text, m) || m.type != json::Value::Object) return;
    const std::string cmd = m["cmd"].str();
    if (g_retiring) return;

    if (cmd == "ready") {
        g_pageReady = true;
        g_lastState.clear();
        RevealWebView();
        if (g_handoffFrom && !g_handoffDone) SetTimer(g_hwnd, kHandoffTimer, 350, nullptr);
        { std::lock_guard<std::mutex> lock(backend::logMutex); g_logSeen = backend::logTotal; }
        static bool weaoKicked = false;
        if (!weaoKicked) {
            weaoKicked = true;
            std::thread([]() { backend::FetchWeaoVersions(); }).detach();
            std::thread([]() { backend::RefreshAdapters(); }).detach();
        }
    } else if (cmd == "macSpoof" || cmd == "macRevert") {
        if (g_macBusy.load()) return;
        if (!EnsureElevatedFor(L"Changing your MAC address")) return;
        int index = (int)m["index"].i64();
        bool spoof = cmd == "macSpoof";
        g_macBusy.store(true);
        std::thread([index, spoof]() {
            if (spoof) backend::SpoofAdapter(index);
            else backend::RestoreAdapter(index);
            g_macBusy.store(false);
        }).detach();
    } else if (cmd == "refreshAdapters") {
        std::thread([]() { backend::RefreshAdapters(); }).detach();
    } else if (cmd == "clearCookies") {
        if (!g_cookieBusy.exchange(true)) {
            std::thread([]() {
                backend::ClearRobloxCookieFiles();
                g_cookieBusy.store(false);
            }).detach();
        }
    } else if (cmd == "toggleMulti") {
        if (g_multiBusy.exchange(true)) return;
        if (backend::watching.load()) {
            std::thread([]() { backend::StopWatching(); g_multiBusy.store(false); }).detach();
        } else {
            if (EnsureElevatedFor(L"Multi Instance (closing Roblox singleton handles)")) backend::StartWatching();
            g_multiBusy.store(false);
        }
    } else if (cmd == "relaunchAdmin") {
        bool multi = m["multi"].boolean();
        if (backend::IsElevated()) {
            if (multi && !backend::watching.load()) backend::StartWatching();
        } else if (!g_elevating.exchange(true)) {
            std::wstring args = L"--handoff=" + std::to_wstring((long long)(INT_PTR)g_hwnd);
            if (multi) args += L" --multi";
            PostToPage("{\"type\":\"relaunch\",\"stage\":\"pending\"}");
            HWND owner = g_hwnd;
            std::thread([args, owner]() {
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                wchar_t path[MAX_PATH];
                GetModuleFileNameW(nullptr, path, MAX_PATH);
                SHELLEXECUTEINFOW sei = {};
                sei.cbSize = sizeof(sei);
                sei.lpVerb = L"runas";
                sei.lpFile = path;
                sei.lpParameters = args.c_str();
                sei.hwnd = owner;
                sei.nShow = SW_SHOWNOACTIVATE;
                BOOL ok = ShellExecuteExW(&sei);
                CoUninitialize();
                PostMessageW(owner, kMsgElevateResult, ok ? 1 : 0, 0);
            }).detach();
        }
    } else if (cmd == "killAll") {
        std::thread([]() { backend::KillAllRobloxInstances(); }).detach();
    } else if (cmd == "launchClient") {
        std::vector<long long> ids = OrderedIds(m["ids"]);
        if (ids.empty()) {
            // No selection: sign in as the top account, or open a plain client if there are none.
            std::lock_guard<std::mutex> lock(backend::accountsMutex);
            if (!backend::accounts.empty()) ids.push_back(backend::accounts[0].userId);
        }
        if (ids.empty()) {
            std::thread([]() { backend::LaunchRobloxClient(); }).detach();
        } else {
            std::thread([ids]() {
                for (size_t k = 0; k < ids.size(); ++k) {
                    int idx = IndexOfUser(ids[k]);
                    if (idx >= 0) backend::LaunchAccountClient(idx);
                    if (k + 1 < ids.size()) Sleep(300);
                }
            }).detach();
        }
    } else if (cmd == "launch") {
        std::vector<long long> ids = OrderedIds(m["ids"]);
        backend::PrivateServer aps;
        { std::lock_guard<std::mutex> lock(backend::activePrivateServerMutex); aps = backend::activePrivateServer; }
        long long placeId = aps.linkCode.empty() ? backend::savedPlaceId.load() : aps.placeId;
        if (placeId <= 0) backend::Log("[!] Set a Place ID first, then launch.");
        else if (ids.empty()) backend::Log("[!] Select at least one account to launch.");
        else {
            std::string code = aps.linkCode;
            bool best = code.empty() && backend::joinBestServer.load();
            std::thread([ids, placeId, code, best]() {
                // For best-ping, resolve one server for the whole batch so every
                // selected account lands in the same low-ping server (one API call).
                std::string gameId = best ? backend::FindBestServer(placeId) : "";
                for (size_t k = 0; k < ids.size(); ++k) {
                    int idx = IndexOfUser(ids[k]);
                    if (idx < 0) continue;
                    if (!code.empty()) backend::LaunchAccountIntoPrivateServer(idx, placeId, code);
                    else if (!gameId.empty()) backend::LaunchAccountIntoServer(idx, placeId, gameId);
                    else backend::LaunchAccountIntoPlace(idx, placeId);
                    if (k + 1 < ids.size()) Sleep(300);
                }
            }).detach();
        }
    } else if (cmd == "setBestServer") {
        backend::SetJoinBestServer(m["value"].boolean());
    } else if (cmd == "rejoinLast") {
        std::vector<long long> ids = OrderedIds(m["ids"]);
        if (ids.empty()) {
            std::lock_guard<std::mutex> lock(backend::accountsMutex);
            if (!backend::accounts.empty()) ids.push_back(backend::accounts[0].userId);
        }
        long long placeId = 0;
        std::string gameId;
        { std::lock_guard<std::mutex> lock(backend::lastServerMutex);
          placeId = backend::lastServer.placeId; gameId = backend::lastServer.gameId; }
        if (placeId <= 0 || gameId.empty()) backend::Log("[!] No last server to rejoin yet - launch into a chosen server first.");
        else if (ids.empty()) backend::Log("[!] Select at least one account to rejoin.");
        else {
            std::thread([ids, placeId, gameId]() {
                for (size_t k = 0; k < ids.size(); ++k) {
                    int idx = IndexOfUser(ids[k]);
                    if (idx < 0) continue;
                    backend::LaunchAccountIntoServer(idx, placeId, gameId);
                    if (k + 1 < ids.size()) Sleep(300);
                }
            }).detach();
        }
    } else if (cmd == "browser") {
        std::vector<long long> ids = OrderedIds(m["ids"]);
        std::thread([ids]() {
            for (long long id : ids) {
                int idx = IndexOfUser(id);
                if (idx >= 0) backend::OpenAccountWeb(idx);
            }
        }).detach();
    } else if (cmd == "remove") {
        std::vector<int> idxs;
        {
            std::lock_guard<std::mutex> lock(backend::accountsMutex);
            for (const auto& v : m["ids"].a) {
                int idx = FindUserIndexLocked(v.i64());
                if (idx >= 0) idxs.push_back(idx);
            }
        }
        std::sort(idxs.rbegin(), idxs.rend());
        idxs.erase(std::unique(idxs.begin(), idxs.end()), idxs.end());
        for (int idx : idxs) backend::RemoveAccount(idx);
    } else if (cmd == "move") {
        backend::MoveAccount(IndexOfUser(m["id"].i64()), (int)m["to"].i64());
    } else if (cmd == "priority") {
        bool value = m["value"].boolean();
        for (long long id : OrderedIds(m["ids"])) backend::SetAccountPriority(IndexOfUser(id), value);
    } else if (cmd == "setGroup") {
        std::string group = m["group"].str();
        for (const auto& v : m["ids"].a) backend::SetAccountGroup(IndexOfUser(v.i64()), group);
    } else if (cmd == "copy") {
        std::string what = m["what"].str();
        std::string uname, pass, cookie;
        {
            std::lock_guard<std::mutex> lock(backend::accountsMutex);
            int idx = FindUserIndexLocked(m["id"].i64());
            if (idx < 0) return;
            uname = backend::accounts[idx].username;
            pass = backend::accounts[idx].password;
            cookie = backend::accounts[idx].cookie;
        }
        std::string value, label;
        if (what == "cookie") { label = "cookie"; if (!cookie.empty()) value = ".ROBLOSECURITY=" + cookie; }
        else if (what == "userpass") { label = "user:pass"; if (!pass.empty()) value = uname + ":" + pass; }
        else if (what == "username") { label = "username"; value = uname; }
        else if (what == "password") { label = "password"; value = pass; }
        else return;
        if (value.empty()) backend::Log("[!] No " + label + " saved for " + uname + ".");
        else if (CopyToClipboard(value)) backend::Log("[v] Copied " + label + " for " + uname + ".");
        else backend::Log("[!] Could not access the clipboard.");
    } else if (cmd == "editAccount") {
        int idx = IndexOfUser(m["id"].i64());
        if (idx < 0) return;
        std::string currentAlias;
        { std::lock_guard<std::mutex> lock(backend::accountsMutex); currentAlias = backend::accounts[idx].alias; }
        std::string alias = m["alias"].str();
        if (alias != currentAlias) backend::SetAccountAlias(idx, alias);
        if (m["setPassword"].boolean()) backend::SetAccountPassword(idx, m["password"].str());
    } else if (cmd == "login") {
        if (!g_loginInProgress.exchange(true)) {
            std::thread([]() {
                login::ShowRobloxLoginWindow(g_exeDir, [](bool ok, std::string cookie) {
                    if (ok) backend::AddAccountFromCookie(cookie);
                    else backend::Log("[i] Login cancelled.");
                    g_loginInProgress.store(false);
                });
            }).detach();
        }
    } else if (cmd == "pasteCookies") {
        std::string data = m["text"].str();
        std::thread([data]() { ImportCookieLines(data, "Pasted cookies"); }).detach();
    } else if (cmd == "loadFile") {
        std::wstring path = OpenCookieFileDialog(g_hwnd);
        if (!path.empty()) {
            std::thread([path]() {
                FILE* f = _wfopen(path.c_str(), L"rb");
                if (!f) { backend::Log("[!] Could not open cookie file."); return; }
                std::string data;
                char buf[8192];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
                fclose(f);
                ImportCookieLines(data, "Cookie file");
            }).detach();
        }
    } else if (cmd == "setPlace") {
        long long id = std::max(0LL, m["id"].i64());
        backend::SavePlaceId(id);
        FetchPlaceAsync(id);
    } else if (cmd == "savePlace") {
        long long cur = m.has("id") ? m["id"].i64() : backend::savedPlaceId.load();
        std::string name = m["name"].str();
        if (cur <= 0) backend::Log("[!] Set a Place ID before saving it.");
        else {
            if (name.empty()) name = "Place " + std::to_string(cur);
            backend::AddSavedPlace(cur, name);
            backend::Log("[v] Saved place \"" + name + "\".");
        }
    } else if (cmd == "favoritePlace") {
        long long id = m["id"].i64();
        bool value = m["value"].boolean();
        if (id <= 0) { backend::Log("[!] Set a Place ID before favouriting it."); return; }
        bool exists = false;
        {
            std::lock_guard<std::mutex> lock(backend::savedPlacesMutex);
            for (auto& p : backend::savedPlaces) if (p.id == id) { exists = true; break; }
        }
        if (!exists) {
            if (!value) return;
            std::string name = m["name"].str();
            backend::AddSavedPlace(id, name.empty() ? "Place " + std::to_string(id) : name);
        }
        backend::SetSavedPlaceFavorite(id, value);
    } else if (cmd == "removePlace") {
        backend::RemoveSavedPlace(m["id"].i64());
    } else if (cmd == "usePrivateServer") {
        std::string link = m["link"].str();
        if (link.empty()) { backend::Log("[!] Paste a private server link first."); return; }
        std::string cookie = FirstAccountCookie();
        std::thread([link, cookie]() {
            backend::PrivateServer ps;
            if (!backend::ResolvePrivateServerLink(link, cookie, ps)) return;
            backend::SetActivePrivateServer(ps);
            backend::SavePlaceId(ps.placeId);
            backend::FetchPlaceInfo(ps.placeId, cookie);
        }).detach();
    } else if (cmd == "selectPrivateServer") {
        std::string code = m["code"].str();
        backend::PrivateServer pick;
        {
            std::lock_guard<std::mutex> lock(backend::privateServersMutex);
            for (auto& p : backend::savedPrivateServers) if (p.linkCode == code) { pick = p; break; }
        }
        if (pick.linkCode.empty()) return;
        std::string cookie = FirstAccountCookie();
        std::thread([pick, cookie]() {
            backend::SetActivePrivateServer(pick);
            backend::SavePlaceId(pick.placeId);
            backend::FetchPlaceInfo(pick.placeId, cookie);
        }).detach();
    } else if (cmd == "savePrivateServer") {
        backend::PrivateServer ps;
        { std::lock_guard<std::mutex> lock(backend::activePrivateServerMutex); ps = backend::activePrivateServer; }
        if (ps.linkCode.empty()) { backend::Log("[!] Use a private server link before saving it."); return; }
        std::string name = m["name"].str();
        ps.name = name.empty() ? "Private " + std::to_string(ps.placeId) : name;
        backend::AddSavedPrivateServer(ps);
    } else if (cmd == "removePrivateServer") {
        backend::RemoveSavedPrivateServer(m["code"].str());
    } else if (cmd == "clearPrivateServer") {
        backend::ClearActivePrivateServer();
    } else if (cmd == "weaoRefresh") {
        std::thread([]() { backend::FetchWeaoVersions(); }).detach();
    } else if (cmd == "downloadLatest") {
        std::thread([]() { backend::ForceLiveBuild(); }).detach();
    } else if (cmd == "downloadPrevious") {
        std::thread([]() { backend::DownloadPreviousBuild(); }).detach();
    } else if (cmd == "downgrade") {
        std::string hash = m["hash"].str();
        if (hash.empty()) { backend::Log("[!] Enter a version hash to downgrade to."); return; }
        std::thread([hash]() { backend::DownloadRobloxBuild(hash); }).detach();
    } else if (cmd == "setBuild") {
        backend::SetActiveBuild(m["version"].str());
    } else if (cmd == "removeBuild") {
        std::string version = m["version"].str();
        if (!version.empty()) std::thread([version]() { backend::DeleteBuild(version); }).detach();
    } else if (cmd == "openUrl") {
        std::string url = m["url"].str();
        if (url.rfind("https://www.roblox.com/", 0) == 0)
            ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == "frame") {
        // Recolour the Windows caption bar (min/max/close) to match the theme.
        auto parseHex = [](const std::string& h) -> COLORREF {
            if (h.size() != 7 || h[0] != '#') return CLR_INVALID;
            auto hx = [&](int i) { return (int)strtol(h.substr(i, 2).c_str(), nullptr, 16); };
            return RGB(hx(1), hx(3), hx(5));
        };
        COLORREF cap = parseHex(m["bg"].str()), txt = parseHex(m["text"].str()), bd = parseHex(m["border"].str());
        if (cap != CLR_INVALID) {
            DwmSetWindowAttribute(g_hwnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof(cap));
            g_clientBg = cap;
            double l = 0.299 * GetRValue(cap) + 0.587 * GetGValue(cap) + 0.114 * GetBValue(cap);
            BOOL dark = l < 140 ? TRUE : FALSE;
            DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
        if (bd != CLR_INVALID) DwmSetWindowAttribute(g_hwnd, 34 /* DWMWA_BORDER_COLOR */, &bd, sizeof(bd));
        if (txt != CLR_INVALID) DwmSetWindowAttribute(g_hwnd, 36 /* DWMWA_TEXT_COLOR */, &txt, sizeof(txt));
    }

    Tick();
}

static HRESULT ServeResource(ICoreWebView2WebResourceRequestedEventArgs* args) {
    ICoreWebView2WebResourceRequest* req = nullptr;
    if (FAILED(args->get_Request(&req)) || !req) return S_OK;
    LPWSTR rawUri = nullptr;
    req->get_Uri(&rawUri);
    std::wstring uri = rawUri ? rawUri : L"";
    if (rawUri) CoTaskMemFree(rawUri);
    req->Release();

    const std::wstring origin(kAppOrigin);
    if (uri.compare(0, origin.size(), origin) != 0) return S_OK;
    std::wstring path = uri.substr(origin.size());
    size_t q = path.find_first_of(L"?#");
    if (q != std::wstring::npos) path.resize(q);

    const BYTE* data = nullptr;
    DWORD size = 0;
    std::vector<unsigned char> owned;
    std::wstring mime = L"application/octet-stream";
    bool cacheable = true;

    if (path.empty() || path == L"index.html") {
        LoadResourceBytes(IDR_UI_HTML, data, size);
        mime = L"text/html; charset=utf-8";
        cacheable = false;
    } else if (path == L"brand.png") {
        LoadResourceBytes(IDR_BRAND_PNG, data, size);
        mime = L"image/png";
    } else if (path == L"inter.ttf") {
        LoadResourceBytes(IDR_FONT_INTER, data, size);
        mime = L"font/ttf";
    } else if (path.rfind(L"avatar/", 0) == 0) {
        long long id = _wtoi64(path.c_str() + 7);
        std::lock_guard<std::mutex> lock(backend::accountsMutex);
        int idx = FindUserIndexLocked(id);
        if (idx >= 0) owned = backend::accounts[idx].avatarPng;
        mime = L"image/png";
    } else if (path.rfind(L"place/", 0) == 0) {
        long long id = _wtoi64(path.c_str() + 6);
        std::lock_guard<std::mutex> lock(backend::placeInfoMutex);
        if (backend::placeInfo.placeId == id) owned = backend::placeInfo.iconPng;
        mime = L"image/png";
    }
    if (!owned.empty()) { data = owned.data(); size = (DWORD)owned.size(); }

    IStream* stream = (data && size) ? SHCreateMemStream(data, size) : nullptr;
    std::wstring headers = L"Content-Type: " + mime +
        (stream && cacheable ? L"\r\nCache-Control: max-age=86400" : L"\r\nCache-Control: no-store");
    ICoreWebView2WebResourceResponse* resp = nullptr;
    if (SUCCEEDED(g_env->CreateWebResourceResponse(stream, stream ? 200 : 404, stream ? L"OK" : L"Not Found",
            headers.c_str(), &resp)) && resp) {
        args->put_Response(resp);
        resp->Release();
    }
    if (stream) stream->Release();
    return S_OK;
}

static void ResizeWebView() {
    if (!g_controller || !g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

static void ShowWebViewFailure(HRESULT hr) {
    wchar_t msg[512];
    swprintf(msg, 512,
        L"Vels Multi Tool couldn't start its interface (WebView2 error 0x%08lX).\n\n"
        L"It needs the Microsoft Edge WebView2 Runtime, which ships with Windows 11. "
        L"Open the download page now?", (unsigned long)hr);
    if (MessageBoxW(g_hwnd, msg, kWindowTitle, MB_YESNO | MB_ICONERROR) == IDYES)
        ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr, nullptr, SW_SHOWNORMAL);
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// Keep the WebView hidden until the page has painted its loader, so the first
// thing on screen is the loading screen, not a bare/flashing browser surface.
static void LoadSplashBitmap() {
    const BYTE* data = nullptr;
    DWORD size = 0;
    if (!LoadResourceBytes(IDR_BRAND_PNG, data, size)) return;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    if (SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(data), size)) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        if (w && h) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = (LONG)w;
            bi.bmiHeader.biHeight = -(LONG)h;  // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bmp && bits && SUCCEEDED(converter->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)bits))) {
                g_splashBmp = bmp;
                g_splashW = (int)w;
                g_splashH = (int)h;
            } else if (bmp) {
                DeleteObject(bmp);
            }
        }
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
}

static void PaintSplash(HDC hdc, const RECT& rc) {
    HBRUSH bgBrush = CreateSolidBrush(g_clientBg);
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);
    if (g_webviewShown || !g_splashBmp) return;

    UINT dpi = GetDpiForWindow(g_hwnd);
    if (!dpi) dpi = 96;
    int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    int tile = MulDiv(86, dpi, 96), r = MulDiv(23, dpi, 96);

    HBRUSH tileBrush = CreateSolidBrush(RGB(23, 25, 29));
    HPEN tilePen = CreatePen(PS_SOLID, 1, RGB(44, 47, 55));
    HGDIOBJ ob = SelectObject(hdc, tileBrush), op = SelectObject(hdc, tilePen);
    RoundRect(hdc, cx - tile / 2, cy - tile / 2, cx + tile / 2, cy + tile / 2, r, r);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(tileBrush);
    DeleteObject(tilePen);

    int iw = MulDiv(52, dpi, 96), ih = iw * g_splashH / (g_splashW ? g_splashW : 1);
    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ oldBmp = SelectObject(mem, g_splashBmp);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(hdc, cx - iw / 2, cy - ih / 2, iw, ih, mem, 0, 0, g_splashW, g_splashH, bf);
    SelectObject(mem, oldBmp);
    DeleteDC(mem);
}

static void RevealWebView() {
    if (g_webviewShown) return;
    g_webviewShown = true;
    KillTimer(g_hwnd, kRevealTimer);
    if (g_controller) g_controller->put_IsVisible(TRUE);
}

static void OnControllerCreated(ICoreWebView2Controller* controller) {
    g_controller = controller;
    g_controller->AddRef();
    g_controller->put_IsVisible(FALSE);
    g_controller->get_CoreWebView2(&g_webview);

    ICoreWebView2Controller2* controller2 = nullptr;
    if (SUCCEEDED(g_controller->QueryInterface(IID_Controller2, reinterpret_cast<void**>(&controller2))) && controller2) {
        COREWEBVIEW2_COLOR bg = { 255, GetRValue(kBgColor), GetGValue(kBgColor), GetBValue(kBgColor) };
        controller2->put_DefaultBackgroundColor(bg);
        controller2->Release();
    }

    ICoreWebView2Settings* settings = nullptr;
    if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
        wchar_t devtools[8];
        bool wantDevTools = GetEnvironmentVariableW(L"VELS_DEVTOOLS", devtools, 8) > 0;
        settings->put_AreDevToolsEnabled(wantDevTools);
        settings->put_AreDefaultContextMenusEnabled(wantDevTools);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreHostObjectsAllowed(FALSE);
        settings->Release();
    }
    ResizeWebView();

    EventRegistrationToken token{};
    std::wstring filter = std::wstring(kAppOrigin) + L"*";
    g_webview->AddWebResourceRequestedFilter(filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

    auto* resourceHandler = new ComHandler<ICoreWebView2WebResourceRequestedEventHandler,
        ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>(IID_WebResourceHandler,
        [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) { return ServeResource(args); });
    g_webview->add_WebResourceRequested(resourceHandler, &token);
    resourceHandler->Release();

    auto* messageHandler = new ComHandler<ICoreWebView2WebMessageReceivedEventHandler,
        ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(IID_WebMessageHandler,
        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR raw = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                std::string text = Narrow(raw);
                CoTaskMemFree(raw);
                HandlePageMessage(text);
            }
            return S_OK;
        });
    g_webview->add_WebMessageReceived(messageHandler, &token);
    messageHandler->Release();

    auto* newWindowHandler = new ComHandler<ICoreWebView2NewWindowRequestedEventHandler,
        ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(IID_NewWindowHandler,
        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            return S_OK;
        });
    g_webview->add_NewWindowRequested(newWindowHandler, &token);
    newWindowHandler->Release();

    g_webview->Navigate((std::wstring(kAppOrigin) + L"index.html").c_str());
    // Failsafe: reveal even if the page never sends 'ready'.
    SetTimer(g_hwnd, kRevealTimer, 4000, nullptr);
}

using CreateEnvironmentFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

// WebView2Loader.dll is embedded in the exe so the tool still ships as one
// file. A copy next to the exe wins; otherwise it is unpacked to LocalAppData.
static CreateEnvironmentFn LoadWebView2Loader() {
    std::vector<std::wstring> candidates = { g_exeDir + L"\\WebView2Loader.dll" };

    const BYTE* data = nullptr;
    DWORD size = 0;
    wchar_t local[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (len > 0 && len < MAX_PATH && LoadResourceBytes(IDR_WV2LOADER, data, size)) {
        std::wstring dir = std::wstring(local, len) + L"\\VelsMultiTool";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring dll = dir + L"\\WebView2Loader.dll";
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        bool current = GetFileAttributesExW(dll.c_str(), GetFileExInfoStandard, &fad) &&
            fad.nFileSizeHigh == 0 && fad.nFileSizeLow == size;
        if (!current) {
            HANDLE h = CreateFileW(dll.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(h, data, size, &written, nullptr);
                CloseHandle(h);
            }
        }
        candidates.push_back(dll);
    }

    for (const auto& path : candidates) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        HMODULE mod = LoadLibraryW(path.c_str());
        if (!mod) continue;
        FARPROC proc = GetProcAddress(mod, "CreateCoreWebView2EnvironmentWithOptions");
        if (proc) return reinterpret_cast<CreateEnvironmentFn>(reinterpret_cast<void*>(proc));
    }
    return nullptr;
}

static void InitWebView() {
    CreateEnvironmentFn createEnvironment = LoadWebView2Loader();
    if (!createEnvironment) { ShowWebViewFailure(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)); return; }

    // Stay on WebView2's default Direct3D 11 GPU path, tuned for smoothness: GPU raster,
    // zero-copy uploads, and no throttling or occlusion pauses (the admin hand-off window
    // has to paint while it's still covered by the old one). The variable is restored
    // once the browser process has started so Roblox's own WebViews don't inherit it.
    static std::wstring previousArgs;
    static bool hadPreviousArgs = false;
    {
        wchar_t existing[2048];
        DWORD n = GetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", existing, 2048);
        hadPreviousArgs = n > 0 && n < 2048;
        if (hadPreviousArgs) previousArgs.assign(existing, n);
        // Note: GPU rasterization is intentionally NOT forced - it makes Chromium
        // render text with grayscale AA instead of crisp ClearType. Keep smooth
        // scrolling and no throttling/occlusion pauses (the last matters for the
        // admin hand-off, which paints while still covered by the old window).
        std::wstring flags = L"--enable-smooth-scrolling "
                             L"--disable-background-timer-throttling --disable-renderer-backgrounding "
                             L"--disable-features=CalculateNativeWinOcclusion";
        if (hadPreviousArgs) flags = previousArgs + L" " + flags;
        SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", flags.c_str());
    }
    auto restoreArgs = []() {
        SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", hadPreviousArgs ? previousArgs.c_str() : nullptr);
    };

    // Admin and non-admin copies can't share one browser profile, and briefly run side by side during the hand-off.
    std::wstring dataDir = g_exeDir + (backend::IsElevated() ? L"\\webview2_data_admin" : L"\\webview2_data");
    auto* envHandler = new ComHandler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
        HRESULT, ICoreWebView2Environment*>(IID_EnvCompletedHandler,
        [restoreArgs](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            restoreArgs();
            if (FAILED(hr) || !env) { ShowWebViewFailure(hr); return S_OK; }
            g_env = env;
            g_env->AddRef();
            auto* ctrlHandler = new ComHandler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                HRESULT, ICoreWebView2Controller*>(IID_CtrlCompletedHandler,
                [](HRESULT hr2, ICoreWebView2Controller* controller) -> HRESULT {
                    if (FAILED(hr2) || !controller) { ShowWebViewFailure(hr2); return S_OK; }
                    OnControllerCreated(controller);
                    return S_OK;
                });
            HRESULT created = g_env->CreateCoreWebView2Controller(g_hwnd, ctrlHandler);
            ctrlHandler->Release();
            if (FAILED(created)) ShowWebViewFailure(created);
            return S_OK;
        });
    HRESULT hr = createEnvironment(nullptr, dataDir.c_str(), nullptr, envHandler);
    envHandler->Release();
    if (FAILED(hr)) ShowWebViewFailure(hr);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        ResizeWebView();
        if (g_controller && g_webviewShown) g_controller->put_IsVisible(wParam != SIZE_MINIMIZED);
        return 0;
    case WM_MOVE:
    case WM_MOVING:
        if (g_controller) g_controller->NotifyParentWindowPositionChanged();
        break;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        UINT dpi = GetDpiForWindow(hwnd);
        if (!dpi) dpi = 96;
        mmi->ptMinTrackSize.x = MulDiv(980, dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(640, dpi, 96);
        return 0;
    }
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ACTIVATE: {
        bool active = LOWORD(wParam) != WA_INACTIVE;
        backend::uiForeground.store(active);
        if (!g_retiring) SetTimer(hwnd, kStateTimer, active ? 500 : 2000, nullptr);
        break;
    }
    case WM_TIMER:
        if (wParam == kStateTimer && !IsIconic(hwnd)) Tick();
        else if (wParam == kHandoffTimer) {
            // The admin copy has painted under the old window: take focus and retire the old one.
            KillTimer(hwnd, kHandoffTimer);
            g_handoffDone = true;
            SetForegroundWindow(hwnd);
            if (IsWindow(g_handoffFrom)) PostMessageW(g_handoffFrom, WM_CLOSE, 0, 0);
        } else if (wParam == kRetireFailsafeTimer) {
            DestroyWindow(hwnd);
        } else if (wParam == kRevealTimer) {
            RevealWebView();
        }
        return 0;
    case kMsgElevateResult:
        if (wParam) {
            // Free the cookie lock and singleton mutex so the admin copy can take them over.
            g_retiring = true;
            KillTimer(hwnd, kStateTimer);
            backend::Shutdown();
            if (g_webview) g_webview->PostWebMessageAsJson(L"{\"type\":\"relaunch\",\"stage\":\"handoff\"}");
            SetTimer(hwnd, kRetireFailsafeTimer, 20000, nullptr);
        } else {
            g_elevating.store(false);
            PostToPage("{\"type\":\"relaunch\",\"stage\":\"cancelled\"}");
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;  // handled in WM_PAINT to avoid flicker
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        PaintSplash(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kStateTimer);
        g_pageReady = false;
        if (g_controller) { g_controller->Close(); g_controller->Release(); g_controller = nullptr; }
        if (g_webview) { g_webview->Release(); g_webview = nullptr; }
        if (g_env) { g_env->Release(); g_env = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wchar_t pathBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, pathBuf, MAX_PATH);
    std::wstring full(pathBuf);
    g_exeDir = full.substr(0, full.find_last_of(L"\\/"));
    std::wstring cmdLine = lpCmdLine ? lpCmdLine : L"";
    size_t handoffPos = cmdLine.find(L"--handoff=");
    if (handoffPos != std::wstring::npos)
        g_handoffFrom = reinterpret_cast<HWND>((INT_PTR)_wtoi64(cmdLine.c_str() + handoffPos + 10));
    bool enableMulti = cmdLine.find(L"--multi") != std::wstring::npos;

    // Give the previous (non-admin) copy a moment to release its locks.
    if (g_handoffFrom) Sleep(500);
    backend::Init(g_exeDir);
    if (enableMulti && backend::IsElevated()) backend::StartWatching();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadSplashBitmap();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"VelsMultiToolClass";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hbrBackground = CreateSolidBrush(kBgColor);
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int width = MulDiv(1280, dpi, 96), height = MulDiv(820, dpi, 96);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    width = std::min(width, (int)(work.right - work.left) - 40);
    height = std::min(height, (int)(work.bottom - work.top) - 40);
    int x = work.left + ((work.right - work.left) - width) / 2;
    int y = work.top + ((work.bottom - work.top) - height) / 2;

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
        x, y, width, height, nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 1;

    // Dark native frame that blends into the page (caption colours are Windows 11 only).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF caption = kBgColor, border = RGB(35, 38, 44), captionText = RGB(200, 204, 211);
    DwmSetWindowAttribute(g_hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(g_hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    DwmSetWindowAttribute(g_hwnd, 36 /* DWMWA_TEXT_COLOR */, &captionText, sizeof(captionText));

    WINDOWPLACEMENT oldPlacement = {};
    oldPlacement.length = sizeof(oldPlacement);
    if (g_handoffFrom && IsWindow(g_handoffFrom) && GetWindowPlacement(g_handoffFrom, &oldPlacement)) {
        // Open exactly where the old window is, tucked underneath it until we're ready.
        WINDOWPLACEMENT wp = oldPlacement;
        wp.showCmd = oldPlacement.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE;
        SetWindowPlacement(g_hwnd, &wp);
        SetWindowPos(g_hwnd, g_handoffFrom, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        g_handoffFrom = nullptr;
        ShowWindow(g_hwnd, nCmdShow);
    }
    UpdateWindow(g_hwnd);

    InitWebView();
    SetTimer(g_hwnd, kStateTimer, 500, nullptr);
    SetTimer(g_hwnd, kRevealTimer, 5000, nullptr);  // failsafe if the page never loads

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    backend::Shutdown();
    CoUninitialize();
    return 0;
}
