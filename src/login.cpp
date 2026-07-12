#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "login.h"
#include "backend.h"
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace login {

// ---------------------------------------------------------------------------
// Tiny ad-hoc JSON field readers. The Chrome DevTools Protocol responses we
// parse here have a small, predictable shape, so a couple of substring
// searches are enough - no need to pull in a JSON library for this.
// ---------------------------------------------------------------------------
static std::string ExtractJsonString(const std::string& json, const std::string& key, size_t from = 0) {
    std::string pat = "\"" + key + "\":";
    size_t pos = json.find(pat, from);
    if (pos == std::string::npos) return "";
    pos += pat.size();
    if (pos < json.size() && json[pos] == ' ') pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// Finds the page target's websocket debugger path out of a /json/list body,
// e.g. "/devtools/page/XXXX" from "ws://127.0.0.1:54321/devtools/page/XXXX".
static std::wstring ExtractPageWebSocketPath(const std::string& json) {
    size_t typePos = json.find("\"type\":\"page\"");
    if (typePos == std::string::npos) typePos = json.find("\"type\": \"page\"");
    if (typePos == std::string::npos) return L"";
    std::string wsUrl = ExtractJsonString(json, "webSocketDebuggerUrl", typePos);
    size_t pathStart = wsUrl.find("/devtools/");
    if (pathStart == std::string::npos) return L"";
    std::string path = wsUrl.substr(pathStart);
    return std::wstring(path.begin(), path.end());
}

static std::string ExtractCookieValue(const std::string& json, const std::string& cookieName) {
    std::string namePat = "\"name\":\"" + cookieName + "\"";
    size_t pos = json.find(namePat);
    if (pos == std::string::npos) return "";
    return ExtractJsonString(json, "value", pos);
}

// A real .ROBLOSECURITY session cookie always carries this warning banner and
// runs several hundred characters - short enough to rule out the placeholder
// values Roblox sometimes sets before sign-in completes.
static bool LooksLikeRealSecurityCookie(const std::string& value) {
    return value.size() > 100 && value.find("WARNING:-DO-NOT-SHARE-THIS") != std::string::npos;
}

static std::wstring FindChromeExe() {
    const wchar_t* envVars[] = { L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA" };
    for (auto* envVar : envVars) {
        wchar_t buf[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(envVar, buf, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) continue;
        std::wstring candidate = std::wstring(buf) + L"\\Google\\Chrome\\Application\\chrome.exe";
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return L"";
}

static std::wstring MakeLoginProfileDir(const std::wstring& exeDir) {
    auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return exeDir + L"\\chrome_login_data\\login_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(ticks);
}

static void RemoveProfileDirBestEffort(const std::wstring& profileDir) {
    for (int i = 0; i < 20; ++i) {
        std::error_code ec;
        std::filesystem::remove_all(profileDir, ec);
        if (!std::filesystem::exists(profileDir)) return;
        Sleep(100);
    }
}

static int PickFreeLocalPort() {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    int port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
        int len = sizeof(addr);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != SOCKET_ERROR) {
            port = ntohs(addr.sin_port);
        }
    }

    closesocket(s);
    WSACleanup();
    return port;
}

// ---------------------------------------------------------------------------
// Minimal WinHTTP helpers: a plain localhost GET (for /json/list) and a
// blocking WebSocket send/receive pair (for the devtools protocol itself).
// ---------------------------------------------------------------------------
static std::string HttpGetLocal(int port, const std::wstring& path) {
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"VelsMultiTool/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;

    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", (INTERNET_PORT)port, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (hRequest) {
            if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hRequest, nullptr)) {
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
                    std::vector<char> chunk(avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) break;
                    result.append(chunk.data(), read);
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

static HINTERNET OpenLocalWebSocket(int port, const std::wstring& path, HINTERNET& outSession, HINTERNET& outConnect) {
    outSession = WinHttpOpen(L"VelsMultiTool/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!outSession) return nullptr;

    outConnect = WinHttpConnect(outSession, L"127.0.0.1", (INTERNET_PORT)port, 0);
    if (!outConnect) return nullptr;

    HINTERNET hRequest = WinHttpOpenRequest(outConnect, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) return nullptr;

    if (!WinHttpSetOption(hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        WinHttpCloseHandle(hRequest);
        return nullptr;
    }
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        return nullptr;
    }

    HINTERNET hWebSocket = WinHttpWebSocketCompleteUpgrade(hRequest, 0);
    WinHttpCloseHandle(hRequest);
    return hWebSocket;
}

static bool WsSendText(HINTERNET ws, const std::string& text) {
    return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (PVOID)text.data(), (DWORD)text.size()) == ERROR_SUCCESS;
}

static bool WsReceiveText(HINTERNET ws, std::string& out) {
    out.clear();
    char buf[8192];
    for (;;) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType;
        DWORD err = WinHttpWebSocketReceive(ws, buf, sizeof(buf), &bytesRead, &bufType);
        if (err != ERROR_SUCCESS) return false;
        out.append(buf, bytesRead);
        if (bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            return true;
        }
        // else a _FRAGMENT type - the message continues, keep reading.
    }
}

void ShowRobloxLoginWindow(const std::wstring& exeDir,
    std::function<void(bool success, std::string cookie)> onComplete) {

    std::wstring chromePath = FindChromeExe();
    if (chromePath.empty()) {
        backend::Log("[!] Google Chrome wasn't found on this PC.");
        onComplete(false, "");
        return;
    }

    // Use a fresh throwaway profile for every login so a stale browser lock or
    // old Roblox session can never be reused by the next Add Account attempt.
    std::wstring profileRoot = exeDir + L"\\chrome_login_data";
    std::wstring profileDir = MakeLoginProfileDir(exeDir);
    int port = PickFreeLocalPort();
    if (port == 0) {
        backend::Log("[!] Could not reserve a local Chrome debugging port.");
        onComplete(false, "");
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(profileRoot, ec);
    ec.clear();
    std::filesystem::create_directories(profileDir, ec);
    if (ec) {
        backend::Log("[!] Failed to create a temporary Chrome profile.");
        onComplete(false, "");
        return;
    }

    std::wstring cmdLine = L"\"" + chromePath + L"\" --remote-debugging-port=" + std::to_wstring(port) +
        L" --user-data-dir=\"" + profileDir +
        L"\" --no-first-run --no-default-browser-check --new-window \"https://www.roblox.com/login\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(0);

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        RemoveProfileDirBestEffort(profileDir);
        backend::Log("[!] Failed to launch Chrome.");
        onComplete(false, "");
        return;
    }
    CloseHandle(pi.hThread);

    auto cleanupChrome = [&]() {
        if (pi.hProcess) {
            CloseHandle(pi.hProcess);
            pi.hProcess = nullptr;
        }
        RemoveProfileDirBestEffort(profileDir);
    };

    backend::Log("[i] Opened Chrome sign-in window.");

    std::wstring wsPath;
    for (int i = 0; i < 100; ++i) {
        wsPath = ExtractPageWebSocketPath(HttpGetLocal(port, L"/json/list"));
        if (!wsPath.empty()) break;
        Sleep(100);
    }

    if (wsPath.empty()) {
        cleanupChrome();
        backend::Log("[!] Chrome did not expose its login automation port in time.");
        onComplete(false, "");
        return;
    }

    HINTERNET hSession = nullptr, hConnect = nullptr;
    HINTERNET hWebSocket = OpenLocalWebSocket(port, wsPath, hSession, hConnect);
    if (!hWebSocket) {
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);
        cleanupChrome();
        onComplete(false, "");
        return;
    }

    bool success = false;
    std::string foundCookie;
    int msgId = 1;
    auto start = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - start < std::chrono::minutes(5)) {
        std::string req = "{\"id\":" + std::to_string(msgId++) + ",\"method\":\"Network.getAllCookies\"}";
        std::string resp;
        if (!WsSendText(hWebSocket, req) || !WsReceiveText(hWebSocket, resp)) break;

        std::string val = ExtractCookieValue(resp, ".ROBLOSECURITY");
        if (LooksLikeRealSecurityCookie(val)) {
            success = true;
            foundCookie = val;
            break;
        }

        Sleep(1500);
    }

    if (success) {
        std::string closeReq = "{\"id\":" + std::to_string(msgId++) + ",\"method\":\"Browser.close\"}";
        WsSendText(hWebSocket, closeReq);
        Sleep(750);
    }

    WinHttpCloseHandle(hWebSocket);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    cleanupChrome();

    onComplete(success, success ? foundCookie : "");
}

