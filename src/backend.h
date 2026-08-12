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
    long long playing = -1;    // live player count
    long long maxPlayers = -1; // server size
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
// Set by the UI loop. While the window isn't the foreground one, the polled
// dashboard stats (process snapshots in particular) back off hard - nobody is
// reading them, and a full system process snapshot is not free.
extern std::atomic<bool> uiForeground;

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

// Same as above, but joins a private server via its link code (the code behind a
// roblox.com/share?code=...&type=Server link, or a ?privateServerLinkCode= URL).
void LaunchAccountIntoPrivateServer(int index, long long placeId, const std::string& linkCode);

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

// Saved place-ID presets shown in the Launch Settings dropdown, persisted to
// places.dat next to the exe (one "id name" per line). Seeded with a default
// set on first run so the dropdown is never empty.
struct SavedPlace { long long id; std::string name; };
extern std::mutex savedPlacesMutex;
extern std::vector<SavedPlace> savedPlaces;
void LoadSavedPlaces();                                  // called once from Init()
void AddSavedPlace(long long id, const std::string& name); // insert or update by id, then persist
void RemoveSavedPlace(long long id);                     // remove by id, then persist

// --- private servers -------------------------------------------------------
// A private server is identified by its link code plus the place it belongs to.
struct PrivateServer {
    long long   placeId = 0;
    std::string linkCode;   // the "code" from a share link, or privateServerLinkCode
    std::string name;       // user-given label (saved presets only)
};

// The private server the next launch should join. Empty linkCode = join the
// public game normally. Persisted to privateserver.dat next to the exe.
extern std::mutex activePrivateServerMutex;
extern PrivateServer activePrivateServer;
void LoadActivePrivateServer();                    // called once from Init()
void SetActivePrivateServer(const PrivateServer& ps); // persists + logs
void ClearActivePrivateServer();

// Saved private-server presets, persisted to privateservers.dat next to the exe
// (one "placeId linkCode name" per line).
extern std::mutex privateServersMutex;
extern std::vector<PrivateServer> savedPrivateServers;
void LoadSavedPrivateServers();                          // called once from Init()
void AddSavedPrivateServer(const PrivateServer& ps);     // insert or update by link code
void RemoveSavedPrivateServer(const std::string& linkCode);

// Turns any of these into a usable {placeId, linkCode}:
//   https://www.roblox.com/share?code=<32 hex>&type=Server   (resolved via the API)
//   https://www.roblox.com/games/<placeId>/x?privateServerLinkCode=<code>
//   a bare share code
// Share links need an authenticated cookie to resolve. Runs synchronously
// (network) - call from a worker thread. Returns false and logs on failure.
bool ResolvePrivateServerLink(const std::string& input, const std::string& cookie, PrivateServer& out);
// True while a resolve is in flight, for the UI spinner/label.
extern std::atomic<bool> privateServerResolving;

// --- Roblox build manager (downgrade / force live) --------------------------
// Builds are downloaded straight from Roblox's deployment CDN the same way RDD
// (rdd.weao.gg) does it - package manifest, per-package zips, AppSettings.xml -
// into "Builds\<version-hash>" next to the exe. The active build, if any, is
// what every launch runs instead of the system install.
struct RobloxBuildState {
    std::string liveVersion;    // WEAO current Windows hash
    std::string liveDate;       // WEAO current Windows timestamp
    std::string futureVersion;  // WEAO next Windows hash (may be empty)
    std::string pastVersion;    // WEAO previous Windows hash (may be empty)
    std::string pastDate;       // WEAO previous Windows timestamp
    bool        weaoLoaded = false;
    std::string activeVersion;  // "" = launch the system-installed client
    std::string preferredVersion; // last build that was switched on, kept across toggling off
    std::string status;         // human-readable progress line for the UI
    float       progress = 0.0f;// 0..1 while busy
    bool        busy = false;
};
extern std::mutex robloxBuildMutex;
extern RobloxBuildState robloxBuild;
extern std::vector<std::string> downloadedBuilds; // hashes present under Builds\ (same mutex)

void LoadRobloxBuilds();   // called once from Init(): scans Builds\ + activebuild.dat
void FetchWeaoVersions();  // GETs weao.xyz current+future. Call from a worker thread.
// Downloads + extracts a build and makes it active. Runs synchronously
// (network + disk), call from a worker thread. Empty hash = the live version.
void DownloadRobloxBuild(std::string versionHash);
void ForceLiveBuild();     // download (if needed) + activate the current live version
void DownloadPreviousBuild(); // same, for the version Roblox shipped before this one
void SetActiveBuild(const std::string& versionHash); // "" = back to the system install
void DeleteBuild(const std::string& versionHash);

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
