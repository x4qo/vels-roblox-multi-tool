
#include "backend.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace backend {

extern std::wstring g_exeDir;

std::mutex customFontMutex;
CustomFontState customFont;

static std::atomic<bool> g_fontEnabled{ false };
static std::mutex g_fontWorkMutex;
static std::set<std::wstring> g_fontWarned;

static const wchar_t* kFontFile = L"CustomFont.ttf";
static const wchar_t* kBackupDir = L"families.velsbak";
static const wchar_t* kStampFile = L"vels_font.stamp";
static const char* kFontAsset = "rbxasset://fonts/CustomFont.ttf";

static fs::path StoredFontPath() { return fs::path(g_exeDir) / L"customfont.ttf"; }
static fs::path FontSettingsPath() { return fs::path(g_exeDir) / L"font.dat"; }

static std::string Utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static void SaveFontSettings() {
    CustomFontState s;
    { std::lock_guard<std::mutex> lk(customFontMutex); s = customFont; }
    std::ofstream f(FontSettingsPath(), std::ios::trunc);
    if (f) f << (s.enabled ? 1 : 0) << '\n' << s.name << '\n';
}

static std::string FontStamp() {
    std::error_code ec;
    auto size = fs::file_size(StoredFontPath(), ec);
    if (ec) return "";
    auto t = fs::last_write_time(StoredFontPath(), ec);
    if (ec) return "";
    return std::to_string(size) + "-" + std::to_string(t.time_since_epoch().count());
}

static bool LooksLikeFont(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    unsigned char m[4] = {};
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    if (n != 4) return false;
    return memcmp(m, "\x00\x01\x00\x00", 4) == 0 || memcmp(m, "OTTO", 4) == 0 ||
           memcmp(m, "ttcf", 4) == 0 || memcmp(m, "true", 4) == 0;
}

static const wchar_t* kStraps[] = { L"Bloxstrap", L"Fishstrap" };

std::vector<fs::path> StrapDirs() {
    std::vector<fs::path> out;
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    if (!local) return out;
    for (const wchar_t* name : kStraps) {
        std::error_code ec;
        fs::path dir = fs::path(local) / name;
        if (fs::is_directory(dir, ec)) out.push_back(dir);
    }
    return out;
}

std::vector<fs::path> RobloxInstalls() {
    std::vector<fs::path> roots;
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA")) roots.push_back(fs::path(local) / L"Roblox" / L"Versions");
    if (const wchar_t* pf86 = _wgetenv(L"ProgramFiles(x86)")) roots.push_back(fs::path(pf86) / L"Roblox" / L"Versions");
    if (const wchar_t* pf = _wgetenv(L"ProgramFiles")) roots.push_back(fs::path(pf) / L"Roblox" / L"Versions");
    roots.push_back(fs::path(g_exeDir) / L"Builds");

    std::vector<fs::path> out;
    for (const auto& strap : StrapDirs()) {
        std::error_code ec;
        roots.push_back(strap / L"Versions");
        fs::path player = strap / L"Roblox" / L"Player";
        if (fs::exists(player / L"content" / L"fonts" / L"families", ec)) out.push_back(player);
    }
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        for (auto& entry : fs::directory_iterator(root, ec)) {
            if (ec || !entry.is_directory(ec)) continue;
            if (fs::exists(entry.path() / L"content" / L"fonts" / L"families", ec)) out.push_back(entry.path());
        }
    }
    return out;
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

