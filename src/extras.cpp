
#include "backend.h"
#include "json.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace backend {

extern std::wstring g_exeDir;
std::vector<fs::path> StrapDirs();
std::vector<fs::path> RobloxInstalls();

static std::string Utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static bool ReadFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool WriteFile(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << data;
    return (bool)f;
}

static bool SameFile(const fs::path& a, const fs::path& b) {
    std::error_code e1, e2;
    auto sa = fs::file_size(a, e1), sb = fs::file_size(b, e2);
    if (e1 || e2 || sa != sb) return false;
    auto ta = fs::last_write_time(a, e1), tb = fs::last_write_time(b, e2);
    return !e1 && !e2 && ta == tb;
}

std::mutex fpsMutex;
std::map<long long, int> fpsCaps;
static int g_fpsBase = 0;
static int g_fpsLast = 0;
static std::mutex g_fpsApplyMutex;
static std::chrono::steady_clock::time_point g_fpsChangedAt{};

static fs::path FpsFilePath() { return fs::path(g_exeDir) / L"fps.dat"; }

static void SaveFpsCapsLocked() {
    std::ofstream f(FpsFilePath(), std::ios::trunc);
    if (!f) return;
    f << g_fpsBase << ' ' << g_fpsLast << '\n';
    for (auto& kv : fpsCaps) f << kv.first << ' ' << kv.second << '\n';
}

void LoadFpsCaps() {
    std::lock_guard<std::mutex> lk(fpsMutex);
    std::ifstream f(FpsFilePath());
    if (!(f >> g_fpsBase >> g_fpsLast)) { g_fpsBase = g_fpsLast = 0; return; }
    long long id;
    int cap;
    while (f >> id >> cap) if (cap > 0) fpsCaps[id] = cap;
}

void SetFpsCap(long long userId, int cap) {
    if (userId <= 0) return;
    std::lock_guard<std::mutex> lk(fpsMutex);
    if (cap <= 0) fpsCaps.erase(userId);
    else fpsCaps[userId] = std::clamp(cap, 5, 1000);
    SaveFpsCapsLocked();
}

static fs::path RobloxSettingsFile() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (!local) return {};
    fs::path best;
    fs::file_time_type bestTime{};
    std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::path(local) / L"Roblox", ec)) {
        std::wstring name = e.path().filename().wstring();
        if (name.rfind(L"GlobalBasicSettings_", 0) != 0 || name.find(L"Studio") != std::wstring::npos ||
            e.path().extension() != L".xml") continue;
        auto t = fs::last_write_time(e.path(), ec);
        if (best.empty() || t > bestTime) { best = e.path(); bestTime = t; }
    }
    return best;
}

void ApplyFpsCapFor(long long userId) {
    std::lock_guard<std::mutex> apply(g_fpsApplyMutex);
    int cap = 0, base, last;
    {
        std::lock_guard<std::mutex> lk(fpsMutex);
        if (fpsCaps.empty() && g_fpsLast == 0) return;
        auto it = fpsCaps.find(userId);
        if (it != fpsCaps.end()) cap = it->second;
        base = g_fpsBase;
        last = g_fpsLast;
    }
    fs::path file = RobloxSettingsFile();
    std::string xml;
    static const std::regex capRe("(<int name=\"FramerateCap\">)(-?\\d+)(</int>)");
    std::smatch m;
    if (file.empty() || !ReadFile(file, xml) || !std::regex_search(xml, m, capRe)) {
        static bool warned = false;
        if (cap > 0 && !warned) { warned = true; Log("[!] Could not find Roblox's FPS setting, so the FPS limit was not applied. Open Roblox once and try again."); }
        return;
    }
    int cur = std::atoi(m[2].str().c_str());
    if (cur != last) base = cur;
    int target = cap > 0 ? cap : base;
    if (target != cur) {
        auto since = std::chrono::steady_clock::now() - g_fpsChangedAt;
        if (since < std::chrono::seconds(4)) std::this_thread::sleep_for(std::chrono::seconds(4) - since);
        std::string out = std::regex_replace(xml, capRe, "$01" + std::to_string(target) + "$03", std::regex_constants::format_first_only);
        if (!WriteFile(file, out)) { Log("[!] Could not write Roblox's settings file for the FPS limit."); return; }
        g_fpsChangedAt = std::chrono::steady_clock::now();
    }
    std::lock_guard<std::mutex> lk(fpsMutex);
    g_fpsBase = base;
    g_fpsLast = target;
    SaveFpsCapsLocked();
}

