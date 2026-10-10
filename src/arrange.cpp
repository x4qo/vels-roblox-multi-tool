
#include "backend.h"

#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
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

struct Monitor { RECT work; bool primary; };

static std::vector<Monitor> ListMonitors() {
    std::vector<Monitor> out;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hm, &mi))
            reinterpret_cast<std::vector<Monitor>*>(lp)->push_back({ mi.rcWork, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
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

static const wchar_t* TargetExe() {
    static std::wstring name = []() {
        const wchar_t* e = _wgetenv(L"VELS_ARRANGE_EXE");
        return std::wstring(e && *e ? e : L"RobloxPlayerBeta.exe");
    }();
    return name.c_str();
}

static std::set<DWORD> RobloxPids() {
    std::set<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, TargetExe()) == 0) pids.insert(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

struct RbxWindow { HWND hwnd; DWORD pid; };

static std::vector<RbxWindow> FindRobloxWindows(bool withMinimised = true) {
    struct Ctx { std::set<DWORD> pids; std::vector<RbxWindow> wins; bool withMin; } ctx;
    ctx.withMin = withMinimised;
    ctx.pids = RobloxPids();
    if (ctx.pids.empty()) return {};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (!c->pids.count(pid)) return TRUE;
        if (IsIconic(h)) {
            if (!c->withMin) return TRUE;
        } else {
            RECT r;
            if (!GetWindowRect(h, &r) || (r.right - r.left) < 120 || (r.bottom - r.top) < 80) return TRUE;
        }
        for (auto& w : c->wins) if (w.pid == pid) return TRUE;
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

static int g_minW = 0, g_minH = 0;

static int SlotStart(int lo, int hi, int size, int count, int index) {
    if (count <= 1) return lo + (hi - lo - size) / 2;
    return lo + (int)std::lround((double)(hi - lo - size) * index / (count - 1));
}

static std::vector<RECT> Grid(const RECT& W, int n, int cols, int gap, int minW, int minH) {
    std::vector<RECT> out;
    if (n <= 0) return out;
    cols = std::clamp(cols, 1, n);
    int rows = (n + cols - 1) / cols;
    int left = W.left + gap, right = W.right - gap, top = W.top + gap, bottom = W.bottom - gap;
    int availW = std::max(1, right - left), availH = std::max(1, bottom - top);
    int th = std::min(availH, std::max((availH - gap * (rows - 1)) / rows, minH));
    for (int r = 0; r < rows; ++r) {
        int inRow = n / rows + (r < n % rows ? 1 : 0);
        int tw = std::min(availW, std::max((availW - gap * (inRow - 1)) / inRow, minW));
        int y = SlotStart(top, bottom, th, rows, r);
        int y2 = (r == rows - 1 && rows > 1) ? bottom : y + th;
        for (int c = 0; c < inRow; ++c) {
            int x = SlotStart(left, right, tw, inRow, c);
            int x2 = (c == inRow - 1 && inRow > 1) ? right : x + tw;
            out.push_back({ x, y, x2, y2 });
        }
    }
    return out;
}

static std::vector<RECT> SmartGrid(const RECT& W, int n, int gap, int minW, int minH) {
    int w = W.right - W.left, h = W.bottom - W.top;
    int bestCols = 1;
    double best = -1e18;
    for (int cols = 1; cols <= n; ++cols) {
        int rows = (n + cols - 1) / cols;
        double tw = (w - gap * (cols + 1)) / (double)cols, th = (h - gap * (rows + 1)) / (double)rows;
        if (tw <= 0 || th <= 0) continue;
        double ew = std::min(tw, th * 16.0 / 9.0), eh = std::min(th, tw * 9.0 / 16.0);
        double score = ew * eh;
        double shortfall = std::max(0.0, minW - tw) / std::max(1, minW) + std::max(0.0, minH - th) / std::max(1, minH);
        if (shortfall > 0) score = -shortfall * 1e9 + score;
        if (score > best) { best = score; bestCols = cols; }
    }
    return Grid(W, n, bestCols, gap, minW, minH);
}

static std::vector<RECT> Layout(const std::string& preset, const RECT& W, int n, int gap, int minW, int minH) {
    int w = W.right - W.left, h = W.bottom - W.top;
    if (n <= 0) return {};
    if (preset == "columns") return Grid(W, n, n, gap, minW, minH);
    if (preset == "rows") return Grid(W, n, 1, gap, minW, minH);
    if (preset == "focus") {
        if (n == 1) return Grid(W, 1, 1, gap, minW, minH);
        std::vector<RECT> out;
        int sideW = std::max((w - gap * 3) / 3, minW);
        int mainW = std::max(w - gap * 3 - sideW, std::min(minW, w - gap * 2));
        out.push_back({ W.left + gap, W.top + gap, W.left + gap + mainW, W.bottom - gap });
        RECT side = { W.right - gap - sideW - gap, W.top, W.right, W.bottom };
        auto rest = Grid(side, n - 1, 1, gap, minW, minH);
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    }
    if (preset == "cascade") {
        std::vector<RECT> out;
        int cw = std::max(w * 62 / 100, std::min(minW, w - gap * 2)), ch = std::max(h * 62 / 100, std::min(minH, h - gap * 2)), step = 34;
        int fit = std::max(1, std::min((w - gap * 2 - cw) / step, (h - gap * 2 - ch) / step) + 1);
        for (int i = 0; i < n; ++i) {
            int k = i % fit;
            int x = W.left + gap + k * step + (i / fit) * (step / 2), y = W.top + gap + k * step;
            x = std::min(x, (int)W.right - gap - cw);
            out.push_back({ x, y, x + cw, y + ch });
        }
        return out;
    }
    if (preset == "mini") {
        int floorW = std::max(200, minW);
        int tw = std::max(520, floorW);
        for (; tw > floorW; tw -= 20) {
            int th = std::max(tw * 9 / 16, minH);
            int cols = std::max(1, (w - gap) / (tw + gap)), rows = std::max(1, (h - gap) / (th + gap));
            if (cols * rows >= n) break;
        }
        tw = std::max(tw, floorW);
        int th = std::max(tw * 9 / 16, minH), cols = std::max(1, (w - gap) / (tw + gap));
        int rows = (n + cols - 1) / cols;
        bool overflow = gap + rows * (th + gap) > h;
        std::vector<RECT> out;
        for (int i = 0; i < n; ++i) {
            int x = W.left + gap + (i % cols) * (tw + gap);
            int y = overflow ? SlotStart(W.top + gap, W.bottom - gap, std::min(th, h - gap * 2), rows, i / cols)
                             : W.top + gap + (i / cols) * (th + gap);
            out.push_back({ x, y, x + tw, y + th });
        }
        return out;
    }
    return SmartGrid(W, n, gap, minW, minH);
}

static bool VisibleFrame(HWND h, RECT& frame) {
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) || GetWindowRect(h, &frame);
}

static void PlaceWindow(HWND h, const RECT& target) {
    RECT outer, frame;
    int l = 0, t = 0, r = 0, b = 0;
    if (GetWindowRect(h, &outer) &&
        SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        l = frame.left - outer.left; t = frame.top - outer.top;
        r = outer.right - frame.right; b = outer.bottom - frame.bottom;
    }
    SetWindowPos(h, nullptr, target.left - l, target.top - t,
                 (target.right - target.left) + l + r, (target.bottom - target.top) + t + b,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | SWP_NOOWNERZORDER);
}

static bool Matches(HWND h, const RECT& target, int tolerance) {
    RECT f;
    if (!VisibleFrame(h, f)) return true;
    return std::abs(f.left - target.left) <= tolerance && std::abs(f.top - target.top) <= tolerance &&
           std::abs(f.right - target.right) <= tolerance && std::abs(f.bottom - target.bottom) <= tolerance;
}

static std::mutex g_arrangeRunMutex;

static int ArrangeImpl(const std::string& presetOverride, bool quiet, bool withMinimised) {
    std::lock_guard<std::mutex> run(g_arrangeRunMutex);
    ArrangeSettings s;
    { std::lock_guard<std::mutex> lk(arrangeMutex); s = arrangeSettings; }
    if (!presetOverride.empty() && ValidPreset(presetOverride)) s.preset = presetOverride;

    auto wins = FindRobloxWindows(withMinimised);
    if (wins.empty()) { if (!quiet) Log("[i] No Roblox windows to arrange."); return 0; }
    auto mons = ListMonitors();
    if (mons.empty()) return 0;
    const RECT W = mons[std::min<size_t>(s.monitor, mons.size() - 1)].work;
    int n = (int)wins.size();

    bool restoring = false;
    for (auto& w : wins)
        if (IsIconic(w.hwnd) || IsZoomed(w.hwnd)) { ShowWindowAsync(w.hwnd, SW_SHOWNOACTIVATE); restoring = true; }
    for (int i = 0; restoring && i < 20; ++i) {
        Sleep(50);
        restoring = false;
        for (auto& w : wins) if (IsIconic(w.hwnd) || IsZoomed(w.hwnd)) restoring = true;
    }

    auto rects = Layout(s.preset, W, n, s.gap, g_minW, g_minH);
    bool exact = false;
    for (int pass = 0; pass < 5 && !exact; ++pass) {
        for (int i = 0; i < n && i < (int)rects.size(); ++i)
            if (!Matches(wins[i].hwnd, rects[i], 0)) PlaceWindow(wins[i].hwnd, rects[i]);
        for (int waited = 0; waited < 8; ++waited) {
            Sleep(40);
            exact = true;
            for (int i = 0; i < n && i < (int)rects.size(); ++i) if (!Matches(wins[i].hwnd, rects[i], 1)) exact = false;
            if (exact) break;
        }
        if (exact) break;

        int learnW = 0, learnH = 0;
        for (int i = 0; i < n && i < (int)rects.size(); ++i) {
            RECT f;
            if (!VisibleFrame(wins[i].hwnd, f)) continue;
            int fw = f.right - f.left, fh = f.bottom - f.top;
            int tw = rects[i].right - rects[i].left, th = rects[i].bottom - rects[i].top;
            if (fw > tw + 1 && (learnW == 0 || fw < learnW)) learnW = fw;
            if (fh > th + 1 && (learnH == 0 || fh < learnH)) learnH = fh;
        }
        if (learnW > g_minW || learnH > g_minH) {
            g_minW = std::max(g_minW, learnW);
            g_minH = std::max(g_minH, learnH);
            rects = Layout(s.preset, W, n, s.gap, g_minW, g_minH);
        }
    }

    bool overlaps = s.preset == "cascade";
    for (int i = 0; i + 1 < (int)rects.size() && !overlaps; ++i)
        for (int j = i + 1; j < (int)rects.size() && !overlaps; ++j) {
            RECT x;
            if (IntersectRect(&x, &rects[i], &rects[j])) overlaps = true;
        }
    if (overlaps)
        for (size_t i = wins.size(); i-- > 0;)
            SetWindowPos(wins[i].hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    if (!quiet) Log("[v] Arranged " + std::to_string(wins.size()) + " Roblox window" + (wins.size() == 1 ? "" : "s") + " (" + s.preset + ").");
    return (int)wins.size();
}

int ArrangeRobloxWindows(const std::string& presetOverride) { return ArrangeImpl(presetOverride, false, true); }

void StartAutoArrangeWatcher() {
    std::thread([]() {
        std::set<HWND> last;
        auto changedAt = std::chrono::steady_clock::time_point{};
        bool pending = false;
        bool announced = false;
        int followUps = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            bool on;
            { std::lock_guard<std::mutex> lk(arrangeMutex); on = arrangeSettings.autoArrange; }
            if (!on) { last.clear(); pending = false; followUps = 0; continue; }
            std::set<HWND> now;
            for (auto& w : FindRobloxWindows(false)) now.insert(w.hwnd);
            if (now != last) {
                bool grew = now.size() > last.size();
                last = now;
                if (!now.empty()) { pending = true; announced = false; followUps = grew ? 2 : 0; changedAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(grew ? 2500 : 600); }
            }
            if (pending && std::chrono::steady_clock::now() >= changedAt) {
                ArrangeImpl("", announced, false);
                announced = true;
                if (followUps > 0) { --followUps; changedAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000); }
                else pending = false;
            }
        }
    }).detach();
}

}
