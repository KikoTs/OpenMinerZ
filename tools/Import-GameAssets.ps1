[CmdletBinding()]
param(
    [string]$GameDirectory,
    [ValidateSet('Auto', 'Classic', 'Modern')][string]$Style = 'Auto',
    [string]$ContentPackRoot = (Join-Path $env:LOCALAPPDATA 'OpenMinerZ/ContentPacks'),
    [string]$RuntimeDirectory = (Join-Path $env:LOCALAPPDATA 'OpenMinerZ/Runtime'),
    [string]$Bridge,
    [switch]$ListInstallations,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-ContentHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $hasher.Dispose() }
}

function Find-GameInstallations {
    $steamRoots = @("${env:ProgramFiles(x86)}/Steam")
    foreach ($key in @('HKCU:/Software/Valve/Steam', 'HKLM:/SOFTWARE/WOW6432Node/Valve/Steam')) {
        $entry = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
        if ($entry) {
            foreach ($property in @('SteamPath', 'InstallPath')) {
                if ($entry.PSObject.Properties[$property]) { $steamRoots += $entry.$property }
            }
        }
    }
    $libraries = @($steamRoots)
    foreach ($steam in ($steamRoots | Select-Object -Unique)) {
        $vdf = Join-Path $steam 'steamapps/libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf -PathType Leaf) {
            foreach ($match in [regex]::Matches([IO.File]::ReadAllText($vdf), '"path"\s+"([^"\r\n]+)"')) {
                $libraries += $match.Groups[1].Value.Replace('\\', '\')
            }
        }
    }
    foreach ($library in ($libraries | Select-Object -Unique)) {
        $candidate = Join-Path $library 'steamapps/common/CastleMiner Z'
        if (Test-Path -LiteralPath (Join-Path $candidate 'CastleMinerZ.exe') -PathType Leaf) {
            (Resolve-Path -LiteralPath $candidate).Path
        }
    }
}