struct ModDef {
    const char* id;
    const char* label;
    const wchar_t* stored;
    const char* magic;
    std::vector<const wchar_t*> targets;
};

static const std::vector<ModDef>& ModDefs() {
    static const std::vector<ModDef> defs = {
        { "cursor", "Cursor", L"cursor.png", "\x89PNG",
          { L"content\\textures\\Cursors\\KeyboardMouse\\ArrowCursor.png",
            L"content\\textures\\Cursors\\KeyboardMouse\\ArrowFarCursor.png" } },
        { "shiftlock", "Shift-lock cursor", L"shiftlock.png", "\x89PNG",
          { L"content\\textures\\MouseLockedCursor.png" } },
        { "death", "Death sound", L"death.ogg", "OggS",
          { L"content\\sounds\\ouch.ogg" } },
    };
    return defs;
}

static std::mutex g_modsMutex;
static std::set<std::wstring> g_modsWarned;

static fs::path ModsDir() { return fs::path(g_exeDir) / L"mods"; }
static fs::path ModNameFile(const ModDef& d) { fs::path p = ModsDir() / d.stored; p += L".name"; return p; }
static const ModDef* FindMod(const std::string& id) {
    for (auto& d : ModDefs()) if (id == d.id) return &d;
    return nullptr;
}

std::vector<ClientMod> ClientMods() {
    std::vector<ClientMod> out;
    for (auto& d : ModDefs()) {
        ClientMod m;
        m.id = d.id;
        m.label = d.label;
        std::error_code ec;
        m.active = fs::exists(ModsDir() / d.stored, ec);
        if (m.active) { ReadFile(ModNameFile(d), m.fileName); if (m.fileName.empty()) m.fileName = "Custom"; }
        out.push_back(m);
    }
    return out;
}

static fs::path BackupOf(const fs::path& target) { fs::path p = target; p += L".velsbak"; return p; }

static bool ApplyModFile(const fs::path& stored, const fs::path& target) {
    std::error_code ec;
    if (!fs::exists(target.parent_path(), ec)) return true;
    if (SameFile(stored, target)) return true;
    fs::path bak = BackupOf(target);
    if (!fs::exists(bak, ec) && fs::exists(target, ec) && !CopyFileW(target.c_str(), bak.c_str(), TRUE)) return false;
    return CopyFileW(stored.c_str(), target.c_str(), FALSE) != FALSE;
}

