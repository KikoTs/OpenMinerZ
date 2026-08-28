[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $HostExecutable,
    [Parameter(Mandatory = $true)][string] $FixtureExecutable,
    [Parameter(Mandatory = $true)][string] $ControlScript,
    [Parameter(Mandatory = $true)][string] $CompareScript
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ('openminerz-managed-state-' + [Guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($stage) | Out-Null
$process = $null
try {
    Copy-Item -LiteralPath $HostExecutable -Destination (Join-Path $stage 'OpenMinerZ.OriginalStateHost.exe')
    $hostConfig = $HostExecutable + '.config'
    if (Test-Path -LiteralPath $hostConfig) {
        Copy-Item -LiteralPath $hostConfig -Destination (Join-Path $stage 'OpenMinerZ.OriginalStateHost.exe.config')
    }
    Copy-Item -LiteralPath $FixtureExecutable -Destination (Join-Path $stage 'FixtureCastleMinerZ.exe')
    $fixtureConfig = $FixtureExecutable + '.config'
    if (Test-Path -LiteralPath $fixtureConfig) {
        Copy-Item -LiteralPath $fixtureConfig -Destination (Join-Path $stage 'FixtureCastleMinerZ.exe.config')
    }
    $process = Start-Process -FilePath (Join-Path $stage 'OpenMinerZ.OriginalStateHost.exe') `
        -ArgumentList '--game', 'FixtureCastleMinerZ.exe' -WorkingDirectory $stage `
        -WindowStyle Hidden -PassThru
    $endpoint = Join-Path $stage 'OpenMinerZOriginalState.endpoint.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath $endpoint) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 50
        if ($process.HasExited) { throw "The managed state fixture exited with $($process.ExitCode)." }
    }
    if (-not (Test-Path -LiteralPath $endpoint)) { throw 'The managed state endpoint was not published.' }

    $status = $null
    $readyDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $status = (& $ControlScript -Endpoint $endpoint -Command status | ConvertFrom-Json)
        if ([bool] $status.ready) { break }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $readyDeadline)
    if (-not [bool] $status.ok -or -not [bool] $status.ready) {
        throw 'The managed state host did not report ready.'
    }
    $firstCapture = (& $ControlScript -Endpoint $endpoint -Command dump-state -CenterX 0 -CenterZ 0 -StateName fixture-a.json | ConvertFrom-Json)
    if (-not [bool] $firstCapture.ok) { throw 'The first deterministic state capture failed.' }
    $secondCapture = (& $ControlScript -Endpoint $endpoint -Command dump-state -CenterX 0 -CenterZ 0 -StateName fixture-b.json | ConvertFrom-Json)
    if (-not [bool] $secondCapture.ok) { throw 'The second deterministic state capture failed.' }

    $firstPath = Join-Path $stage 'InteropStates\fixture-a.json'
    $secondPath = Join-Path $stage 'InteropStates\fixture-b.json'
    $firstText = Get-Content -LiteralPath $firstPath -Raw
    $first = $firstText | ConvertFrom-Json
    $second = Get-Content -LiteralPath $secondPath -Raw | ConvertFrom-Json
    if ([string] $first.schema -cne 'openminerz.interop-state.v1' -or
        [string] $first.role -cne 'host' -or [string] $first.phase -cne 'hosting' -or
        [int] $first.protocol -ne 4 -or [int] $first.world_seed -ne 12345 -or
        [int] $first.region.min_x -ne -16 -or [int] $first.region.max_x -ne 16 -or
        [int] $first.region.min_z -ne -16 -or [int] $first.region.max_z -ne 16 -or
        [int] $first.remote_players -ne 1 -or [int] $first.enemies -ne 3 -or
        [int] $first.pickups -ne 2 -or -not [bool] $first.dragon_active -or
        @($first.players).Count -ne 2 -or [int] $first.player.x -ne 12 -or
        [Math]::Abs([double] $first.player.health - 79.0) -gt 0.001) {
        throw 'The managed fixture state did not preserve its known semantic values.'
    }
    if ($null -ne $first.edit_hash -or $null -ne $first.edit_count -or $null -ne $first.state_hash) {
        throw 'Unavailable original global-edit evidence must remain explicitly null.'
    }
    if ($firstText.Contains('Alpha Mine')) { throw 'The state artifact leaked a private waypoint label.' }
    if ([string] $first.region.hash -cne [string] $second.region.hash -or
        [string] $first.inventory_hash -cne [string] $second.inventory_hash) {
        throw 'Repeated captures of deterministic terrain or inventory diverged.'
    }
    if ([string] $first.region.hash -cne 'ef433c9576e46e47' -or
        [string] $first.inventory_hash -cne '628130799efa3ca1') {
        throw 'The original-compatible FNV byte contract changed.'
    }
    $powerShell = (Get-Process -Id $PID).Path
    $strictCertificateText = & $powerShell -NoProfile -File $CompareScript `
        -Left $firstPath -Right $secondPath -AllowOffline
    $strictExitCode = $LASTEXITCODE
    $strictCertificate = $strictCertificateText | ConvertFrom-Json
    if ($strictExitCode -ne 1 -or [bool] $strictCertificate.compatible -or
        'GlobalEditEvidenceUnavailable' -notin @($strictCertificate.mismatches)) {
        throw 'The comparator did not fail closed when global edit evidence was absent.'
    }
    $mutatedPath = Join-Path $stage 'InteropStates\fixture-inventory-mismatch.json'
    $mutated = Get-Content -LiteralPath $secondPath -Raw | ConvertFrom-Json
    $mutated.inventory_hash = '0000000000000000'
    $mutated | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $mutatedPath -Encoding utf8NoBOM
    $mismatchText = & $powerShell -NoProfile -File $CompareScript -Left $firstPath -Right $mutatedPath `
        -AllowOffline -AllowRegionOnlyEdits
    $mismatchExitCode = $LASTEXITCODE
    $mismatchCertificate = $mismatchText | ConvertFrom-Json
    if ($mismatchExitCode -ne 1 -or [bool] $mismatchCertificate.compatible -or
        'InventoryHashMismatch' -notin @($mismatchCertificate.mismatches)) {
        throw 'The comparator did not reject a one-field inventory divergence.'
    }
    $certificate = (& $CompareScript -Left $firstPath -Right $secondPath -AllowOffline -AllowRegionOnlyEdits | ConvertFrom-Json)
    if (-not [bool] $certificate.compatible) {
        throw 'The comparator rejected equivalent region-only original evidence.'
    }
}
finally {
    New-Item -ItemType File -Path (Join-Path $stage 'OpenMinerZFixture.stop') -Force | Out-Null
    if ($null -ne $process -and -not $process.HasExited) {
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Output ('Managed original-state host fixture passed (region={0}, inventory={1}).' -f `
    [string] $first.region.hash, [string] $first.inventory_hash)
