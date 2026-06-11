$ErrorActionPreference = "Stop"

$packageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$payloadCandidates = @(
    (Join-Path $packageRoot "obs-universal-media-player"),
    (Join-Path $packageRoot "package\obs-universal-media-player"),
    (Join-Path $packageRoot "release\obs-universal-media-player")
)
$source = $payloadCandidates |
    Where-Object {
        Test-Path -LiteralPath (Join-Path $_ "bin\64bit\obs-universal-media-player.dll")
    } |
    Select-Object -First 1
$destination = Join-Path $env:ProgramData "obs-studio\plugins\obs-universal-media-player"

if (-not $source) {
    $searched = $payloadCandidates -join [Environment]::NewLine
    throw "Plugin payload was not found. Searched:$([Environment]::NewLine)$searched"
}

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw "OBS is running. Close OBS normally, then run this installer again."
}

New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -Path (Join-Path $source "*") -Destination $destination -Recurse -Force

Write-Host "Universal Media Player installed from $source"
Write-Host "Start OBS and use Sources > Add > Universal Media Player."
