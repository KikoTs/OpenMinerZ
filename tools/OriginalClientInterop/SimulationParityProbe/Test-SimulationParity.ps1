param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
# Differential test of everything a peer simulates for itself, against the
# owned original assemblies.
#
# For each build the probe steps the ORIGINAL enemy, player and projectile
# classes through scripted scenarios and writes what they did frame by frame.
# The native test binary then replays the same inputs through OpenMinerZ and
# must reproduce the original: positions within 0.05 blocks, the state timeline
# within two frames, and every hit on the player.
#
# Scenarios named `base@ticks` are the original stepped at another frame time
# (the shipped game steps with the display; OpenMinerZ at a fixed 60 Hz). The
# `baseline` lines of the summary report how far the original drifts from
# itself there. Traces and summaries stay under the output directory; nothing
# is committed. Exit code 77 (no graphics device for the original content
# reader) is reported to CTest as a skip.
$ErrorActionPreference = 'Stop'
[void][IO.Directory]::CreateDirectory($OutputDirectory)

foreach ($profile in @(
    @{ Id = 'classic'; Directory = $ClassicGameDirectory },
    @{ Id = 'modern'; Directory = $ModernGameDirectory }
)) {
    $root = Join-Path $OutputDirectory $profile.Id
    [void][IO.Directory]::CreateDirectory($root)
    & $Probe $profile.Directory $root
    if ($LASTEXITCODE -eq 77) { Write-Output "SKIP no graphics device for the original content reader"; exit 77 }
    if ($LASTEXITCODE -ne 0) { throw "original simulation probe ($($profile.Id)) failed with exit code $LASTEXITCODE" }
    foreach ($trace in Get-ChildItem -Path $root -Filter '*.trace') {
        $summary = Join-Path $root ($trace.BaseName + '.summary.txt')
        & $NativeTests --replay $trace.FullName --summary $summary --strict
        if ($LASTEXITCODE -ne 0) { throw "native simulation differs from the original in $($profile.Id)/$($trace.Name)" }
    }
}
Write-Output 'PASS native simulation matches the original in both generations'
exit 0
