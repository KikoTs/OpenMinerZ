[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$')][string]$PackageName,
    [string]$OutputDirectory,
    [ValidateRange(1, 64)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $repository 'out/releases' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
$destination = Join-Path $output $PackageName
$archive = "$destination.zip"
$sourceArchive = Join-Path $output "$PackageName-source.zip"
foreach ($path in @($destination, $archive, $sourceArchive, "$archive.sha256", "$sourceArchive.sha256")) {
    if (Test-Path -LiteralPath $path) { throw "Output already exists; choose a new package name: $path" }
}
if (($output.Equals($repository, [StringComparison]::OrdinalIgnoreCase) -or
     $output.StartsWith($repository + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) -and
    -not $output.StartsWith((Join-Path $repository 'out') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Package inside ignored out/ or outside the source checkout.'
}

function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed (exit $LASTEXITCODE). No release was published." }
}
function Get-ContentHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $hasher.Dispose() }
}
function Copy-ReleaseFile([string]$Source, [string]$Relative) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Required release file is missing: $Source" }
    $target = Join-Path $destination $Relative
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
    Copy-Item -LiteralPath $Source -Destination $target
}
function Write-ReleaseText([string]$Relative, [string]$Text) {
    [IO.File]::WriteAllText((Join-Path $destination $Relative), $Text, [Text.UTF8Encoding]::new($false))
}

Push-Location $repository
try {
    Invoke-Checked py @('-3', 'tools/release.py', '--history')
    Invoke-Checked py @('-3', '-m', 'unittest', 'discover', '-s', 'tools/tests', '-p', 'test_*.py')
    $before = (& py -3 tools/release.py --fingerprint).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint source.' }
    Invoke-Checked cmake @('--preset', 'native-public')
    Invoke-Checked cmake @('--build', '--preset', 'native-public', '--parallel', "$Jobs")
    Invoke-Checked ctest @('--preset', 'native-public')
    $release = Join-Path $repository 'out/build/native-public/Release'
    Invoke-Checked dotnet @('build', 'tools/ContentBridge/OpenMinerZ.ContentBridge.csproj',
        '--configuration', 'Release', '--output', (Join-Path $release 'ImportTools'), '--nologo')
    $after = (& py -3 tools/release.py --fingerprint).Trim()
    if ($LASTEXITCODE -ne 0 -or $after -ne $before) { throw 'Source changed during the build. Review the changes and run packaging again.' }

    [void][IO.Directory]::CreateDirectory($destination)
    Write-ReleaseText 'PACKAGE_INCOMPLETE.txt' 'Packaging is incomplete until this marker is removed.'
    foreach ($file in @('openminerz.exe', 'openminerz_mod_sdk.exe', 'SDL3.dll', 'freetype.dll',
                        'libpng16.dll', 'zlib1.dll', 'bz2.dll', 'brotlidec.dll', 'brotlicommon.dll',
                        'SteamBridge/OpenMinerZ.SteamBridge32.exe',
                        'AudioBridge/OpenMinerZ.AudioRuntime.exe', 'AudioBridge/OpenMinerZ.AudioRuntime.exe.config',
                        'ImportTools/OpenMinerZ.ContentBridge.exe', 'ImportTools/OpenMinerZ.ContentBridge.exe.config')) {
        Copy-ReleaseFile (Join-Path $release $file) $file
    }
    # Only reviewed original/licensed examples; never copy the whole build folder.
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $repository 'examples/mods') -File -Recurse) {
        Copy-ReleaseFile $file.FullName $file.FullName.Substring($repository.Length + 1)
    }
    foreach ($file in @('README.md', 'AGENTS.md', 'LICENSE', 'NOTICE.txt', 'THIRD_PARTY_NOTICES.txt')) {
        Copy-ReleaseFile (Join-Path $repository $file) $file
    }
    Copy-ReleaseFile (Join-Path $repository 'tools/Import-GameAssets.ps1') 'Import-GameAssets.ps1'
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $repository 'docs') -File -Recurse) {
        Copy-ReleaseFile $file.FullName $file.FullName.Substring($repository.Length + 1)
    }
    $dependencies = Join-Path $repository 'out/vcpkg_installed/x64-windows/share'
    foreach ($dependency in @('sdl3', 'freetype', 'libpng', 'zlib', 'bzip2', 'brotli', 'luau', 'cgltf', 'stb')) {
        Copy-ReleaseFile (Join-Path $dependencies "$dependency/copyright") "Notices/$dependency.txt"
    }
    Write-ReleaseText 'Play OpenMinerZ.cmd' @'
