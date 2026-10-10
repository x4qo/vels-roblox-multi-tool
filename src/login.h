#pragma once

#include <string>
#include <functional>

namespace login {

bool ChromeInstalled();

void ShowRobloxLoginWindow(const std::wstring& exeDir,
    std::function<void(bool success, std::string cookie)> onComplete);

void OpenAccountWebSession(const std::wstring& exeDir, const std::string& cookie,
    long long userId, const std::string& username);

}
