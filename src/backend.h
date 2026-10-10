#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <map>

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
    std::string group;
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

extern std::mutex launchedMutex;
extern std::map<long long, unsigned long> launchedPids;
void PruneLaunchedPids();

void LoadAccounts();
void SaveAccounts();

bool AddAccountFromCookie(const std::string& cookie, const std::string& password = "");
void RemoveAccount(int index);
void SetAccountPassword(int index, const std::string& password);
void SetAccountAlias(int index, const std::string& alias);

void MoveAccount(int from, int to);
void SetAccountPriority(int index, bool priority);
void SetAccountGroup(int index, const std::string& group);

void LaunchAccountIntoPlace(int index, long long placeId);
void LaunchAccountIntoServer(int index, long long placeId, const std::string& gameId);

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

struct ArrangeSettings { std::string preset = "grid"; int gap = 6; bool autoArrange = false; int monitor = 0; };
extern std::mutex arrangeMutex;
extern ArrangeSettings arrangeSettings;
void LoadArrangeSettings();
void SetArrangeSettings(const ArrangeSettings& s);
std::vector<std::string> MonitorLabels();
int CountRobloxWindows();
int ArrangeRobloxWindows(const std::string& presetOverride);
void StartAutoArrangeWatcher();

struct CustomFontState { bool enabled = false; bool hasFont = false; std::string name; };
extern std::mutex customFontMutex;
extern CustomFontState customFont;
void LoadCustomFont();
bool SetCustomFontFile(const std::wstring& path);
void SetCustomFontEnabled(bool on);
void EnsureCustomFont();

struct UpdateState { std::string status = "idle"; std::string message; float progress = 0.0f; bool prompt = false; };
extern std::mutex updateMutex;
extern UpdateState updateState;
extern std::atomic<bool> updateNotify;
extern std::atomic<bool> updateAuto;
void CheckForUpdate();
bool InstallUpdate();
void StartupUpdateCheck();
void SetUpdateSettings(bool notify, bool autoUpdate);

struct GameLookup { long long universeId = 0; long long rootPlaceId = 0; std::string name; std::string iconUrl; };
bool LookupGame(long long placeId, GameLookup& out);
bool QueryPresence(long long userId, const std::string& cookie, long long& placeId, long long& rootPlaceId, std::string& gameId);

void PrepareRobloxInstalls();
void OnAccountLaunched(long long userId, long long placeId, const std::string& gameId, const std::string& linkCode);

extern std::mutex fpsMutex;
extern std::map<long long, int> fpsCaps;
void LoadFpsCaps();
void SetFpsCap(long long userId, int cap);
void ApplyFpsCapFor(long long userId);

struct ClientMod { std::string id; std::string label; bool active = false; std::string fileName; };
std::vector<ClientMod> ClientMods();
bool SetClientModFile(const std::string& id, const std::wstring& path);
void ClearClientMod(const std::string& id);

extern std::mutex fastFlagsMutex;
extern std::map<std::string, std::string> fastFlags;
void LoadFastFlags();
bool SetFastFlag(const std::string& name, const std::string& value);
void ResetFastFlags();
std::string ExportFastFlagsJson();
int ImportFastFlagsJson(const std::string& text, int& skipped);

struct ServerVisit {
    long long time = 0; long long placeId = 0; long long rootPlaceId = 0;
    std::string gameId; long long userId = 0; std::string linkCode; std::string name;
};
extern std::mutex historyMutex;
extern std::vector<ServerVisit> serverHistory;
void LoadServerHistory();
void ClearServerHistory();
void RemoveServerVisit(const std::string& gameId);

struct ServerRow { std::string id; int playing = 0; int maxPlayers = 0; int ping = 0; int fps = 0; std::string region; bool regionTried = false; };
struct ServerBrowserState { long long placeId = 0; bool loading = false; bool smallestFirst = false; std::string error; std::vector<ServerRow> rows; };
extern std::mutex serversMutex;
extern ServerBrowserState serverBrowser;
void LoadServerBrowser(long long placeId, bool smallestFirst, long long regionUserId);

struct FriendRow { long long id = 0; std::string name; std::string display; std::string game; std::string gameId; std::string avatarUrl; long long placeId = 0; long long rootPlaceId = 0; };
struct FriendsState { long long userId = 0; bool loading = false; std::string error; int total = 0; int online = 0; std::vector<FriendRow> rows; };
extern std::mutex friendsMutex;
extern FriendsState friendsState;
void LoadFriends(long long userId);

struct PlaytimeGame { long long universeId = 0; long long rootPlaceId = 0; std::string name; std::string iconUrl; };
struct PlaytimeEntry { long long userId = 0; long long universeId = 0; long long seconds = 0; long long lastPlayed = 0; };
extern std::mutex playtimeMutex;
extern std::map<long long, PlaytimeGame> playtimeGames;
extern std::vector<PlaytimeEntry> playtime;
extern std::map<long long, long long> playSessions;
void LoadPlaytime();
void BeginPlaySession(long long userId, const GameLookup& game);
void ClearPlaytime();

extern std::atomic<bool> discordEnabled;
extern std::atomic<bool> discordConnected;
void LoadDiscordSettings();
void SetDiscordEnabled(bool on);
extern std::atomic<int> discordTimeMode;
extern std::atomic<int> discordTimeOffset;
void SetDiscordTime(int mode, int offsetSeconds);
void SetDiscordGame(long long placeId);

}
