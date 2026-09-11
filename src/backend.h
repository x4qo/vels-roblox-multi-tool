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
    bool isActive = false;
};

struct RobloxAccount {
    std::string username;
    long long userId = 0;
    std::string cookie;
    std::string password;
    std::string alias;
    bool priority = false;

    long long friendsCount = -1;
    long long followersCount = -1;
    long long followingCount = -1;
    long long robuxBalance = -1;
    std::string joinDate;
    std::string accountAge;
    bool statsLoaded = false;
    bool statsRequested = false;

    std::vector<unsigned char> avatarPng;
    bool avatarLoaded = false;
    bool avatarRequested = false;
};

struct PlaceInfo {
    long long placeId = 0;
    long long rootPlaceId = 0;
    std::string name;
    std::string placeName;
    bool isSubPlace = false;
    std::string creator;
    long long visits = -1;
    long long favorites = -1;
    long long playing = -1;
    long long maxPlayers = -1;
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
    std::string time;
    std::string text;
};

struct BrowserCookieStatus {
    std::string name;
    bool installed = false;
    bool found = false;
    int count = 0;
    bool scanFailed = false;
};

namespace backend {

extern std::mutex logMutex;
extern std::vector<LogEntry> logLines;
extern long long logTotal;
void ClearLog();

extern std::atomic<bool> watching;
extern std::atomic<int> instanceCount;

extern std::mutex adaptersMutex;
extern std::vector<NetworkAdapterInfo> adapters;
extern std::atomic<int> defaultAdapterIndex;

void Log(const std::string& msg);

bool IsElevated();
bool RelaunchAsAdmin();

void Init(const std::wstring& exeDir);
void Shutdown();

void StartWatching();
void StopWatching();
void LaunchNewInstance();
void KillAllRobloxInstances();

void CloseRobloxSingletonsNow();

extern std::atomic<bool> uiForeground;

int CountRobloxProcesses(bool force = false);
float GetCpuUsagePercent();
float GetMemoryUsagePercent();
std::string GetUptimeString();

void ClearRobloxCookieFile();
bool RobloxCookieFileHasData();
void ClearBrowserCookies();
void ClearRobloxCookieFiles();

extern std::mutex browserCookieMutex;
extern std::vector<BrowserCookieStatus> browserCookieStatus;
extern std::atomic<bool> browserCookieScanning;
extern std::atomic<bool> browserCookieScanned;
void ScanBrowserCookies();

void RefreshAdapters();
void SpoofAdapter(int index);
void RestoreAdapter(int index);

extern std::mutex accountsMutex;
extern std::vector<RobloxAccount> accounts;

void LoadAccounts();
void SaveAccounts();

bool AddAccountFromCookie(const std::string& cookie, const std::string& password = "");
void RemoveAccount(int index);
void SetAccountPassword(int index, const std::string& password);
void SetAccountAlias(int index, const std::string& alias);

void MoveAccount(int from, int to);
void SetAccountPriority(int index, bool priority);

void LaunchAccountIntoPlace(int index, long long placeId);
void LaunchAccountIntoServer(int index, long long placeId, const std::string& gameId);

// Picks the lowest-ping non-full public server from Roblox's own server list
// (games.roblox.com/v1/games/{placeId}/servers/Public) - the same source RoSeal
// reads. Returns the server (job) GUID, or "" if none/failure.
std::string FindBestServer(long long placeId);

extern std::atomic<bool> joinBestServer;
void SetJoinBestServer(bool on);
void LoadJoinBestServer();

struct LastServer { long long placeId = 0; std::string gameId; };
extern std::mutex lastServerMutex;
extern LastServer lastServer;
void LoadLastServer();
void SaveLastServer(long long placeId, const std::string& gameId);

void LaunchRobloxClient();
void LaunchAccountClient(int index);

void LaunchAccountIntoPrivateServer(int index, long long placeId, const std::string& linkCode);

void OpenAccountWeb(int index);

void FetchAccountStats(int index);

void FetchAccountAvatar(int index);

extern std::mutex placeInfoMutex;
extern PlaceInfo placeInfo;
void FetchPlaceInfo(long long placeId, const std::string& cookie);

extern std::atomic<long long> savedPlaceId;
void SavePlaceId(long long placeId);
void LoadPlaceId();

struct SavedPlace { long long id; std::string name; bool favorite = false; };
extern std::mutex savedPlacesMutex;
extern std::vector<SavedPlace> savedPlaces;
void LoadSavedPlaces();
void AddSavedPlace(long long id, const std::string& name);
void RemoveSavedPlace(long long id);
void SetSavedPlaceFavorite(long long id, bool favorite);

struct PrivateServer {
    long long   placeId = 0;
    std::string linkCode;
    std::string name;
};

extern std::mutex activePrivateServerMutex;
extern PrivateServer activePrivateServer;
void LoadActivePrivateServer();
void SetActivePrivateServer(const PrivateServer& ps);
void ClearActivePrivateServer();

extern std::mutex privateServersMutex;
extern std::vector<PrivateServer> savedPrivateServers;
void LoadSavedPrivateServers();
void AddSavedPrivateServer(const PrivateServer& ps);
void RemoveSavedPrivateServer(const std::string& linkCode);

bool ResolvePrivateServerLink(const std::string& input, const std::string& cookie, PrivateServer& out);
extern std::atomic<bool> privateServerResolving;

struct RobloxBuildState {
    std::string liveVersion;
    std::string liveDate;
    std::string futureVersion;
    std::string pastVersion;
    std::string pastDate;
    bool        weaoLoaded = false;
    std::string activeVersion;
    std::string preferredVersion;
    std::string status;
    float       progress = 0.0f;
    bool        busy = false;
};
extern std::mutex robloxBuildMutex;
extern RobloxBuildState robloxBuild;
extern std::vector<std::string> downloadedBuilds;

void LoadRobloxBuilds();
void FetchWeaoVersions();
void DownloadRobloxBuild(std::string versionHash);
void ForceLiveBuild();
void DownloadPreviousBuild();
void SetActiveBuild(const std::string& versionHash);
void DeleteBuild(const std::string& versionHash);

extern std::mutex activityMutex;
extern std::vector<ActivityEntry> activityLog;
void AddActivity(const std::string& title, const std::string& subtitle);
std::string RelativeTimeString(long long unixSeconds);

extern std::mutex systemStatusMutex;
extern SystemStatus systemStatus;
void RefreshSystemStatus(int selectedAccountIndex);

}
