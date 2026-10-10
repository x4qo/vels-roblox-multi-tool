
#include "backend.h"
#include "json.h"

#include <windows.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <thread>

namespace backend {

extern std::wstring g_exeDir;

std::atomic<bool> discordEnabled{ false };
std::atomic<bool> discordConnected{ false };
std::atomic<int> discordTimeMode{ 0 };
std::atomic<int> discordTimeOffset{ 0 };
static std::atomic<int> g_settingsGen{ 0 };

static const char* kDiscordAppId = "1005469189907173486";

static std::mutex g_gameMutex;
static long long g_gamePlace = 0;
static long long g_gameSince = 0;
static std::atomic<bool> g_threadStarted{ false };

static std::wstring DiscordFilePath() { return g_exeDir + L"\\discord.dat"; }

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static int g_nonce = 0;

static void ClosePipe() {
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
    discordConnected = false;
}

static bool WriteFrame(int op, const std::string& json) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    std::string buf(8, 0);
    int len = (int)json.size();
    memcpy(&buf[0], &op, 4);
    memcpy(&buf[4], &len, 4);
    buf += json;
    DWORD written = 0;
    if (!WriteFile(g_pipe, buf.data(), (DWORD)buf.size(), &written, nullptr) || written != buf.size()) {
        ClosePipe();
        return false;
    }
    return true;
}

static void DrainPipe(std::string* lastFrame = nullptr) {
    while (g_pipe != INVALID_HANDLE_VALUE) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr)) { ClosePipe(); return; }
        if (avail < 8) return;
        int head[2] = {};
        DWORD got = 0;
        if (!ReadFile(g_pipe, head, 8, &got, nullptr) || got != 8 || head[1] < 0 || head[1] > (1 << 20)) { ClosePipe(); return; }
        std::string body((size_t)head[1], 0);
        DWORD total = 0;
        while (total < (DWORD)head[1]) {
            if (!ReadFile(g_pipe, &body[total], (DWORD)head[1] - total, &got, nullptr) || got == 0) { ClosePipe(); return; }
            total += got;
        }
        if (head[0] == 3) WriteFrame(4, body);
        else if (head[0] == 2) { ClosePipe(); return; }
        if (lastFrame) *lastFrame = body;
    }
}

static bool ConnectPipe() {
    for (int i = 0; i < 10; ++i) {
        std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        g_pipe = h;
        if (!WriteFrame(0, std::string("{\"v\":1,\"client_id\":\"") + kDiscordAppId + "\"}")) continue;
        std::string ready;
        for (int waited = 0; waited < 20 && ready.empty() && g_pipe != INVALID_HANDLE_VALUE; ++waited) {
            Sleep(100);
            DrainPipe(&ready);
        }
        if (g_pipe != INVALID_HANDLE_VALUE && ready.find("\"READY\"") != std::string::npos) {
            discordConnected = true;
            return true;
        }
        ClosePipe();
    }
    return false;
}

static bool SendActivity(const std::string& activityJson) {
    std::string msg = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
                      ",\"activity\":" + activityJson + "},\"nonce\":\"" + std::to_string(++g_nonce) + "\"}";
    return WriteFrame(1, msg);
}

static std::string ActivityJson(long long placeId, long long since) {
    GameLookup game;
    if (!LookupGame(placeId, game)) game.name = "Roblox";
    long long page = game.rootPlaceId > 0 ? game.rootPlaceId : placeId;
    std::string o = "{\"type\":0,\"details\":" + json::Quote(game.name);
    int mode = discordTimeMode.load();
    if (mode != 2) o += ",\"timestamps\":{\"start\":" + std::to_string(mode == 1 ? since - discordTimeOffset.load() : since) + "}";
    if (!game.iconUrl.empty())
        o += ",\"assets\":{\"large_image\":" + json::Quote(game.iconUrl) + ",\"large_text\":" + json::Quote(game.name) + "}";
    o += ",\"buttons\":[{\"label\":\"View game\",\"url\":\"https://www.roblox.com/games/" + std::to_string(page) + "\"}]}";
    return o;
}

static void DiscordThread() {
    long long shownPlace = 0, shownSince = 0;
    int retryIn = 0, shownGen = -1;
    for (;;) {
        Sleep(2000);
        long long place, since;
        {
            std::lock_guard<std::mutex> lk(g_gameMutex);
            if (g_gamePlace && std::time(nullptr) - g_gameSince > 30 && CountRobloxProcesses() == 0) g_gamePlace = 0;
            place = g_gamePlace;
            since = g_gameSince;
        }
        if (!discordEnabled) place = 0;

        if (place == 0) {
            if (g_pipe != INVALID_HANDLE_VALUE) {
                if (shownPlace) SendActivity("null");
                ClosePipe();
            }
            shownPlace = 0;
            continue;
        }
        if (g_pipe == INVALID_HANDLE_VALUE) {
            if (retryIn > 0) { --retryIn; continue; }
            if (!ConnectPipe()) { retryIn = 5; continue; }
            shownPlace = 0;
        }
        int gen = g_settingsGen.load();
        if (place != shownPlace || since != shownSince || gen != shownGen) {
            if (SendActivity(ActivityJson(place, since))) { shownPlace = place; shownSince = since; shownGen = gen; }
        }
        std::string reply;
        DrainPipe(&reply);
        if (reply.find("\"evt\":\"ERROR\"") != std::string::npos) {
            static bool warned = false;
            if (!warned) { warned = true; Log("[!] Discord did not accept the game status."); }
        }
    }
}

static void SaveDiscordSettings() {
    std::ofstream f(DiscordFilePath().c_str(), std::ios::trunc);
    if (f) f << (discordEnabled ? 1 : 0) << ' ' << discordTimeMode.load() << ' ' << discordTimeOffset.load();
}

void SetDiscordTime(int mode, int offsetSeconds) {
    discordTimeMode = mode < 0 || mode > 2 ? 0 : mode;
    discordTimeOffset = offsetSeconds < 0 ? 0 : (offsetSeconds > 9999 * 3600 ? 9999 * 3600 : offsetSeconds);
    ++g_settingsGen;
    SaveDiscordSettings();
}

void LoadDiscordSettings() {
    std::ifstream f(DiscordFilePath().c_str());
    int on = 0, mode = 0, offset = 0;
    if (f >> on) discordEnabled = on != 0;
    if (f >> mode >> offset) { discordTimeMode = mode < 0 || mode > 2 ? 0 : mode; discordTimeOffset = offset < 0 ? 0 : offset; }
    if (!g_threadStarted.exchange(true)) std::thread(DiscordThread).detach();
}

void SetDiscordEnabled(bool on) {
    discordEnabled = on;
    SaveDiscordSettings();
}

void SetDiscordGame(long long placeId) {
    std::lock_guard<std::mutex> lk(g_gameMutex);
    if (placeId == g_gamePlace) return;
    g_gamePlace = placeId;
    g_gameSince = (long long)std::time(nullptr);
}

}