static bool RestoreModFile(const fs::path& target) {
    std::error_code ec;
    fs::path bak = BackupOf(target);
    if (!fs::exists(bak, ec)) return true;
    return MoveFileExW(bak.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

static bool ApplyModToStrap(const fs::path& strap, const fs::path& stored, const wchar_t* rel) {
    fs::path mod = strap / L"Modifications" / rel, keep = strap / L"VelsBackup" / rel;
    fs::path marker = keep;
    marker += L".applied";
    std::error_code ec;
    if (fs::exists(marker, ec) && SameFile(stored, mod)) return true;
    fs::create_directories(keep.parent_path(), ec);
    if (!fs::exists(marker, ec) && fs::exists(mod, ec) && !CopyFileW(mod.c_str(), keep.c_str(), FALSE)) return false;
    fs::create_directories(mod.parent_path(), ec);
    if (!CopyFileW(stored.c_str(), mod.c_str(), FALSE)) return false;
    return WriteFile(marker, "1");
}

static bool RestoreModInStrap(const fs::path& strap, const wchar_t* rel) {
    fs::path mod = strap / L"Modifications" / rel, keep = strap / L"VelsBackup" / rel;
    fs::path marker = keep;
    marker += L".applied";
    std::error_code ec;
    if (!fs::exists(marker, ec)) return true;
    if (fs::exists(keep, ec)) {
        if (!MoveFileExW(keep.c_str(), mod.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    } else {
        fs::remove(mod, ec);
    }
    fs::remove(marker, ec);
    return true;
}

static int ApplyMod(const ModDef& d, bool verbose) {
    fs::path stored = ModsDir() / d.stored;
    int failed = 0;
    auto warn = [&](const fs::path& where) {
        ++failed;
        if (g_modsWarned.insert(where.wstring() + L"|" + d.stored).second || verbose)
            Log(std::string("[!] Could not apply the custom ") + d.label + " to " + Utf8(where.filename().wstring()) +
                " (close Roblox, or restart as admin if it is in Program Files).");
    };
    for (const auto& install : RobloxInstalls()) {
        bool ok = true;
        for (const wchar_t* rel : d.targets) ok = ApplyModFile(stored, install / rel) && ok;
        if (!ok) warn(install);
    }
    for (const auto& strap : StrapDirs()) {
        bool ok = true;
        for (const wchar_t* rel : d.targets) ok = ApplyModToStrap(strap, stored, rel) && ok;
        if (!ok) warn(strap);
    }
    return failed;
}

static void EnsureClientMods() {
    std::lock_guard<std::mutex> lk(g_modsMutex);
    for (auto& d : ModDefs()) {
        std::error_code ec;
        if (fs::exists(ModsDir() / d.stored, ec)) ApplyMod(d, false);
    }
}

bool SetClientModFile(const std::string& id, const std::wstring& path) {
    const ModDef* d = FindMod(id);
    if (!d) return false;
    {
        unsigned char head[4] = {};
        FILE* f = _wfopen(path.c_str(), L"rb");
        size_t n = f ? fread(head, 1, 4, f) : 0;
        if (f) fclose(f);
        if (n != 4 || memcmp(head, d->magic, 4) != 0) {
            Log(std::string("[!] The ") + d->label + (std::string(d->magic) == "OggS" ? " has to be an .ogg file." : " has to be a .png image."));
            return false;
        }
    }
    std::lock_guard<std::mutex> lk(g_modsMutex);
    std::error_code ec;
    fs::create_directories(ModsDir(), ec);
    fs::path stored = ModsDir() / d->stored;
    if (!fs::equivalent(path, stored, ec) && !CopyFileW(path.c_str(), stored.c_str(), FALSE)) {
        Log("[!] Could not copy that file next to the tool.");
        return false;
    }
    WriteFile(ModNameFile(*d), Utf8(fs::path(path).filename().wstring()));
    if (ApplyMod(*d, true) == 0)
        Log(std::string("[v] Custom ") + d->label + " set. Restart Roblox to see it.");
    return true;
}

void ClearClientMod(const std::string& id) {
    const ModDef* d = FindMod(id);
    if (!d) return;
    std::lock_guard<std::mutex> lk(g_modsMutex);
    std::error_code ec;
    fs::remove(ModsDir() / d->stored, ec);
    fs::remove(ModNameFile(*d), ec);
    bool ok = true;
    for (const auto& install : RobloxInstalls())
        for (const wchar_t* rel : d->targets) ok = RestoreModFile(install / rel) && ok;
    for (const auto& strap : StrapDirs())
        for (const wchar_t* rel : d->targets) ok = RestoreModInStrap(strap, rel) && ok;
    if (ok) Log(std::string("[v] ") + d->label + " is back to Roblox's default. Restart Roblox to see it.");
    else Log(std::string("[!] Could not restore the default ") + d->label + " everywhere (close Roblox and try again).");
}

static const char* kAllowedFlags[] = {
    "DFIntCSGLevelOfDetailSwitchingDistance", "DFIntCSGLevelOfDetailSwitchingDistanceL12",
    "DFIntCSGLevelOfDetailSwitchingDistanceL23", "DFIntCSGLevelOfDetailSwitchingDistanceL34",
    "FFlagHandleAltEnterFullscreenManually", "DFFlagTextureQualityOverrideEnabled", "DFIntTextureQualityOverride",
    "FIntDebugForceMSAASamples", "DFFlagDisableDPIScale", "FFlagDebugGraphicsPreferD3D11", "FFlagDebugSkyGray",
    "DFFlagDebugPauseVoxelizer", "DFIntDebugFRMQualityLevelOverride", "FIntFRMMaxGrassDistance",
    "FIntFRMMinGrassDistance", "FFlagDebugGraphicsPreferVulkan", "FFlagDebugGraphicsPreferOpenGL",
    "FIntGrassMovementReducedMotionFactor",
};

std::mutex fastFlagsMutex;
std::map<std::string, std::string> fastFlags;
static std::mutex g_flagsApplyMutex;

static fs::path FlagsFilePath() { return fs::path(g_exeDir) / L"fflags.dat"; }
static bool IsIntFlag(const std::string& name) { return name.rfind("FInt", 0) == 0 || name.rfind("DFInt", 0) == 0; }

static bool ValidFlag(const std::string& name, const std::string& value) {
    bool known = false;
    for (const char* k : kAllowedFlags) if (name == k) known = true;
    if (!known) return false;
    if (!IsIntFlag(name)) return value == "true" || value == "false";
    if (value.empty() || value.size() > 9) return false;
    for (size_t i = 0; i < value.size(); ++i)
        if (!(isdigit((unsigned char)value[i]) || (i == 0 && value[i] == '-' && value.size() > 1))) return false;
    return true;
}

static void SaveFastFlagsLocked() {
    std::ofstream f(FlagsFilePath(), std::ios::trunc);
    for (auto& kv : fastFlags) f << kv.first << ' ' << kv.second << '\n';
}

void LoadFastFlags() {
    std::lock_guard<std::mutex> lk(fastFlagsMutex);
    fastFlags.clear();
    std::ifstream f(FlagsFilePath());
    std::string name, value;
    while (f >> name >> value) if (ValidFlag(name, value)) fastFlags[name] = value;
}

static std::string FastFlagsJson() {
    std::lock_guard<std::mutex> lk(fastFlagsMutex);
    if (fastFlags.empty()) return "";
    std::string o = "{";
    for (auto& kv : fastFlags) o += std::string(o.size() > 1 ? "," : "") + "\n  \"" + kv.first + "\": " + kv.second;
    return o + "\n}\n";
}

static void EnsureFastFlags() {
    std::lock_guard<std::mutex> lk(g_flagsApplyMutex);
    std::string json = FastFlagsJson();
    for (const auto& install : RobloxInstalls()) {
        fs::path dir = install / L"ClientSettings", file = dir / L"ClientAppSettings.json";
        fs::path marker = dir / L"vels_fflags", bak = BackupOf(file);
        std::error_code ec;
        bool ours = fs::exists(marker, ec);
        if (!json.empty()) {
            std::string cur;
            if (ours && ReadFile(file, cur) && cur == json) continue;
            fs::create_directories(dir, ec);
            if (!ours && fs::exists(file, ec)) CopyFileW(file.c_str(), bak.c_str(), FALSE);
            if (WriteFile(file, json)) WriteFile(marker, "1");
        } else if (ours) {
            if (fs::exists(bak, ec)) MoveFileExW(bak.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
            else fs::remove(file, ec);
            fs::remove(marker, ec);
        }
    }
}

bool SetFastFlag(const std::string& name, const std::string& value) {
    {
        std::lock_guard<std::mutex> lk(fastFlagsMutex);
        if (value.empty()) fastFlags.erase(name);
        else if (!ValidFlag(name, value)) return false;
        else fastFlags[name] = value;
        SaveFastFlagsLocked();
    }
    EnsureFastFlags();
    return true;
}

std::string ExportFastFlagsJson() {
    std::string json = FastFlagsJson();
    return json.empty() ? "{}\n" : json;
}

int ImportFastFlagsJson(const std::string& text, int& skipped) {
    skipped = 0;
    json::Value root;
    if (!json::Parse(text.rfind("\xEF\xBB\xBF", 0) == 0 ? text.substr(3) : text, root) || root.type != json::Value::Object) return -1;
    int taken = 0;
    {
        std::lock_guard<std::mutex> lk(fastFlagsMutex);
        for (auto& kv : root.o) {
            std::string value;
            if (kv.second.type == json::Value::Bool) value = kv.second.b ? "true" : "false";
            else if (kv.second.type == json::Value::Number) value = std::to_string((long long)kv.second.n);
            else if (kv.second.type == json::Value::String) {
                value = kv.second.s;
                for (char& c : value) c = (char)tolower((unsigned char)c);
            }
            if (ValidFlag(kv.first, value)) { fastFlags[kv.first] = value; ++taken; }
            else ++skipped;
        }
        SaveFastFlagsLocked();
    }
    EnsureFastFlags();
    return taken;
}

void ResetFastFlags() {
    {
        std::lock_guard<std::mutex> lk(fastFlagsMutex);
        fastFlags.clear();
        SaveFastFlagsLocked();
    }
    EnsureFastFlags();
    Log("[v] Fast flags cleared. Restart Roblox to see it.");
}

void PrepareRobloxInstalls() {
    EnsureCustomFont();
    EnsureClientMods();
    EnsureFastFlags();
}

std::mutex historyMutex;
std::vector<ServerVisit> serverHistory;
static const size_t kHistoryMax = 60;

static fs::path HistoryFilePath() { return fs::path(g_exeDir) / L"history.dat"; }

static void SaveServerHistoryLocked() {
    std::ofstream f(HistoryFilePath(), std::ios::trunc);
    for (auto& v : serverHistory)
        f << v.time << '\t' << v.placeId << '\t' << v.rootPlaceId << '\t' << v.gameId << '\t' << v.userId << '\t'
          << (v.linkCode.empty() ? "-" : v.linkCode) << '\t' << v.name << '\n';
}

void LoadServerHistory() {
    std::lock_guard<std::mutex> lk(historyMutex);
    serverHistory.clear();
    std::ifstream f(HistoryFilePath());
    std::string line;
    while (std::getline(f, line) && serverHistory.size() < kHistoryMax) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> p;
        size_t start = 0;
        for (int i = 0; i < 6; ++i) {
            size_t tab = line.find('\t', start);
            if (tab == std::string::npos) break;
            p.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if (p.size() != 6) continue;
        ServerVisit v;
        try { v.time = std::stoll(p[0]); v.placeId = std::stoll(p[1]); v.rootPlaceId = std::stoll(p[2]); v.userId = std::stoll(p[4]); }
        catch (...) { continue; }
        v.gameId = p[3];
        if (p[5] != "-") v.linkCode = p[5];
        v.name = line.substr(start);
        if (v.placeId > 0 && !v.gameId.empty()) serverHistory.push_back(v);
    }
}

static void AddServerVisit(ServerVisit v) {
    for (char& c : v.name) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    std::lock_guard<std::mutex> lk(historyMutex);
    serverHistory.erase(std::remove_if(serverHistory.begin(), serverHistory.end(),
        [&](const ServerVisit& o) { return o.gameId == v.gameId; }), serverHistory.end());
    serverHistory.insert(serverHistory.begin(), v);
    if (serverHistory.size() > kHistoryMax) serverHistory.resize(kHistoryMax);
    SaveServerHistoryLocked();
}

void ClearServerHistory() {
    std::lock_guard<std::mutex> lk(historyMutex);
    serverHistory.clear();
    SaveServerHistoryLocked();
}

void RemoveServerVisit(const std::string& gameId) {
    std::lock_guard<std::mutex> lk(historyMutex);
    serverHistory.erase(std::remove_if(serverHistory.begin(), serverHistory.end(),
        [&](const ServerVisit& o) { return o.gameId == gameId; }), serverHistory.end());
    SaveServerHistoryLocked();
}

void OnAccountLaunched(long long userId, long long placeId, const std::string& gameId, const std::string& linkCode) {
    SetDiscordGame(placeId);
    std::thread([userId, placeId, gameId, linkCode]() {
        ServerVisit v;
        v.time = (long long)std::time(nullptr);
        v.placeId = placeId;
        v.gameId = gameId;
        v.userId = userId;
        v.linkCode = linkCode;
        GameLookup game;
        if (LookupGame(placeId, game) || LookupGame(placeId, game)) {
            v.name = game.name;
            v.rootPlaceId = game.rootPlaceId;
            BeginPlaySession(userId, game);
        }
        if (v.name.empty()) v.name = "Place " + std::to_string(placeId);
        if (!v.gameId.empty()) { AddServerVisit(v); return; }

        std::string cookie;
        {
            std::lock_guard<std::mutex> lk(accountsMutex);
            for (auto& a : accounts) if (a.userId == userId) cookie = a.cookie;
        }
        if (cookie.empty()) return;
        for (int attempt = 0; attempt < 10; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(attempt == 0 ? 15 : 10));
            if (CountRobloxProcesses(true) == 0) return;
            long long nowPlace = 0, nowRoot = 0;
            std::string nowGame;
            if (!QueryPresence(userId, cookie, nowPlace, nowRoot, nowGame) || nowGame.empty()) continue;
            v.gameId = nowGame;
            if (nowPlace > 0) v.placeId = nowPlace;
            if (nowRoot > 0) v.rootPlaceId = nowRoot;
            AddServerVisit(v);
            if (nowPlace > 0 && nowPlace != placeId) SetDiscordGame(nowPlace);
            return;
        }
    }).detach();
}

std::mutex playtimeMutex;
std::map<long long, PlaytimeGame> playtimeGames;
std::vector<PlaytimeEntry> playtime;
std::map<long long, long long> playSessions;

struct PlaySession { long long universeId = 0; long long rootPlaceId = 0; bool sawAlive = false; std::chrono::steady_clock::time_point started, lastCheck; double carry = 0; };
static std::map<long long, PlaySession> g_sessions;
static bool g_playtimeDirty = false;
static std::atomic<bool> g_playtimeThread{ false };

static fs::path PlaytimeFilePath() {
    static fs::path path = []() {
        fs::path beside = fs::path(g_exeDir) / L"playtime.dat";
        const wchar_t* appdata = _wgetenv(L"APPDATA");
        if (!appdata || !*appdata) return beside;
        std::error_code ec;
        fs::path dir = fs::path(appdata) / L"VelsMultiTool";
        fs::create_directories(dir, ec);
        if (ec) return beside;
        fs::path p = dir / L"playtime.dat";
        if (!fs::exists(p, ec) && fs::exists(beside, ec)) fs::copy_file(beside, p, ec);
        return p;
    }();
    return path;
}

static void SavePlaytimeLocked() {
    std::ofstream f(PlaytimeFilePath(), std::ios::trunc);
    for (auto& kv : playtimeGames) {
        std::string name = kv.second.name;
        for (char& c : name) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        f << "G\t" << kv.first << '\t' << kv.second.rootPlaceId << '\t' << (kv.second.iconUrl.empty() ? "-" : kv.second.iconUrl) << '\t' << name << '\n';
    }
    for (auto& e : playtime) f << "T\t" << e.userId << '\t' << e.universeId << '\t' << e.seconds << '\t' << e.lastPlayed << '\n';
    g_playtimeDirty = false;
}

static std::vector<std::string> SplitTabs(const std::string& line, size_t maxParts) {
    std::vector<std::string> p;
    size_t start = 0;
    while (p.size() + 1 < maxParts) {
        size_t tab = line.find('\t', start);
        if (tab == std::string::npos) break;
        p.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    p.push_back(line.substr(start));
    return p;
}

static void AddPlaySecondsLocked(long long userId, long long universeId, long long seconds) {
    for (auto& e : playtime)
        if (e.userId == userId && e.universeId == universeId) { e.seconds += seconds; e.lastPlayed = (long long)std::time(nullptr); return; }
    playtime.push_back({ userId, universeId, seconds, (long long)std::time(nullptr) });
}

static void PlaytimeThread() {
    auto lastSave = std::chrono::steady_clock::now(), lastWhere = lastSave;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        PruneLaunchedPids();
        std::set<long long> alive;
        { std::lock_guard<std::mutex> lk(launchedMutex); for (auto& kv : launchedPids) alive.insert(kv.first); }
        auto now = std::chrono::steady_clock::now();
        bool checkWhere = now - lastWhere > std::chrono::minutes(4);
        std::vector<std::pair<long long, long long>> toCheck;
        {
            std::lock_guard<std::mutex> lk(playtimeMutex);
            for (auto it = g_sessions.begin(); it != g_sessions.end();) {
                PlaySession& ps = it->second;
                double dt = std::chrono::duration<double>(now - ps.lastCheck).count();
                ps.lastCheck = now;
                if (alive.count(it->first)) {
                    ps.sawAlive = true;
                    ps.carry += std::min(dt, 30.0);
                    long long whole = (long long)ps.carry;
                    if (whole > 0) { ps.carry -= whole; AddPlaySecondsLocked(it->first, ps.universeId, whole); g_playtimeDirty = true; }
                    if (checkWhere) toCheck.push_back({ it->first, ps.rootPlaceId });
                    ++it;
                } else if (ps.sawAlive || now - ps.started > std::chrono::seconds(120)) {
                    playSessions.erase(it->first);
                    it = g_sessions.erase(it);
                } else ++it;
            }
            if (g_playtimeDirty && now - lastSave > std::chrono::seconds(30)) { SavePlaytimeLocked(); lastSave = now; }
        }
        if (checkWhere) {
            lastWhere = now;
            for (auto& uc : toCheck) {
                std::string cookie;
                { std::lock_guard<std::mutex> lk(accountsMutex); for (auto& a : accounts) if (a.userId == uc.first) cookie = a.cookie; }
                long long place = 0, root = 0;
                std::string gameId;
                if (cookie.empty() || !QueryPresence(uc.first, cookie, place, root, gameId) || root <= 0 || root == uc.second) continue;
                GameLookup game;
                if (LookupGame(place > 0 ? place : root, game)) BeginPlaySession(uc.first, game);
            }
        }
    }
}

void LoadPlaytime() {
    {
        std::lock_guard<std::mutex> lk(playtimeMutex);
        playtimeGames.clear();
        playtime.clear();
        std::ifstream f(PlaytimeFilePath());
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            try {
                if (line.rfind("G\t", 0) == 0) {
                    auto p = SplitTabs(line, 5);
                    if (p.size() != 5) continue;
                    PlaytimeGame g;
                    g.universeId = std::stoll(p[1]);
                    g.rootPlaceId = std::stoll(p[2]);
                    if (p[3] != "-") g.iconUrl = p[3];
                    g.name = p[4];
                    playtimeGames[g.universeId] = g;
                } else if (line.rfind("T\t", 0) == 0) {
                    auto p = SplitTabs(line, 5);
                    if (p.size() != 5) continue;
                    playtime.push_back({ std::stoll(p[1]), std::stoll(p[2]), std::stoll(p[3]), std::stoll(p[4]) });
                }
            } catch (...) {}
        }
    }
    if (!g_playtimeThread.exchange(true)) std::thread(PlaytimeThread).detach();
}

void BeginPlaySession(long long userId, const GameLookup& game) {
    if (userId <= 0 || game.universeId <= 0) return;
    std::lock_guard<std::mutex> lk(playtimeMutex);
    PlaytimeGame& g = playtimeGames[game.universeId];
    g.universeId = game.universeId;
    if (game.rootPlaceId > 0) g.rootPlaceId = game.rootPlaceId;
    if (!game.name.empty()) g.name = game.name;
    if (!game.iconUrl.empty()) g.iconUrl = game.iconUrl;
    auto now = std::chrono::steady_clock::now();
    auto it = g_sessions.find(userId);
    if (it != g_sessions.end() && it->second.sawAlive) {
        it->second.universeId = game.universeId;
        it->second.rootPlaceId = g.rootPlaceId;
    } else {
        PlaySession ps;
        ps.universeId = game.universeId;
        ps.rootPlaceId = g.rootPlaceId;
        ps.started = ps.lastCheck = now;
        g_sessions[userId] = ps;
    }
    playSessions[userId] = game.universeId;
    g_playtimeDirty = true;
}

void ClearPlaytime() {
    std::lock_guard<std::mutex> lk(playtimeMutex);
    playtime.clear();
    playtimeGames.clear();
    SavePlaytimeLocked();
}

}
