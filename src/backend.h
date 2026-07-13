// Backend logic for Vels Multi Tool: Roblox multi-instance watcher,
// cookie cleaner, and MAC address spoofer. No UI code here — the ImGui
// front-end in main.cpp polls these globals/functions every frame.
#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>

struct NetworkAdapterInfo {
    std::string id;
    std::string description;
    std::string connectionName;
    std::string currentMac;
    bool isActive = false; // true if this is the adapter currently used for the default route
};

struct RobloxAccount {
    std::string username;
    long long userId = 0;
    std::string cookie; // .ROBLOSECURITY value - kept only in memory + the encrypted accounts.dat
    std::string password; // optional; stored in the same DPAPI-encrypted accounts.dat
    std::string alias; // optional display name shown instead of username; stored the same way

    // Profile stats, fetched lazily by backend::FetchAccountStats(). -1 = not loaded yet.
    // All access happens under accountsMutex, same as the fields above.
    long long friendsCount = -1;
    long long followersCount = -1;
    long long followingCount = -1;
    long long robuxBalance = -1;
    std::string joinDate;        // "Jan 12, 2022"
    std::string accountAge;      // "2 years, 4 months"
    bool statsLoaded = false;
    bool statsRequested = false; // a fetch thread has been kicked off at least once

    std::vector<unsigned char> avatarPng; // raw headshot bytes from backend::FetchAccountAvatar(); empty until loaded
    bool avatarLoaded = false;
    bool avatarRequested = false;
};

struct PlaceInfo {
    long long placeId = 0;
    std::string name;
    std::string creator;
    long long visits = -1;
    long long favorites = -1;
    std::vector<unsigned char> iconPng;
    bool loaded = false;
    bool requested = false;
};

struct ActivityEntry {
    std::string title;
    std::string subtitle;
    long long unixSeconds = 0;
};

struct SystemStatus {
    bool robloxApiOk = false;
    bool authOk = false;
    bool accountStoreOk = false;
    bool checked = false;
};

struct LogEntry {
    std::string time; // "HH:MM:SS"
    std::string text;
};

struct BrowserCookieStatus {
    std::string name;
    bool installed = false;  // browser's profile directory exists on this machine
    bool found = false;      // a roblox.com cookie row was found in at least one profile
    int count = 0;           // total matching cookie rows across all profiles
    bool scanFailed = false; // a profile's Cookies DB couldn't be copied/opened/queried -
                              // distinct from a confirmed zero, so the UI doesn't claim "no
                              // cookies" when the scan actually just couldn't check.
};

namespace backend {

// --- shared state the UI polls each frame ---
extern std::mutex logMutex;
extern std::vector<LogEntry> logLines;
void ClearLog();

extern std::atomic<bool> watching;
extern std::atomic<int> instanceCount;

extern std::mutex adaptersMutex;
extern std::vector<NetworkAdapterInfo> adapters;
extern std::atomic<int> defaultAdapterIndex; // index into adapters[] currently used for the default route, or -1

void Log(const std::string& msg);

bool IsElevated();
bool RelaunchAsAdmin();

void Init(const std::wstring& exeDir);
void Shutdown();

void StartWatching();
void StopWatching();
void LaunchNewInstance();
void KillAllRobloxInstances(); // runs synchronously, call from a worker thread

// Closes any current Roblox singleton lock(s) once, immediately, without
// starting the continuous watcher - runs synchronously, call from a worker thread.
void CloseRobloxSingletonsNow();

// Live system stats for the dashboard. Internally throttled/cached so polling
// them every UI frame doesn't re-snapshot the process list or system times.
int CountRobloxProcesses(bool force = false);
float GetCpuUsagePercent();
float GetMemoryUsagePercent();
std::string GetUptimeString(); // "HH:MM:SS" since backend::Init()

void ClearRobloxCookieFile(); // runs synchronously, call from a worker thread
bool RobloxCookieFileHasData(); // read-only status for the Cookie Cleaner UI
void ClearBrowserCookies(); // runs synchronously, call from a worker thread
void ClearRobloxCookieFileAndBrowsers(); // runs synchronously, call from a worker thread

extern std::mutex browserCookieMutex;
extern std::vector<BrowserCookieStatus> browserCookieStatus;
extern std::atomic<bool> browserCookieScanning;
extern std::atomic<bool> browserCookieScanned; // true once at least one scan has completed
void ScanBrowserCookies(); // runs synchronously (read-only), call from a worker thread

void RefreshAdapters();
void SpoofAdapter(int index);
void RestoreAdapter(int index);

// --- account manager (separate, locally-encrypted store; never touches Roblox's own cookie files) ---
extern std::mutex accountsMutex;
extern std::vector<RobloxAccount> accounts;

void LoadAccounts();   // reads + DPAPI-decrypts accounts.dat next to the exe, populates accounts
void SaveAccounts();   // DPAPI-encrypts + writes accounts.dat

// Validates the cookie against the Roblox API (fetches username/userId) and, on
// success, appends it to accounts + persists. Returns false if the cookie is invalid.
bool AddAccountFromCookie(const std::string& cookie, const std::string& password = "");
void RemoveAccount(int index);
void SetAccountPassword(int index, const std::string& password);
void SetAccountAlias(int index, const std::string& alias);

// Exchanges the account's long-lived cookie for a one-time auth ticket and
// launches the Roblox client into the given place via the roblox-player: protocol.
void LaunchAccountIntoPlace(int index, long long placeId);

// Launches a separate browser instance with its own per-account profile, injects
// the account's cookie, and opens roblox.com logged in. Call from a worker thread.
void OpenAccountWeb(int index);

// Populates accounts[index]'s friends/followers/following/robux/join-date fields
// from Roblox's API. Runs synchronously, call from a worker thread.
void FetchAccountStats(int index);

// Downloads the account's avatar headshot PNG into accounts[index].avatarPng.
// Runs synchronously (network), call from a worker thread.
void FetchAccountAvatar(int index);

// Currently-configured place's cached name/visits/favorites/icon.
extern std::mutex placeInfoMutex;
extern PlaceInfo placeInfo;
// Fetches name/visits/favorites/icon for placeId into placeInfo. Runs
// synchronously, call from a worker thread.
// cookie should be any saved account's .ROBLOSECURITY - multiget-place-details
// requires authentication even though it returns public game info.
void FetchPlaceInfo(long long placeId, const std::string& cookie);

// The place ID configured on the Accounts page, persisted to placeid.dat
// next to the exe so it survives restarts. 0 = none saved yet.
extern std::atomic<long long> savedPlaceId;
void SavePlaceId(long long placeId);
void LoadPlaceId(); // called once from Init()

// Recent-activity feed, newest first. Populated by real app events (account
// added/removed, place ID saved, launch attempted) - not Roblox API data.
extern std::mutex activityMutex;
extern std::vector<ActivityEntry> activityLog;
void AddActivity(const std::string& title, const std::string& subtitle);
std::string RelativeTimeString(long long unixSeconds); // "2 hours ago"

// Lightweight health checks for the Accounts page status panel.
extern std::mutex systemStatusMutex;
extern SystemStatus systemStatus;
void RefreshSystemStatus(int selectedAccountIndex); // runs synchronously, call from a worker thread

} // namespace backend
