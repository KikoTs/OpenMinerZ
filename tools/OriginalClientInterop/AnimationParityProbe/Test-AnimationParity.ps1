param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    # Rewrites tests/animation_parity_pinned_<generation>.inc from this run.
    [string]$PinDirectory = ''
)
# Differential test of WHICH ANIMATION PLAYS WHEN, against the owned original
# assemblies.
#
# For each build the probe drives the ORIGINAL Player, its avatar animation
# collection and every held item's own input code through scripted timelines
# and writes, per frame and per animation channel, the clip, its clock, its
# speed and its cross-fade, for the local player and for the remote copy a peer
# animates from PlayerUpdateMessage. The native test binary then replays the
# same inputs through world::PlayerAnimator and must reproduce every frame
# exactly.
#
# Scenarios named `base@ticks` are the original stepped at another frame time
# (the shipped game steps with the display; OpenMinerZ at a fixed 60 Hz); they
# are replayed exactly too, and the `baseline` lines of the summary report how
# far the original's own event times move with its frame rate. Traces and
# summaries stay under the output directory; nothing is committed. Exit code 77
# (no graphics device for the original content reader) is reported to CTest as
# a skip.
$ErrorActionPreference = 'Stop'
[void][IO.Directory]::CreateDirectory($OutputDirectory)

foreach ($profile in @(
    @{ Id = 'classic'; Directory = $ClassicGameDirectory },
    @{ Id = 'modern'; Directory = $ModernGameDirectory }
)) {
    $root = Join-Path $OutputDirectory $profile.Id
    [void][IO.Directory]::CreateDirectory($root)
    $arguments = @($profile.Directory, $root)
    if ($PinDirectory -ne '') {
        $arguments += "--pin=$(Join-Path $PinDirectory "animation_parity_pinned_$($profile.Id).inc")"
    }
    & $Probe @arguments | Out-File -FilePath (Join-Path $root 'probe.log') -Encoding utf8
    if ($LASTEXITCODE -eq 77) { Write-Output "SKIP no graphics device for the original content reader"; exit 77 }
    if ($LASTEXITCODE -ne 0) { throw "original animation probe ($($profile.Id)) failed with exit code $LASTEXITCODE" }
    $trace = Join-Path $root 'animation.trace'
    $summary = Join-Path $root 'animation.summary.txt'
    & $NativeTests --replay $trace --summary $summary --strict
    if ($LASTEXITCODE -ne 0) { throw "native animation differs from the original in $($profile.Id)" }
}
Write-Output 'PASS native animation matches the original in both generations'
exit 0
