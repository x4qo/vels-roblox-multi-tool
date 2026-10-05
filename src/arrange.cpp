// Auto-arrange: tiles every Roblox window neatly across a monitor using one of a
// few presets, and (optionally) re-tiles whenever a Roblox window opens or closes.

#include "backend.h"

#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <thread>

namespace backend {

extern std::wstring g_exeDir;

std::mutex arrangeMutex;
ArrangeSettings arrangeSettings;

static const char* kPresets[] = { "grid", "columns", "rows", "focus", "cascade", "mini" };

static bool ValidPreset(const std::string& p) {
    for (const char* k : kPresets) if (p == k) return true;
    return false;
}

static std::wstring ArrangeFilePath() { return g_exeDir + L"\\arrange.dat"; }

void LoadArrangeSettings() {
    std::ifstream f(ArrangeFilePath().c_str());
    ArrangeSettings s;
    std::string preset;
    int gap = 6, autoOn = 0, monitor = 0;
    if (f >> preset >> gap >> autoOn >> monitor) {
        if (ValidPreset(preset)) s.preset = preset;
        s.gap = std::clamp(gap, 0, 40);
        s.autoArrange = autoOn != 0;
        s.monitor = std::max(0, monitor);
    }
    std::lock_guard<std::mutex> lk(arrangeMutex);
    arrangeSettings = s;
}

void SetArrangeSettings(const ArrangeSettings& in) {
    ArrangeSettings s = in;
    if (!ValidPreset(s.preset)) s.preset = "grid";
    s.gap = std::clamp(s.gap, 0, 40);
    s.monitor = std::max(0, s.monitor);
    {
        std::lock_guard<std::mutex> lk(arrangeMutex);
        arrangeSettings = s;
    }
    std::ofstream f(ArrangeFilePath().c_str(), std::ios::trunc);
    if (f) f << s.preset << ' ' << s.gap << ' ' << (s.autoArrange ? 1 : 0) << ' ' << s.monitor;
}

/* ---------- monitors ---------- */

struct Monitor { RECT work; bool primary; };

static std::vector<Monitor> ListMonitors() {
    std::vector<Monitor> out;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hm, &mi))
            reinterpret_cast<std::vector<Monitor>*>(lp)->push_back({ mi.rcWork, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    // Primary first, then left-to-right, so index 0 is always the main screen.
    std::stable_sort(out.begin(), out.end(), [](const Monitor& a, const Monitor& b) {
        if (a.primary != b.primary) return a.primary;
        return a.work.left < b.work.left;
    });
    return out;
}

std::vector<std::string> MonitorLabels() {
    std::vector<std::string> labels;
    auto mons = ListMonitors();
    for (size_t i = 0; i < mons.size(); ++i) {
        const RECT& r = mons[i].work;
        labels.push_back("Display " + std::to_string(i + 1) + " · " + std::to_string(r.right - r.left) + "×" +
                         std::to_string(r.bottom - r.top) + (mons[i].primary ? " (main)" : ""));
    }
    return labels;
}

/* ---------- finding Roblox windows ---------- */

static std::set<DWORD> RobloxPids() {
    std::set<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"RobloxPlayerBeta.exe") == 0) pids.insert(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

struct RbxWindow { HWND hwnd; DWORD pid; };

// Visible, unowned top-level windows of RobloxPlayerBeta, ordered like the account
// list (accounts with a known pid first, in list order), then everything else by pid.
static std::vector<RbxWindow> FindRobloxWindows() {
    struct Ctx { std::set<DWORD> pids; std::vector<RbxWindow> wins; } ctx;
    ctx.pids = RobloxPids();
    if (ctx.pids.empty()) return {};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (!c->pids.count(pid)) return TRUE;
        RECT r;
        if (!GetWindowRect(h, &r) || (r.right - r.left) < 120 || (r.bottom - r.top) < 80) return TRUE;  // skip tiny helper windows
        for (auto& w : c->wins) if (w.pid == pid) return TRUE;  // one main window per process
        c->wins.push_back({ h, pid });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    std::map<DWORD, size_t> rank;
    {
        std::lock_guard<std::mutex> la(accountsMutex);
        std::lock_guard<std::mutex> lp(launchedMutex);
        for (size_t i = 0; i < accounts.size(); ++i) {
            auto it = launchedPids.find(accounts[i].userId);
            if (it != launchedPids.end()) rank[(DWORD)it->second] = i;
        }
    }
    std::stable_sort(ctx.wins.begin(), ctx.wins.end(), [&](const RbxWindow& a, const RbxWindow& b) {
        size_t ra = rank.count(a.pid) ? rank[a.pid] : SIZE_MAX, rb = rank.count(b.pid) ? rank[b.pid] : SIZE_MAX;
        if (ra != rb) return ra < rb;
        return a.pid < b.pid;
    });
    return ctx.wins;
}

int CountRobloxWindows() { return (int)FindRobloxWindows().size(); }

/* ---------- layouts ---------- */

static std::vector<RECT> Grid(const RECT& W, int n, int cols, int rows, int gap) {
    std::vector<RECT> out;
    int w = W.right - W.left, h = W.bottom - W.top;
    int tw = (w - gap * (cols + 1)) / cols, th = (h - gap * (rows + 1)) / rows;
    for (int i = 0; i < n; ++i) {
        int c = i % cols, r = i / cols;
        // Centre a short last row instead of leaving it ragged on the left.
        int inRow = (r == rows - 1) ? n - r * cols : cols;
        int offset = (cols - inRow) * (tw + gap) / 2;
        int x = W.left + gap + offset + c * (tw + gap), y = W.top + gap + r * (th + gap);
        out.push_back({ x, y, x + tw, y + th });
    }
    return out;
}

// Picks the column count that gives each window the most usable 16:9 area.
static std::vector<RECT> SmartGrid(const RECT& W, int n, int gap) {
    int w = W.right - W.left, h = W.bottom - W.top;
    int bestCols = 1;
    double best = -1;
    for (int cols = 1; cols <= n; ++cols) {
        int rows = (n + cols - 1) / cols;
        double tw = (w - gap * (cols + 1)) / (double)cols, th = (h - gap * (rows + 1)) / (double)rows;
        if (tw <= 0 || th <= 0) continue;
        double ew = std::min(tw, th * 16.0 / 9.0), eh = std::min(th, tw * 9.0 / 16.0);
        if (ew * eh > best) { best = ew * eh; bestCols = cols; }
    }
    return Grid(W, n, bestCols, (n + bestCols - 1) / bestCols, gap);
}

static std::vector<RECT> Layout(const std::string& preset, const RECT& W, int n, int gap) {
    int w = W.right - W.left, h = W.bottom - W.top;
    if (n <= 0) return {};
    if (preset == "columns") return Grid(W, n, n, 1, gap);
    if (preset == "rows") return Grid(W, n, 1, n, gap);
    if (preset == "focus") {
        if (n == 1) return Grid(W, 1, 1, 1, gap);
        // One large window on the left, the rest stacked in a column on the right.
        std::vector<RECT> out;
        int mainW = (w - gap * 3) * 2 / 3;
        out.push_back({ W.left + gap, W.top + gap, W.left + gap + mainW, W.bottom - gap });
        RECT side = { W.left + gap + mainW, W.top, W.right, W.bottom };
        auto rest = Grid(side, n - 1, 1, n - 1, gap);
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }
    if (preset == "cascade") {
        std::vector<RECT> out;
        int cw = w * 62 / 100, ch = h * 62 / 100, step = 34;
        int fit = std::max(1, std::min((w - cw) / step, (h - ch) / step) + 1);
        for (int i = 0; i < n; ++i) {
            int k = i % fit;
            int x = W.left + gap + k * step + (i / fit) * (step / 2), y = W.top + gap + k * step;
            out.push_back({ x, y, x + cw, y + ch });
        }
        return out;
    }
    if (preset == "mini") {
        // Small 16:9 tiles packed from the top-left; shrink only if they'd overflow.
        int tw = 520;
        for (; tw >= 200; tw -= 20) {
            int th = tw * 9 / 16;
            int cols = std::max(1, (w - gap) / (tw + gap)), rows = std::max(1, (h - gap) / (th + gap));
            if (cols * rows >= n) break;
        }
        int th = tw * 9 / 16, cols = std::max(1, (w - gap) / (tw + gap));
        std::vector<RECT> out;
        for (int i = 0; i < n; ++i) {
            int x = W.left + gap + (i % cols) * (tw + gap), y = W.top + gap + (i / cols) * (th + gap);
            out.push_back({ x, y, x + tw, y + th });
        }
        return out;
    }
    return SmartGrid(W, n, gap);
}

// Moves a window so its *visible* frame lands on target (Windows 10/11 windows have
// invisible resize borders that GetWindowRect includes).
static void PlaceWindow(HWND h, const RECT& target) {
    if (IsIconic(h) || IsZoomed(h)) ShowWindowAsync(h, SW_RESTORE);
    RECT outer, frame;
    int l = 0, t = 0, r = 0, b = 0;
    if (GetWindowRect(h, &outer) &&
        SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        l = frame.left - outer.left; t = frame.top - outer.top;
        r = outer.right - frame.right; b = outer.bottom - frame.bottom;
    }
    // Async: a frozen Roblox window must never hang the tool.
    SetWindowPos(h, nullptr, target.left - l, target.top - t,
                 (target.right - target.left) + l + r, (target.bottom - target.top) + t + b,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | SWP_NOOWNERZORDER);
}

int ArrangeRobloxWindows(const std::string& presetOverride) {
    ArrangeSettings s;
    { std::lock_guard<std::mutex> lk(arrangeMutex); s = arrangeSettings; }
    if (!presetOverride.empty() && ValidPreset(presetOverride)) s.preset = presetOverride;

    auto wins = FindRobloxWindows();
    if (wins.empty()) { Log("[i] No Roblox windows to arrange."); return 0; }
    auto mons = ListMonitors();
    if (mons.empty()) return 0;
    const RECT& W = mons[std::min<size_t>(s.monitor, mons.size() - 1)].work;

    auto rects = Layout(s.preset, W, (int)wins.size(), s.gap);
    for (size_t i = 0; i < wins.size() && i < rects.size(); ++i) PlaceWindow(wins[i].hwnd, rects[i]);
    // Cascade overlaps, so stack them in order with the first on top.
    if (s.preset == "cascade")
        for (size_t i = wins.size(); i-- > 0;)
            SetWindowPos(wins[i].hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    Log("[v] Arranged " + std::to_string(wins.size()) + " Roblox window" + (wins.size() == 1 ? "" : "s") + " (" + s.preset + ").");
    return (int)wins.size();
}

// Background watcher: when auto-arrange is on, re-tile shortly after the set of
// Roblox windows changes (Roblox resizes itself while starting, so wait for it).
void StartAutoArrangeWatcher() {
    std::thread([]() {
        std::set<HWND> last;
        auto changedAt = std::chrono::steady_clock::time_point{};
        bool pending = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            bool on;
            { std::lock_guard<std::mutex> lk(arrangeMutex); on = arrangeSettings.autoArrange; }
            if (!on) { last.clear(); pending = false; continue; }
            std::set<HWND> now;
            for (auto& w : FindRobloxWindows()) now.insert(w.hwnd);
            if (now != last) {
                bool grew = now.size() > last.size();
                last = now;
                if (!now.empty()) { pending = true; changedAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(grew ? 2500 : 600); }
            }
            if (pending && std::chrono::steady_clock::now() >= changedAt) {
                pending = false;
                ArrangeRobloxWindows("");
            }
        }
    }).detach();
}

}  // namespace backend
