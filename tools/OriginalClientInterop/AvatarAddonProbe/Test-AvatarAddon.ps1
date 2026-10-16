param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    # The add-on is built per engine era (1.9.9 moved the type it replaces), and
    # ships two ways. Each build that is present gets its own run.
    [string]$ClassicAddon = '',      # standalone, pre-1.9.9:  runtimes\legacy\XboxAvatarMod.dll
    [string]$ModernAddon = '',       # standalone, 1.9.9+:     runtimes\modern\XboxAvatarMod.dll
    [string]$CastleForgeAddon = ''   # mod-loader build, 1.9.9+: !Mods\XboxAvatar.dll
)
# Xbox Avatar add-on interop, proven by running the add-on itself:
#   1. the native test writes OpenMinerZ's bytes - markers, a marked presence,
#      avatar payloads of several sizes, synthetic avatars;
#   2. AvatarAddonProbe hosts the add-on in the ORIGINAL engine, puts those
#      bytes through the add-on's readers and writers, then lets the add-on's
#      own state machine hold a whole session with the native test in `--peer`
#      mode: handshake, both avatars, live replacement on each side;
#   3. the native test reads back what the add-on wrote.
# The add-on is read from where it is and never copied; nothing is written
# outside the build tree; no game, Steam or socket is involved.
$ErrorActionPreference = 'Stop'
$runs = @(
    @{ Name = 'classic-standalone'; Protocol = '3'; Game = $ClassicGameDirectory; Addon = $ClassicAddon },
    @{ Name = 'modern-standalone'; Protocol = '4'; Game = $ModernGameDirectory; Addon = $ModernAddon },
    @{ Name = 'modern-castleforge'; Protocol = '4'; Game = $ModernGameDirectory; Addon = $CastleForgeAddon }
) | Where-Object { $_.Addon -and (Test-Path -LiteralPath $_.Addon) }
if (-not $runs) {
    Write-Output 'SKIP: no build of the Xbox Avatar add-on was found'
    exit 77
}
[void][IO.Directory]::CreateDirectory($OutputDirectory)
$fixtures = Join-Path $OutputDirectory 'native'
& $NativeTests --write-fixtures $fixtures
if ($LASTEXITCODE -ne 0) { throw 'Native avatar fixture export failed.' }
foreach ($run in $runs) {
    # The add-on keeps its cache and logs beside the executable that hosts it,
    # so each run gets a private copy of the probe (our own build output)
    # inside the output directory.
    $hostDirectory = Join-Path $OutputDirectory ('host-' + $run.Name)
    [void][IO.Directory]::CreateDirectory($hostDirectory)
    Copy-Item -Path (Join-Path (Split-Path -Parent $Probe) 'OpenMinerZ.AvatarAddonProbe.*') -Destination $hostDirectory -Force
    $results = Join-Path $OutputDirectory ('addon-' + $run.Name)
    Write-Output ('== ' + $run.Name + ': ' + $run.Addon)
    & (Join-Path $hostDirectory (Split-Path -Leaf $Probe)) $run.Game $run.Addon $fixtures $results $NativeTests $run.Protocol |
        Tee-Object -FilePath (Join-Path $OutputDirectory ($run.Name + '-interop.txt'))
    if ($LASTEXITCODE -ne 0) { throw ('The add-on and OpenMinerZ disagree: ' + $run.Name) }
    & $NativeTests --check-probe $results
    if ($LASTEXITCODE -ne 0) { throw ('The native codec could not read the add-on: ' + $run.Name) }
}
