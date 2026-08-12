# Vels Multi Tool

Vels Multi Tool is a Windows desktop utility for Roblox workflows. It uses a native C++ Dear ImGui interface with a Win32 + DirectX 11 renderer and keeps the automation/backend code separated from the UI.

## Features

**Multiple clients.** Toggling Multi Instance holds the singleton mutex and closes any `ROBLOX_singletonEvent` handles, which is what lets a second client start at all. Running instances are counted live.

**Accounts.** Add them by logging in through Chrome, pasting cookies, or loading a cookie file. The store is encrypted with Windows DPAPI, so `accounts.dat` only decrypts on the machine that wrote it. Each account keeps its avatar, stats, an optional alias and password, and a menu to copy its cookie, `user:pass`, username or password.

**Launching.** Select any number of accounts and send them all into a Place ID. Paste a private server link (`roblox.com/share?code=...&type=Server`, or a `?privateServerLinkCode=` URL) and it resolves the code through Roblox's share-link API, then launches every selected account into that server. Places and private servers can both be saved as named presets.

**Downgrading.** Live, previous and upcoming Windows versions come from the WEAO API. Picking one downloads the packages from Roblox's deployment CDN the same way rdd.weao.gg does, unpacks them into `Builds\<version-hash>` next to the exe, and writes the `AppSettings.xml` the client needs. A switch decides whether launches use that build or your normal install, so deleting the folder is all it takes to undo.

**Cleanup.** Clears `RobloxCookies.dat` and can lock it, and scans browsers for leftover Roblox cookies. MAC spoofing lists adapters and restores the original address.

Admin rights are requested when a feature needs them, not at startup.

## Layout

Everything sits on one screen: adding accounts and the version manager on the left, place and private server settings on the right, and the account table underneath. Multi Instance is a toggle in the header rather than a separate page.

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
places.dat           Named place presets
privateserver.dat    Active private server
privateservers.dat   Named private server presets
activebuild.dat      Which downloaded build to launch, if any
Builds/              Downloaded Roblox clients, one folder per version
chrome_login_data/   Temporary Chrome login profiles
webview2_data/       Runtime browser data, if created
```

These files are ignored by `.gitignore` and should not be committed.

## Account and Cookie Notes

Saved Roblox accounts are stored in `accounts.dat` using Windows DPAPI, so the data is encrypted for the current Windows user/machine. The app still handles `.ROBLOSECURITY` cookies, which are sensitive session credentials. Do not share `accounts.dat`, logs, screenshots, or copied cookies.

Use this tool only with accounts you own or have explicit permission to manage.

## Cleaning a Repository Before Publishing

Runtime data and build leftovers (`src\app_icon.res`, `chrome_login_data/`, `webview2_data/`, `accounts.dat`, the `.dat` presets, and `Builds/`) are already in `.gitignore` and should stay out of commits. `Builds/` in particular is hundreds of MB of downloaded Roblox clients. The prebuilt `VelsMultiTool.exe` is committed on purpose so the quick installer can fetch it.

Keep `handle64.exe` only if you are allowed to redistribute it. If not, remove it from the repo and tell users to download it separately from Microsoft Sysinternals.