static bool BackupFamilies(const fs::path& families, const fs::path& backup) {
    std::error_code ec;
    if (fs::exists(backup, ec)) return true;
    fs::path tmp = backup;
    tmp += L".tmp";
    fs::remove_all(tmp, ec);
    if (!fs::create_directories(tmp, ec)) return false;
    for (auto& e : fs::directory_iterator(families, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (!CopyFileW(e.path().c_str(), (tmp / e.path().filename()).c_str(), FALSE)) {
            fs::remove_all(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, backup, ec);
    return !ec;
}

enum class FontResult { Applied, Fresh, Failed };

static FontResult ApplyToInstall(const fs::path& install, const std::string& stamp) {
    fs::path fonts = install / L"content" / L"fonts";
    fs::path families = fonts / L"families", backup = fonts / kBackupDir;
    fs::path fontFile = fonts / kFontFile, stampFile = fonts / kStampFile;
    std::error_code ec;

    std::string have;
    if (ReadFile(stampFile, have) && have == stamp && fs::exists(fontFile, ec) && fs::exists(backup, ec))
        return FontResult::Fresh;

    if (!BackupFamilies(families, backup)) return FontResult::Failed;
    if (!CopyFileW(StoredFontPath().c_str(), fontFile.c_str(), FALSE)) return FontResult::Failed;

    static const std::regex assetRe("(\"assetId\"\\s*:\\s*)\"[^\"]*\"");
    const std::string replacement = std::string("$1\"") + kFontAsset + "\"";
    for (auto& e : fs::directory_iterator(backup, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != L".json") continue;
        std::string json;
        if (!ReadFile(e.path(), json)) return FontResult::Failed;
        if (!WriteFile(families / e.path().filename(), std::regex_replace(json, assetRe, replacement)))
            return FontResult::Failed;
    }
    return WriteFile(stampFile, stamp) ? FontResult::Applied : FontResult::Failed;
}

static bool RestoreInstall(const fs::path& install, bool& changed) {
    fs::path fonts = install / L"content" / L"fonts";
    fs::path families = fonts / L"families", backup = fonts / kBackupDir;
    std::error_code ec;
    changed = false;
    if (!fs::exists(backup, ec)) return true;
    for (auto& e : fs::directory_iterator(backup, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (!CopyFileW(e.path().c_str(), (families / e.path().filename()).c_str(), FALSE)) return false;
    }
    fs::remove_all(backup, ec);
    fs::remove(fonts / kStampFile, ec);
    fs::remove(fonts / kFontFile, ec);
    changed = true;
    return true;
}

static FontResult ApplyToStrap(const fs::path& strap, const std::string& stamp) {
    fs::path fonts = strap / L"Modifications" / L"content" / L"fonts";
    fs::path fontFile = fonts / kFontFile, stampFile = strap / kStampFile, backup = strap / L"CustomFont.velsbak.ttf";
    std::error_code ec, ec2;

    std::string have;
    bool stamped = ReadFile(stampFile, have);
    auto theirs = fs::file_size(fontFile, ec), ours = fs::file_size(StoredFontPath(), ec2);
    if (stamped && have == stamp && !ec && !ec2 && theirs == ours) return FontResult::Fresh;

    if (!stamped && !ec && !CopyFileW(fontFile.c_str(), backup.c_str(), FALSE)) return FontResult::Failed;
    fs::create_directories(fonts, ec);
    if (!CopyFileW(StoredFontPath().c_str(), fontFile.c_str(), FALSE)) return FontResult::Failed;
    return WriteFile(stampFile, stamp) ? FontResult::Applied : FontResult::Failed;
}

static bool RestoreStrap(const fs::path& strap, bool& changed) {
    fs::path fontFile = strap / L"Modifications" / L"content" / L"fonts" / kFontFile;
    fs::path stampFile = strap / kStampFile, backup = strap / L"CustomFont.velsbak.ttf";
    std::error_code ec;
    changed = false;
    if (!fs::exists(stampFile, ec)) return true;
    if (fs::exists(backup, ec)) {
        if (!MoveFileExW(backup.c_str(), fontFile.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    } else {
        fs::remove(fontFile, ec);
    }
    fs::remove(stampFile, ec);
    changed = true;
    return true;
}

static void ApplyAll(bool verbose) {
    std::lock_guard<std::mutex> work(g_fontWorkMutex);
    std::string stamp = FontStamp();
    if (stamp.empty()) {
        if (verbose) Log("[!] The custom font file is missing. Choose a font again.");
        return;
    }
    int applied = 0, fresh = 0, failed = 0;
    auto tally = [&](FontResult r, const fs::path& where) {
        switch (r) {
        case FontResult::Applied: ++applied; g_fontWarned.erase(where.wstring()); break;
        case FontResult::Fresh:   ++fresh; break;
        case FontResult::Failed:
            ++failed;
            if (g_fontWarned.insert(where.wstring()).second || verbose)
                Log("[!] Could not apply the custom font to " + Utf8(where.filename().wstring()) +
                    " (close Roblox, or restart as admin if it is in Program Files).");
            break;
        }
    };
    for (const auto& install : RobloxInstalls()) tally(ApplyToInstall(install, stamp), install);
    for (const auto& strap : StrapDirs()) tally(ApplyToStrap(strap, stamp), strap);
    std::string name;
    { std::lock_guard<std::mutex> lk(customFontMutex); name = customFont.name; }
    if (verbose) {
        if (applied + fresh == 0 && failed == 0) Log("[i] Custom font saved. No Roblox install found yet - it is applied when you launch.");
        else if (applied + fresh > 0)
            Log("[v] Roblox font set to " + name + " on " + std::to_string(applied + fresh) + " install" +
                (applied + fresh == 1 ? "" : "s") + ". Restart Roblox to see it.");
    } else if (applied > 0) {
        Log("[i] Custom font applied to " + std::to_string(applied) + " new Roblox install" + (applied == 1 ? "" : "s") + ".");
    }
}

static void RestoreAll() {
    std::lock_guard<std::mutex> work(g_fontWorkMutex);
    int restored = 0, failed = 0;
    for (const auto& install : RobloxInstalls()) {
        bool changed = false;
        if (!RestoreInstall(install, changed)) {
            ++failed;
            Log("[!] Could not restore the original fonts in " + Utf8(install.filename().wstring()) +
                " (close Roblox, or restart as admin if it is in Program Files).");
        } else if (changed) ++restored;
    }
    for (const auto& strap : StrapDirs()) {
        bool changed = false;
        if (!RestoreStrap(strap, changed)) {
            ++failed;
            Log("[!] Could not put back the font " + Utf8(strap.filename().wstring()) + " had before.");
        } else if (changed) ++restored;
    }
    g_fontWarned.clear();
    if (!failed) Log("[v] Roblox fonts are back to default" + std::string(restored ? ". Restart Roblox to see it." : "."));
}

void LoadCustomFont() {
    CustomFontState s;
    {
        std::ifstream f(FontSettingsPath());
        int on = 0;
        std::string line;
        if (f && std::getline(f, line)) on = std::atoi(line.c_str());
        if (f && std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            s.name = line;
        }
        std::error_code ec;
        s.hasFont = fs::exists(StoredFontPath(), ec);
        s.enabled = on != 0 && s.hasFont;
        if (s.name.empty()) s.name = "Custom font";
    }
    { std::lock_guard<std::mutex> lk(customFontMutex); customFont = s; }
    g_fontEnabled = s.enabled;
    if (s.enabled) std::thread([]() { ApplyAll(false); }).detach();
}

bool SetCustomFontFile(const std::wstring& path) {
    if (!LooksLikeFont(path)) {
        Log("[!] That file is not a TrueType or OpenType font (.ttf, .otf, .ttc).");
        return false;
    }
    {
        std::lock_guard<std::mutex> work(g_fontWorkMutex);
        std::error_code ec;
        if (!fs::equivalent(path, StoredFontPath(), ec) && !CopyFileW(path.c_str(), StoredFontPath().c_str(), FALSE)) {
            Log("[!] Could not copy the font next to the tool.");
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(customFontMutex);
        customFont.name = Utf8(fs::path(path).stem().wstring());
        customFont.hasFont = true;
        customFont.enabled = true;
    }
    g_fontEnabled = true;
    SaveFontSettings();
    ApplyAll(true);
    AddActivity("Roblox font changed", Utf8(fs::path(path).stem().wstring()));
    return true;
}

void SetCustomFontEnabled(bool on) {
    {
        std::lock_guard<std::mutex> lk(customFontMutex);
        if (on && !customFont.hasFont) on = false;
        customFont.enabled = on;
    }
    g_fontEnabled = on;
    SaveFontSettings();
    if (on) ApplyAll(true);
    else RestoreAll();
}

void EnsureCustomFont() {
    if (g_fontEnabled) ApplyAll(false);
}

}
