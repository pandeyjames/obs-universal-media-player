$ErrorActionPreference = "Stop"

$destination = Join-Path $env:ProgramData "obs-studio\plugins\obs-universal-media-player"
if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw "OBS is running. Close OBS normally, then run this uninstaller again."
}

if (Test-Path -LiteralPath $destination) {
    Remove-Item -LiteralPath $destination -Recurse -Force
}

Write-Host "Universal Media Player uninstalled."
