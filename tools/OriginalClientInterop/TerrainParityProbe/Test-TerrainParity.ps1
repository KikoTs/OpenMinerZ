param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateSet('quick', 'full')][string]$Plan = 'quick'
)
# Diffs the generated world block for block against the owned original builder:
#   1. the native test executable writes the chunk plan (seeds, every radial
#      band and transition on several headings, negative and far coordinates);
#   2. the probe executes the ORIGINAL CastleMinerZBuilder.BuildWorldChunk for
#      every planned chunk and writes the block ids it produced;
#   3. the native executable generates the same chunks and compares every cell,
#      reporting the percentage that differs per biome band and depth.
# Dumps stay under the build tree; nothing is copied from the game.
$ErrorActionPreference = 'Stop'
[void][IO.Directory]::CreateDirectory($OutputDirectory)
$failed = $false
foreach ($profile in @(
    @{ Name = 'classic'; Directory = $ClassicGameDirectory },
    @{ Name = 'modern'; Directory = $ModernGameDirectory }
)) {
    $request = Join-Path $OutputDirectory ($profile.Name + '-request.txt')
    $retail = Join-Path $OutputDirectory ($profile.Name + '-retail.bin')
    $report = Join-Path $OutputDirectory ($profile.Name + '-terrain-parity.txt')
    & $NativeTests --write-request $profile.Name $request $Plan
    if ($LASTEXITCODE -ne 0) { throw ('Could not write the ' + $profile.Name + ' chunk plan.') }
    & $Probe $profile.Directory $request $retail
    if ($LASTEXITCODE -ne 0) { throw ('The original ' + $profile.Name + ' world builder did not run.') }
    & $NativeTests --compare $profile.Name $request $retail $report
    if ($LASTEXITCODE -ne 0) {
        Write-Host ('FAIL ' + $profile.Name + ' generated terrain differs from retail; see ' + $report)
        $failed = $true
    }
    # The dump is tens of megabytes of derived game output; the report is kept.
    Remove-Item -LiteralPath $retail -ErrorAction SilentlyContinue
}
if ($failed) { exit 1 }
Write-Host 'PASS generated terrain matches the original builder block for block'
