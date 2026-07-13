# Vels Multi Tool - quick installer
# Downloads the prebuilt VelsMultiTool.exe and its handle64.exe runtime helper,
# then launches the tool. No build tools required.
#
# One-line install (run in PowerShell):
#   irm https://raw.githubusercontent.com/x4qo/vels-roblox-multi-tool/main/install.ps1 | iex

$ErrorActionPreference = 'Stop'

$repo    = 'x4qo/vels-roblox-multi-tool'
$branch  = 'main'
$base    = "https://raw.githubusercontent.com/$repo/$branch"
$dest    = Join-Path (Join-Path $HOME 'Downloads') 'VelsMultiTool'
# Relative paths under the repo -> placed at the same relative path under $dest.
# The fonts are loaded from <exe>\fonts\ at runtime, so they must ship alongside.
$files   = @(
    'VelsMultiTool.exe',
    'handle64.exe',
    'fonts/Inter-Variable.ttf',
    'fonts/Lucide.ttf'
)

# "Launch Browser" in the tool drives a real, separate Chrome window via the
# DevTools Protocol to sign it into an account's cookie - it needs Chrome
# installed at one of these standard locations (same paths login.cpp checks).
function Test-ChromeInstalled {
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA) | Where-Object { $_ }
    foreach ($root in $roots) {
        if (Test-Path (Join-Path $root 'Google\Chrome\Application\chrome.exe')) { return $true }
    }
    return $false
}

if (-not (Test-ChromeInstalled)) {
    Write-Host 'Google Chrome not found - installing...' -ForegroundColor Cyan
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if ($winget) {
        winget install --id Google.Chrome -e --accept-package-agreements --accept-source-agreements --silent
    } else {
        $chromeSetup = Join-Path $env:TEMP 'ChromeSetup.exe'
        Invoke-WebRequest -Uri 'https://dl.google.com/chrome/install/latest/chrome_installer.exe' -OutFile $chromeSetup -UseBasicParsing
        Start-Process -FilePath $chromeSetup -ArgumentList '/silent /install' -Wait
        Remove-Item $chromeSetup -Force -ErrorAction SilentlyContinue
    }
    if (Test-ChromeInstalled) {
        Write-Host 'Chrome installed.' -ForegroundColor Green
    } else {
        Write-Host "Chrome install didn't seem to finish - Launch Browser needs Chrome to work." -ForegroundColor Yellow
    }
} else {
    Write-Host 'Google Chrome found.' -ForegroundColor DarkGray
}

Write-Host 'Installing Vels Multi Tool...' -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $dest | Out-Null

foreach ($f in $files) {
    $url = "$base/$f"
    $out = Join-Path $dest ($f -replace '/', '\')
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $out) | Out-Null
    Write-Host "  downloading $f" -ForegroundColor DarkGray
    Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
}

$exe = Join-Path $dest 'VelsMultiTool.exe'
Write-Host "Installed to $dest" -ForegroundColor Green
Write-Host 'Launching...' -ForegroundColor Cyan
Start-Process -FilePath $exe -WorkingDirectory $dest
