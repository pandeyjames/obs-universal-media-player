[CmdletBinding()]
param(
    [string] $Destination = (Join-Path (Split-Path -Parent $PSScriptRoot) "data\bin")
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$ytDlpVersion = "2026.06.09"
$ytDlpSha256 = "3A48CB955D55C8821B60CCBDBBC6F61BC958F2F3D3B7AD5EAF3D83A543293A27"
$streamlinkBuild = "8.4.0-1"
$streamlinkArchive = "streamlink-8.4.0-1-py314-x86_64.zip"
$streamlinkSha256 = "A8D3BD2B409E6D1B1F7A0E2A5C0CBFBA619775E475DA3F31285AF08D680FB71C"

function Get-VerifiedFile {
    param(
        [Parameter(Mandatory)]
        [string] $Uri,
        [Parameter(Mandatory)]
        [string] $Path,
        [Parameter(Mandatory)]
        [string] $Sha256
    )

    Invoke-WebRequest -Uri $Uri -OutFile $Path -UseBasicParsing
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actual -ne $Sha256) {
        Remove-Item -LiteralPath $Path -Force
        throw "SHA-256 mismatch for $Uri. Expected $Sha256, received $actual."
    }
}

$destinationPath = [System.IO.Path]::GetFullPath($Destination)
$projectRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $destinationPath.StartsWith($projectRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Dependency destination must remain inside the project directory: $destinationPath"
}

$temporary = Join-Path ([System.IO.Path]::GetTempPath()) "obs-universal-media-player-dependencies"
if (Test-Path -LiteralPath $temporary) {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}

New-Item -ItemType Directory -Path $temporary, $destinationPath -Force | Out-Null

try {
    $ytDlpPath = Join-Path $destinationPath "yt-dlp.exe"
    Get-VerifiedFile `
        -Uri "https://github.com/yt-dlp/yt-dlp/releases/download/$ytDlpVersion/yt-dlp.exe" `
        -Path $ytDlpPath `
        -Sha256 $ytDlpSha256

    $streamlinkZip = Join-Path $temporary $streamlinkArchive
    Get-VerifiedFile `
        -Uri "https://github.com/streamlink/windows-builds/releases/download/$streamlinkBuild/$streamlinkArchive" `
        -Path $streamlinkZip `
        -Sha256 $streamlinkSha256

    $expanded = Join-Path $temporary "streamlink"
    Expand-Archive -LiteralPath $streamlinkZip -DestinationPath $expanded
    $portableRoot = Join-Path $expanded "streamlink-8.4.0-1-py314-x86_64"
    $streamlinkDestination = Join-Path $destinationPath "streamlink"
    if (Test-Path -LiteralPath $streamlinkDestination) {
        Remove-Item -LiteralPath $streamlinkDestination -Recurse -Force
    }
    Move-Item -LiteralPath $portableRoot -Destination $streamlinkDestination

    Write-Host "Fetched yt-dlp $ytDlpVersion and Streamlink $streamlinkBuild."
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}
