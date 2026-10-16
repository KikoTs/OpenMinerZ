param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    # <build>/content-packs: when a pack has imported sprite fonts, they are
    # held to the original SpriteFont's glyph table and measurements too.
    [string]$ContentPacks = ''
)
# Differential test of what a player READS against the owned original builds.
#
#   1. PresentationParityProbe loads each shipped build offline and writes its
#      string table, the Name/Description of every registered item class, every
#      BlockType.Name, the achievements as CreateAcheivements builds them, each
#      sprite font's metrics with SpriteFont.MeasureString over sample text, and
#      the cue names in the sound bank;
#   2. the native test answers the same keys from the tables the client
#      compiles in (and from the imported fonts, when a pack is present);
#   3. every key must agree.
#
# Windows PowerShell 5.1 compatible. Everything generated stays under
# -OutputDirectory; nothing from the owned game is copied anywhere.
$ErrorActionPreference = 'Stop'
$failed = $false
[void][IO.Directory]::CreateDirectory($OutputDirectory)
foreach ($generation in @(
        @{ Name = 'classic'; Directory = $ClassicGameDirectory },
        @{ Name = 'modern'; Directory = $ModernGameDirectory })) {
    $directory = Join-Path $OutputDirectory $generation.Name
    [void][IO.Directory]::CreateDirectory($directory)
    Write-Host ('=== ' + $generation.Name + ' ===')
    & $Probe $generation.Directory $directory | Write-Host
    if ($LASTEXITCODE -ne 0) { throw ('The original presentation probe failed for ' + $generation.Name) }

    $arguments = @('--compare', (Join-Path $directory 'presentation-facts.txt'), $generation.Name)
    $derived = if ($ContentPacks) { Join-Path (Join-Path $ContentPacks $generation.Name) 'DerivedData' } else { '' }
    if ($derived -and (Test-Path (Join-Path $derived 'font_MedFont.omfnt'))) {
        $arguments += $derived
    } else {
        Write-Host 'NOTE no imported sprite fonts for this generation: text tables compared, fonts not.'
    }
    & $NativeTests @arguments | Write-Host
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}
if ($failed) {
    Write-Host 'FAIL presentation differs from the original (see above)'
    exit 1
}
Write-Host 'PASS presentation matches the original in both generations'
exit 0
