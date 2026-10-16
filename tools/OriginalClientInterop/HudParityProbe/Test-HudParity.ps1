param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    # Rewrite tests/hud_parity_pinned.hpp from this recording instead of comparing.
    [string]$Pin = ''
)
# Holds OpenMinerZ's HUD layout to the owned original assemblies:
#   1. the probe runs the ORIGINAL InGameHUD.OnDraw for a set of scripted player
#      states at five window sizes and records every SpriteBatch call, plus a
#      PNG of the frame for side-by-side review;
#   2. the native test executable lists the strings its layout measures and the
#      probe answers with the ORIGINAL SpriteFont.MeasureString;
#   3. the native executable lays the same states out and compares its draw
#      list with the recording, element by element.
# Everything stays under the build tree; nothing is copied from the game.
# Works in Windows PowerShell 5.1 and PowerShell 7.
$ErrorActionPreference = 'Stop'
$retail = Join-Path $OutputDirectory 'retail'
[void][IO.Directory]::CreateDirectory($retail)
$sizes = @('1280x720', '1920x1080', '2560x1440', '1920x1200', '1024x768')
$builds = @(
    @{ Name = 'classic'; Directory = $ClassicGameDirectory },
    @{ Name = 'modern'; Directory = $ModernGameDirectory }
)
foreach ($build in $builds) {
    foreach ($size in $sizes) {
        & $Probe draw $build.Directory $retail $size | Out-Null
        if ($LASTEXITCODE -eq 77) { Write-Output 'SKIP no graphics device'; exit 77 }
        if ($LASTEXITCODE -ne 0) { throw ('The original HUD could not be recorded for ' + $build.Name + ' at ' + $size) }
    }
}
$classicStrings = Join-Path $OutputDirectory 'classic-strings.tsv'
$modernStrings = Join-Path $OutputDirectory 'modern-strings.tsv'
& $NativeTests --strings $classicStrings $modernStrings
if ($LASTEXITCODE -ne 0) { throw 'The native layout could not list its measured strings.' }
$classicMeasures = Join-Path $OutputDirectory 'classic-measures.tsv'
$modernMeasures = Join-Path $OutputDirectory 'modern-measures.tsv'
& $Probe measure $ClassicGameDirectory $classicMeasures 1280x720 $classicStrings
if ($LASTEXITCODE -ne 0) { throw 'The original Classic fonts could not be measured.' }
& $Probe measure $ModernGameDirectory $modernMeasures 1280x720 $modernStrings
if ($LASTEXITCODE -ne 0) { throw 'The original Modern fonts could not be measured.' }
if ($Pin -ne '') {
    & $NativeTests --pin $retail $classicMeasures $modernMeasures $Pin
    if ($LASTEXITCODE -ne 0) { throw 'The pinned header could not be written.' }
    Write-Output ('Pinned ' + $Pin + '; rebuild and rerun without -Pin.')
    exit 0
}
& $NativeTests --compare $retail
if ($LASTEXITCODE -ne 0) { throw 'The native HUD layout differs from the original.' }