// Minimal JSON string escaper for the cookie value we inject via CDP. A
// .ROBLOSECURITY value is base64-ish (no quotes/backslashes in practice), but we
// escape defensively so a stray character can't break the JSON command.
static std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c >= 0x20) o += c; // drop other control chars
                break;
        }
    }
    return o;
}

void OpenAccountWebSession(const std::wstring& exeDir, const std::string& cookie,
    long long userId, const std::string& username) {

    if (cookie.empty()) {
        backend::Log("[!] This account has no saved cookie to open the web with.");
        return;
    }

    std::wstring chromePath = FindChromeExe();
    if (chromePath.empty()) {
        backend::Log("[!] Google Chrome wasn't found on this PC.");
        return;
    }

    // Persistent, per-account profile so each account gets its own isolated
    // browser instance and stays logged in between opens.
    std::wstring profileDir = exeDir + L"\\web_profiles\\acct_" +
        (userId > 0 ? std::to_wstring(userId) : L"default");
    int port = PickFreeLocalPort();
    if (port == 0) {
        backend::Log("[!] Could not reserve a local Chrome debugging port.");
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(profileDir, ec);

    std::wstring cmdLine = L"\"" + chromePath + L"\" --remote-debugging-port=" + std::to_wstring(port) +
        L" --user-data-dir=\"" + profileDir +
        L"\" --no-first-run --no-default-browser-check --new-window \"about:blank\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(0);

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        backend::Log("[!] Failed to launch Chrome.");
        return;
    }
    CloseHandle(pi.hThread);
    // Intentionally leave the browser running and the profile on disk; just drop
    // our own handle to the process.
    if (pi.hProcess) CloseHandle(pi.hProcess);

    backend::Log("[i] Opened a browser window for " + username + ".");

    std::wstring wsPath;
    for (int i = 0; i < 100; ++i) {
        wsPath = ExtractPageWebSocketPath(HttpGetLocal(port, L"/json/list"));
        if (!wsPath.empty()) break;
        Sleep(100);
    }
    if (wsPath.empty()) {
        // Most likely this account's profile was already open in another window;
        // the browser is up, we just couldn't drive it to (re)inject the cookie.
        backend::Log("[!] Browser is open, but its automation port wasn't reachable to set the cookie.");
        return;
    }

    HINTERNET hSession = nullptr, hConnect = nullptr;
    HINTERNET hWebSocket = OpenLocalWebSocket(port, wsPath, hSession, hConnect);
    if (!hWebSocket) {
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);
        backend::Log("[!] Could not connect to Chrome to set the session cookie.");
        return;
    }

    int msgId = 1;
    auto sendCmd = [&](const std::string& method, const std::string& params) -> bool {
        int id = msgId++;
        std::string req = "{\"id\":" + std::to_string(id) + ",\"method\":\"" + method + "\"";
        if (!params.empty()) req += ",\"params\":" + params;
        req += "}";
        if (!WsSendText(hWebSocket, req)) return false;
        std::string idPat = "\"id\":" + std::to_string(id);
        for (int i = 0; i < 60; ++i) { // skip interleaved CDP events until our reply
            std::string resp;
            if (!WsReceiveText(hWebSocket, resp)) return false;
            if (resp.find(idPat) != std::string::npos) return true;
        }
        return false;
    };

    sendCmd("Network.enable", "");
    std::string cookieParams =
        std::string("{\"name\":\".ROBLOSECURITY\",\"value\":\"") + JsonEscape(cookie) +
        "\",\"domain\":\".roblox.com\",\"path\":\"/\",\"secure\":true,\"httpOnly\":true}";
    bool cookieOk = sendCmd("Network.setCookie", cookieParams);
    sendCmd("Page.navigate", "{\"url\":\"https://www.roblox.com/home\"}");

    WinHttpCloseHandle(hWebSocket);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (cookieOk) backend::Log("[v] Signed the browser into " + username + ".");
    else backend::Log("[!] Opened the browser but couldn't confirm the session cookie for " + username + ".");
}

} // namespace login
