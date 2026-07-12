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
$dest    = Join-Path $env:LOCALAPPDATA 'VelsMultiTool'
# Relative paths under the repo -> placed at the same relative path under $dest.
# The fonts are loaded from <exe>\fonts\ at runtime, so they must ship alongside.
$files   = @(
    'VelsMultiTool.exe',
    'handle64.exe',
    'fonts/Inter-Variable.ttf',
    'fonts/Lucide.ttf'
)

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
