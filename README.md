# Vels Multi Tool

Vels Multi Tool is a Windows desktop utility for Roblox workflows. It uses a native C++ Dear ImGui interface with a Win32 + DirectX 11 renderer and keeps the automation/backend code separated from the UI.

## Features

- Multi-instance Roblox launcher with singleton-handle cleanup support
- Roblox process watcher and live instance count
- Roblox cookie file cleaner and locker for `RobloxCookies.dat`
- Browser cookie scanner/cleaner for Roblox cookies
- MAC address spoofer with adapter discovery and restore support
- Encrypted local Roblox account manager using Windows DPAPI
- Account stats, avatar previews, aliases, optional stored passwords, and quick launch into a saved Place ID
- Place metadata preview, including name, creator, visits, favorites, and icon
- Administrator elevation flow for features that need elevated Windows permissions

## Screens

The app is organized into four main pages:

- **Multi-Instance** - launch Roblox, monitor running clients, and jump to common tools.
- **Cookie Cleaner** - scan and clear Roblox/browser cookie traces.
- **MAC Spoofer** - view adapters, spoof the selected MAC address, or restore it.
- **Accounts** - add Roblox accounts, save a Place ID, and launch selected accounts into that place.

## Quick Install

Run this in PowerShell to download the prebuilt tool and launch it (no build tools needed):

```powershell
irm https://raw.githubusercontent.com/x4qo/vels-roblox-multi-tool/main/install.ps1 | iex
```

This fetches `VelsMultiTool.exe` and `handle64.exe` into `%LOCALAPPDATA%\VelsMultiTool` and starts the app. To build from source instead, see [Build](#build) below.

## Requirements

- Windows
- Google Chrome, required for the add-account login flow
- Roblox Player
- Administrator rights for MAC spoofing and singleton-handle cleanup
- MinGW-w64 with `g++` and `windres` on `PATH`
- `handle64.exe` next to `VelsMultiTool.exe` for watcher-based singleton handle cleanup

The build script suggests installing MinGW-w64 with:

```bat
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

Restart your terminal after installing so `g++` and `windres` are available.

## Build

From the project root:

```bat
build.bat
```

If the build succeeds, it creates:

```text
VelsMultiTool.exe
```

The script compiles the app icon resource, then builds the executable from the C++ source, bundled Dear ImGui files, and Win32/DirectX dependencies.

## Run

Place these files in the same folder:

```text
VelsMultiTool.exe
handle64.exe
```

Then run `VelsMultiTool.exe`. The app will request administrator access when needed.

## Project Structure

```text
assets/              App icon and static assets
fonts/               UI fonts
src/main.cpp         Dear ImGui UI and Win32/DirectX app shell
src/backend.cpp      Roblox, cookie, adapter, account, and launch logic
src/backend.h        Shared backend data structures and API
src/login.cpp        Chrome-based Roblox login helper
src/login.h          Login helper API
src/imgui/           Bundled Dear ImGui source and backends
build.bat            MinGW-w64 build script
handle64.exe         Sysinternals Handle helper used at runtime
```

## Runtime Data

The app generates local runtime files next to the executable:

```text
accounts.dat         DPAPI-encrypted account store
placeid.dat          Saved Roblox Place ID
chrome_login_data/   Temporary Chrome login profiles
webview2_data/       Runtime browser data, if created
```

These files are ignored by `.gitignore` and should not be committed.

## Account and Cookie Notes

Saved Roblox accounts are stored in `accounts.dat` using Windows DPAPI, so the data is encrypted for the current Windows user/machine. The app still handles `.ROBLOSECURITY` cookies, which are sensitive session credentials. Do not share `accounts.dat`, logs, screenshots, or copied cookies.

Use this tool only with accounts you own or have explicit permission to manage.

## Cleaning a Repository Before Publishing

Generated runtime data and intermediate build outputs (`src\app_icon.res`, `chrome_login_data/`, `webview2_data/`, `accounts.dat`, `placeid.dat`) are covered by `.gitignore` and should not be committed. The prebuilt `VelsMultiTool.exe` is committed on purpose so the quick installer can fetch it.

Keep `handle64.exe` only if you are allowed to redistribute it. If not, remove it from the repo and tell users to download it separately from Microsoft Sysinternals.
