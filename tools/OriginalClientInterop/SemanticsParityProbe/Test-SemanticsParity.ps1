param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
# Differential test of gameplay SEMANTICS against the owned original assemblies.
#
#   1. the native client writes its tables and sampled sends as key=value facts
#      and its own SpawnEnemyMessage payloads;
#   2. SemanticsParityProbe executes the original's code for the same things,
#      writes the same keys, saves the payloads the original emitted, and reads
#      the native payloads back through the original's readers;
#   3. the native client pushes the original's payloads through its own host
#      authority, decoders and limits;
#   4. every key both sides produced must agree.
#
# Windows PowerShell 5.1 compatible. Everything generated stays under
# -OutputDirectory; nothing from the owned game is copied anywhere.
$ErrorActionPreference = 'Stop'
$knownPath = Join-Path $PSScriptRoot 'Known-Differences.txt'
$known = @()
if (Test-Path $knownPath) {
    foreach ($line in Get-Content $knownPath) {
        if ($line -match '^\s*(#|$)') { continue }
        $parts = $line -split '\s+', 3
        $known += [pscustomobject]@{ Protocol = $parts[0]; Pattern = $parts[1]; Reason = $parts[2] }
    }
}

function Read-Facts([string]$Path) {
    $facts = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        $index = $line.IndexOf('=')
        if ($index -gt 0) { $facts[$line.Substring(0, $index)] = $line.Substring($index + 1) }
    }
    return $facts
}

function Convert-Value([string]$Text) {
    if ($Text.StartsWith('f:')) {
        return [double][BitConverter]::ToSingle([BitConverter]::GetBytes([Convert]::ToUInt32($Text.Substring(2), 16)), 0)
    }
    return [double]::Parse($Text, [Globalization.CultureInfo]::InvariantCulture)
}

$failed = $false
[void][IO.Directory]::CreateDirectory($OutputDirectory)
foreach ($profile in @(
        @{ Id = '3'; Directory = $ClassicGameDirectory },
        @{ Id = '4'; Directory = $ModernGameDirectory })) {
    $directory = Join-Path $OutputDirectory $profile.Id
    [void][IO.Directory]::CreateDirectory($directory)
    Write-Host ('=== network version ' + $profile.Id + ' ===')

    & $NativeTests --write-facts $directory $profile.Id
    if ($LASTEXITCODE -ne 0) { throw ('Native fact export failed for protocol ' + $profile.Id) }

    & $Probe $profile.Directory $directory (Join-Path $directory 'native') |
        Tee-Object -FilePath (Join-Path $directory 'probe.txt') |
        Where-Object { $_ -notmatch '^PASS ' } | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Host ('FAIL the original-side checks failed for protocol ' + $profile.Id)
        $failed = $true
    }

    $fixtures = Join-Path $directory 'fixtures'
    if (@(Get-ChildItem $fixtures -Filter '*.bin' -ErrorAction SilentlyContinue).Count -gt 0) {
        & $NativeTests --cross-feed $fixtures $profile.Id
        if ($LASTEXITCODE -ne 0) {
            Write-Host ('FAIL original payloads were refused by the native client, protocol ' + $profile.Id)
            $failed = $true
        }
    }

    $original = Read-Facts (Join-Path $directory 'original-facts.txt')
    $native = Read-Facts (Join-Path $directory 'native-facts.txt')
    $compared = 0
    $mismatches = New-Object System.Collections.Generic.List[string]
    $waived = New-Object System.Collections.Generic.List[string]
    foreach ($key in ($native.Keys | Sort-Object)) {
        if (-not $original.ContainsKey($key)) { continue }
        ++$compared
        $ours = $native[$key]
        $theirs = $original[$key]
        $equal = $ours -eq $theirs
        if (-not $equal -and $key -match '\.(min|max)$') {
            # Sampled extremes of a random quantity: both sides drew about a
            # thousand samples, so the bounds agree to a fraction of the range.
            $stem = $key.Substring(0, $key.LastIndexOf('.'))
            $span = [Math]::Abs((Convert-Value $original[$stem + '.max']) - (Convert-Value $original[$stem + '.min']))
            $tolerance = [Math]::Max(2.0, $span * 0.02)
            if ($ours.StartsWith('f:')) { $tolerance = [Math]::Max(0.001, $span * 0.02) }
            $equal = [Math]::Abs((Convert-Value $ours) - (Convert-Value $theirs)) -le $tolerance
        }
        if ($equal) { continue }
        $shown = $key + ': native=' + $ours + ' original=' + $theirs
        if ($ours.StartsWith('f:')) {
            $shown += ' (' + (Convert-Value $ours) + ' vs ' + (Convert-Value $theirs) + ')'
        }
        $reason = $null
        foreach ($entry in $known) {
            if ($entry.Protocol -eq $profile.Id -and $key -match $entry.Pattern) { $reason = $entry.Reason; break }
        }
        if ($null -ne $reason) { $waived.Add($shown + '   [known: ' + $reason + ']') }
        else { $mismatches.Add($shown) }
    }
    $report = Join-Path $directory 'comparison.txt'
    $lines = @('compared=' + $compared, 'mismatches=' + $mismatches.Count, 'known=' + $waived.Count)
    $lines += ($original.Keys | Where-Object { $_ -like 'scenario.*' } | Sort-Object |
            ForEach-Object { $_ + '=' + $original[$_] })
    $lines += $mismatches | ForEach-Object { 'MISMATCH ' + $_ }
    $lines += $waived | ForEach-Object { 'KNOWN ' + $_ }
    [IO.File]::WriteAllLines($report, [string[]]$lines)
    Write-Host ('compared ' + $compared + ' facts: ' + $mismatches.Count + ' mismatches, ' + $waived.Count + ' known differences')
    $original.Keys | Where-Object { $_ -like 'scenario.*' -and $original[$_] -ne 'executed' } | Sort-Object |
        ForEach-Object { Write-Host ('NOT EXECUTED ' + $_.Substring(9) + ' (read, not run, for protocol ' + $profile.Id + ')') }
    foreach ($line in ($mismatches | Select-Object -First 60)) { Write-Host ('MISMATCH ' + $line) }
    if ($mismatches.Count -gt 60) { Write-Host ('... ' + ($mismatches.Count - 60) + ' more in ' + $report) }
    if ($mismatches.Count -ne 0) { $failed = $true }
}
if ($failed) { throw 'Original/native gameplay semantics differ.' }
Write-Host 'Original/native gameplay semantics agree.'
