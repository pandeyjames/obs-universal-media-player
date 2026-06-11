[CmdletBinding(DefaultParameterSetName = "List")]
param(
    [Parameter(Mandatory, ParameterSetName = "Remove")]
    [string]$Name,

    [Parameter(ParameterSetName = "List")]
    [switch]$List,

    [string]$Collection,

    [switch]$IncludeScenes
)

$ErrorActionPreference = "Stop"

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw "OBS is running. Close OBS normally before managing global sources."
}

$sceneDirectory = Join-Path $env:APPDATA "obs-studio\basic\scenes"
if (-not (Test-Path -LiteralPath $sceneDirectory)) {
    throw "OBS scene collection directory was not found: $sceneDirectory"
}

$collections = @(Get-ChildItem -LiteralPath $sceneDirectory -Filter "*.json" -File)
if ($Collection) {
    $collectionName = [System.IO.Path]::GetFileNameWithoutExtension($Collection)
    $collectionFile = $collections |
        Where-Object { $_.BaseName -eq $collectionName } |
        Select-Object -First 1
    if (-not $collectionFile) {
        throw "Scene collection '$Collection' was not found."
    }
} elseif ($collections.Count -eq 1) {
    $collectionFile = $collections[0]
} else {
    $names = ($collections.BaseName | Sort-Object) -join ", "
    throw "Multiple scene collections exist. Use -Collection with one of: $names"
}

$document = Get-Content -LiteralPath $collectionFile.FullName -Raw |
    ConvertFrom-Json -Depth 100
$sources = @($document.sources)

if ($PSCmdlet.ParameterSetName -eq "List") {
    $sources |
        Sort-Object name |
        Select-Object name, id, uuid |
        Format-Table -AutoSize
    return
}

$targets = @($sources | Where-Object { $_.name -ceq $Name })
if ($targets.Count -eq 0) {
    throw "No global source named '$Name' exists in collection '$($collectionFile.BaseName)'."
}

$protectedTargets = @($targets | Where-Object { $_.id -in @("scene", "group") })
if ($protectedTargets.Count -gt 0 -and -not $IncludeScenes) {
    throw "'$Name' is a scene or group. Re-run with -IncludeScenes only if deleting it is intentional."
}

$targetUuids = @($targets.uuid | Where-Object { $_ })
$removedReferences = 0

foreach ($container in $sources) {
    if (-not $container.settings -or -not $container.settings.items) {
        continue
    }

    $items = @($container.settings.items)
    $remainingItems = @(
        $items | Where-Object {
            $matchesName = $_.name -ceq $Name
            $matchesUuid = $_.source_uuid -and ($targetUuids -contains $_.source_uuid)
            $remove = $matchesName -or $matchesUuid
            if ($remove) {
                $script:removedReferences++
            }
            -not $remove
        }
    )
    $container.settings.items = $remainingItems
}

$document.sources = @(
    $sources | Where-Object { $_.name -cne $Name }
)

if ($document.groups) {
    $document.groups = @(
        $document.groups | Where-Object {
            if ($_ -is [string]) {
                $_ -cne $Name
            } else {
                $_.name -cne $Name
            }
        }
    )
}

if ($IncludeScenes) {
    $document.scene_order = @(
        $document.scene_order | Where-Object { $_.name -cne $Name }
    )
}

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backupPath = "$($collectionFile.FullName).backup-$timestamp"
Copy-Item -LiteralPath $collectionFile.FullName -Destination $backupPath

$json = $document | ConvertTo-Json -Depth 100
[System.IO.File]::WriteAllText(
    $collectionFile.FullName,
    $json,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host "Removed $($targets.Count) global source(s) named '$Name'."
Write-Host "Removed $removedReferences scene reference(s)."
Write-Host "Backup: $backupPath"