function Assert-PrivateDestination([string]$Path, [string]$SourceGame) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $source = $SourceGame.TrimEnd('\', '/')
    if ($full.Equals($source, [StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($source + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Import destinations must be outside the original game installation.'
    }
    $ancestor = $full
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Import destinations cannot traverse links or junctions: $ancestor"
            }
        }
        if (Test-Path -LiteralPath (Join-Path $ancestor '.git')) {
            $privateRoot = Join-Path $ancestor 'out'
            if (-not $full.StartsWith($privateRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Inside a Git checkout, imported assets may only be written below out/.'
            }
            break
        }
        $parent = [IO.Directory]::GetParent($ancestor)
        $ancestor = if ($parent) { $parent.FullName } else { $null }
    }
    return $full
}

$installations = @(Find-GameInstallations | Select-Object -Unique)
if ($ListInstallations) { $installations; return }
if ([string]::IsNullOrWhiteSpace($GameDirectory)) {
    if ($installations.Count -ne 1) {
        throw 'Pass -GameDirectory with your CastleMiner Z installation. Use -ListInstallations to list detected Steam copies.'
    }
    $GameDirectory = $installations[0]
}
$game = (Resolve-Path -LiteralPath $GameDirectory).Path.TrimEnd('\', '/')
$content = Join-Path $game 'Content'
if (-not (Test-Path -LiteralPath (Join-Path $game 'CastleMinerZ.exe') -PathType Leaf)) {
    throw 'CastleMinerZ.exe is missing from the selected installation.'
}
$detected = if (Test-Path -LiteralPath (Join-Path $content 'ReachContent/Terrain/Textures.xnb')) { 'Modern' }
            elseif (Test-Path -LiteralPath (Join-Path $content 'Terrain/Textures.xnb')) { 'Classic' }
            else { throw 'The selected installation has no supported terrain content. Verify its files in Steam.' }
if ($Style -eq 'Auto') { $Style = $detected }
if ($Style -ne $detected) { throw "This installation is $detected, not $Style." }
$banks = @('Sounds.xgs', 'Sounds.xsb', 'Sounds.xwb', 'SoundsStreaming.xwb')
foreach ($bank in $banks) {
    if (-not (Test-Path -LiteralPath (Join-Path $content $bank) -PathType Leaf)) {
        throw "Required audio bank is missing: Content/$bank"
    }
}
$packRoot = Assert-PrivateDestination $ContentPackRoot $game
$runtimeRoot = Assert-PrivateDestination $RuntimeDirectory $game
if ($packRoot.Equals($runtimeRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $packRoot.StartsWith($runtimeRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    $runtimeRoot.StartsWith($packRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Content-pack and runtime destinations must be separate, non-overlapping directories.'
}
$target = Join-Path $packRoot $Style.ToLowerInvariant()
if (Test-Path -LiteralPath $target) { [void](Assert-PrivateDestination $target $game) }
foreach ($directory in @($packRoot, $runtimeRoot, $target)) {
    if ((Test-Path -LiteralPath $directory) -and -not (Test-Path -LiteralPath $directory -PathType Container)) {
        throw "Expected a directory, but found a file: $directory"
    }
}
$existingRuntime = Join-Path $runtimeRoot 'steam_api.dll'
if (Test-Path -LiteralPath $existingRuntime) {
    [void](Assert-PrivateDestination $existingRuntime $game)
    if (-not (Test-Path -LiteralPath $existingRuntime -PathType Leaf)) { throw 'The Steam runtime path is a directory.' }
}
if ($ValidateOnly) {
    [PSCustomObject]@{ Game = $game; Style = $Style; Pack = $target; Runtime = $runtimeRoot; Valid = $true }
    return
}

if ([string]::IsNullOrWhiteSpace($Bridge)) {
    $packaged = Join-Path $PSScriptRoot 'ImportTools/OpenMinerZ.ContentBridge.exe'
    $repository = Split-Path -Parent $PSScriptRoot
    $project = Join-Path $repository 'tools/ContentBridge/OpenMinerZ.ContentBridge.csproj'
    if (Test-Path -LiteralPath $packaged -PathType Leaf) { $Bridge = $packaged }
    elseif (Test-Path -LiteralPath $project -PathType Leaf) {
        if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) { throw 'Building the importer requires the .NET SDK. See docs/building.md.' }
        $bridgeOutput = Join-Path $repository 'out/tools/content-bridge'
        & dotnet build $project --configuration Release --output $bridgeOutput --nologo --verbosity quiet
        if ($LASTEXITCODE -ne 0) { throw 'Content importer build failed. Install the .NET Framework 4.8 targeting pack and XNA 4.0 from the owned game prerequisites.' }
        $Bridge = Join-Path $bridgeOutput 'OpenMinerZ.ContentBridge.exe'
    } else { throw 'The content importer is missing. Keep ImportTools beside this script or pass -Bridge.' }
}
$Bridge = (Resolve-Path -LiteralPath $Bridge).Path
$token = [Guid]::NewGuid().ToString('N')
$stage = Join-Path $packRoot ('.import-' + $token)
$previous = Join-Path $packRoot ('.previous-' + $Style.ToLowerInvariant() + '-' + $token)
$runtimeStage = Join-Path $runtimeRoot ('.steam-' + $token + '.tmp')
$runtimeFile = Join-Path $runtimeRoot 'steam_api.dll'
$runtimePrevious = Join-Path $runtimeRoot ('.steam-' + $token + '.previous')
foreach ($packPath in @($target, $stage, $previous)) {
    if (-not [IO.Path]::GetFullPath($packPath).StartsWith($packRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'An import path escaped the content-pack root.'
    }
}
$oldPackMoved = $false; $newPackMoved = $false; $oldRuntimeMoved = $false; $newRuntimeMoved = $false
try {
    [void][IO.Directory]::CreateDirectory((Join-Path $stage 'DerivedData'))
    Write-Host "Importing $Style assets from your installed game. The original files are read only."
    & $Bridge $game (Join-Path $stage 'DerivedData')
    if ($LASTEXITCODE -ne 0) { throw "Content import failed (exit $LASTEXITCODE); the previous pack is unchanged." }
    foreach ($required in @('terrain_diffuse.bmp', 'terrain_normal_spec.bmp', 'terrain_metal_light.bmp',
                            'character.omrc', 'font_SmallFont.omfnt', 'screen_BlockUIBack.bmp')) {
        if (-not (Test-Path -LiteralPath (Join-Path $stage "DerivedData/$required") -PathType Leaf)) {
            throw "The importer did not produce $required; the previous pack is unchanged."
        }
    }
    [void][IO.Directory]::CreateDirectory((Join-Path $stage 'Content'))
    foreach ($bank in $banks) { Copy-Item -LiteralPath (Join-Path $content $bank) -Destination (Join-Path $stage "Content/$bank") }
    [IO.File]::WriteAllText((Join-Path $stage '.private-import'), "Locally imported CastleMiner Z assets. Do not commit or distribute this directory.`n")
    $manifest = [ordered]@{ format = 1; style = $Style.ToLowerInvariant(); importedUtc = [DateTime]::UtcNow.ToString('o'); files = @() }
    $manifest.files = @(Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($stage.Length + 1).Replace('\', '/'); sha256 = Get-ContentHash $_.FullName }
    })
    [IO.File]::WriteAllText((Join-Path $stage 'import-manifest.json'), ($manifest | ConvertTo-Json -Depth 5))
    $ownedSteam = Join-Path $game 'steam_api.dll'
    $hasSteam = Test-Path -LiteralPath $ownedSteam -PathType Leaf
    if ($hasSteam) {
        [void][IO.Directory]::CreateDirectory($runtimeRoot)
        Copy-Item -LiteralPath $ownedSteam -Destination $runtimeStage
    }
    if (Test-Path -LiteralPath $target) { Move-Item -LiteralPath $target -Destination $previous; $oldPackMoved = $true }
    Move-Item -LiteralPath $stage -Destination $target; $newPackMoved = $true
    if ($hasSteam) {
        if (Test-Path -LiteralPath $runtimeFile) { Move-Item -LiteralPath $runtimeFile -Destination $runtimePrevious; $oldRuntimeMoved = $true }
        Move-Item -LiteralPath $runtimeStage -Destination $runtimeFile; $newRuntimeMoved = $true
    }
    Write-Host "Imported: $target"
    if ($oldPackMoved) { Write-Host "Previous pack retained: $previous" }
    if (-not $hasSteam) { Write-Warning 'No steam_api.dll was found. Multiplayer still needs the Steam runtime from your own installation.' }
    Write-Host 'Restart OpenMinerZ. This private import must not be included in a public release.'
} catch {
    if ($newRuntimeMoved) { Move-Item -LiteralPath $runtimeFile -Destination $runtimeStage }
    if ($oldRuntimeMoved) { Move-Item -LiteralPath $runtimePrevious -Destination $runtimeFile }
    if ($newPackMoved) { Move-Item -LiteralPath $target -Destination $stage }
    if ($oldPackMoved) { Move-Item -LiteralPath $previous -Destination $target }
    throw
} finally {
    # These are unique task-owned staging paths, checked against their intended roots.
    if ([IO.Path]::GetFullPath($stage).StartsWith($packRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $stage)) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
    if ([IO.Path]::GetFullPath($runtimeStage).StartsWith($runtimeRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $runtimeStage)) {
        Remove-Item -LiteralPath $runtimeStage -Force
    }
}
