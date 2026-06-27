// Roblox login via a real, separate Chrome window (its own throwaway profile,
// driven through the Chrome DevTools Protocol) instead of an embedded
// browser control. Runs synchronously - call from a worker thread.
#pragma once

#include <string>
#include <functional>

namespace login {

// Launches Chrome pointed at the real Roblox login page, waits for the user
// to sign in, then hands back the resulting .ROBLOSECURITY cookie.
// onComplete(success, cookieValue) fires on the calling (worker) thread.
// If the user closes the Chrome window without completing login, success is
// false. Chrome's profile directory is wiped before every call, so Roblox
// never has a saved/remembered session there - it's a fresh login every time.
void ShowRobloxLoginWindow(const std::wstring& exeDir,
    std::function<void(bool success, std::string cookie)> onComplete);

} // namespace login
