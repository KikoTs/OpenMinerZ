param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
# Cross-feeds player state between OpenMinerZ and the owned original assemblies:
#   1. our sender's bytes for a scripted set of player states, plus what we mean
#      by them, are written by the native test executable;
#   2. the probe runs the ORIGINAL receive/apply path on those bytes and the
#      ORIGINAL sender on the same states, and checks what the original believes;
#   3. the native executable decodes the original sender's bytes and checks what
#      WE believe.
# Everything stays under the build tree; nothing is copied from the game.
$ErrorActionPreference = 'Stop'
$fixtures = Join-Path $OutputDirectory 'fixtures'
[void][IO.Directory]::CreateDirectory($fixtures)
& $NativeTests --write-fixtures $fixtures
if ($LASTEXITCODE -ne 0) { throw 'Native player fixture export failed.' }
foreach ($profile in @(
    @{ Id = '3'; Directory = $ClassicGameDirectory },
    @{ Id = '4'; Directory = $ModernGameDirectory }
)) {
    $original = Join-Path $OutputDirectory ('original-' + $profile.Id)
    & $Probe $profile.Directory (Join-Path $fixtures $profile.Id) $original |
        Tee-Object -FilePath (Join-Path $OutputDirectory ($profile.Id + '-player-parity.txt'))
    if ($LASTEXITCODE -ne 0) { throw ('Original player parity failed for protocol ' + $profile.Id) }
    & $NativeTests --check-original $original $profile.Id
    if ($LASTEXITCODE -ne 0) { throw ('Original sender bytes mean something else to us for protocol ' + $profile.Id) }
}