@echo off
setlocal
pushd "%~dp0"
"%~dp0openminerz.exe" %*
set "GAME_EXIT=%ERRORLEVEL%"
popd
if not "%GAME_EXIT%"=="0" pause
exit /b %GAME_EXIT%
'@
    Write-ReleaseText 'Import Game Assets.cmd' @'
@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Import-GameAssets.ps1" %*
set "IMPORT_EXIT=%ERRORLEVEL%"
pause
exit /b %IMPORT_EXIT%
'@
    Write-ReleaseText 'READ_ME.txt' @"
OpenMinerZ — $PackageName
Development preview for Windows x64. Back up important worlds before testing.

1. Extract the whole folder. Install and run your owned CastleMiner Z once.
2. Run Import Game Assets.cmd. If needed, pass -GameDirectory "D:\path\CastleMiner Z".
3. Run Play OpenMinerZ.cmd. Keep Steam signed in for multiplayer.

This package includes no original game art, fonts, audio banks, avatars, Steam
DLL or Microsoft prerequisite installers. Imports stay in your local user data.
See docs/importing-assets.md for troubleshooting. Add-ons are off by default
when bundled; enable them in the main-menu Add-Ons screen.

Saves/settings: %APPDATA%\OpenMinerZ
Private content/mods/runtime: %LOCALAPPDATA%\OpenMinerZ

Source: distribute $PackageName-source.zip alongside this binary package.
Copyright (c) 2026 Kiril Tsanov and contributors. AGPL-3.0-or-later.
See LICENSE, NOTICE.txt, THIRD_PARTY_NOTICES.txt and Notices/.
"@
    $buildInfo = [ordered]@{
        package = $PackageName; gitBase = (& git rev-parse HEAD).Trim(); sourceFingerprint = $before
        builtUtc = [DateTime]::UtcNow.ToString('o'); preset = 'native-public'; ownedAssetsIncluded = $false
        sourceArchive = [IO.Path]::GetFileName($sourceArchive)
    }
    Write-ReleaseText 'build-info.json' ($buildInfo | ConvertTo-Json)
    Invoke-Checked py @('-3', 'tools/release.py', '--runtime', $destination)
    Invoke-Checked py @('-3', 'tools/release.py', '--source-zip', $sourceArchive)
    $finalFingerprint = (& py -3 tools/release.py --fingerprint).Trim()
    if ($LASTEXITCODE -ne 0 -or $finalFingerprint -ne $before) {
        throw 'Source changed while assembling the package; the output is incomplete. Choose a new package name after reviewing the changes.'
    }
    Remove-Item -LiteralPath (Join-Path $destination 'PACKAGE_INCOMPLETE.txt')
    $files = @(Get-ChildItem -LiteralPath $destination -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($destination.Length + 1).Replace('\', '/');
                    bytes = $_.Length; sha256 = Get-ContentHash $_.FullName }
    })
    Write-ReleaseText 'files.sha256.json' (ConvertTo-Json -InputObject $files -Depth 4)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($destination, $archive, [IO.Compression.CompressionLevel]::Optimal, $true)
    [IO.File]::WriteAllText("$archive.sha256", (Get-ContentHash $archive) + "  $PackageName.zip`n")
    Write-Host "Runtime: $archive"
    Write-Host "Matching source: $sourceArchive"
} finally { Pop-Location }
