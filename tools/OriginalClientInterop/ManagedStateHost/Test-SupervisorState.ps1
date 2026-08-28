[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Launcher,
    [Parameter(Mandatory = $true)][string] $HostExecutable,
    [Parameter(Mandatory = $true)][string] $FixtureExecutable,
    [Parameter(Mandatory = $true)][string] $ControlScript
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ('openminerz-supervised-state-' + [Guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($stage) | Out-Null
$process = $null
try {
    Copy-Item -LiteralPath $Launcher -Destination (Join-Path $stage 'OpenMinerZInteropLauncher.exe')
    Copy-Item -LiteralPath $HostExecutable -Destination (Join-Path $stage 'OpenMinerZ.OriginalStateHost.exe')
    if (Test-Path -LiteralPath ($HostExecutable + '.config')) {
        Copy-Item -LiteralPath ($HostExecutable + '.config') -Destination (Join-Path $stage 'OpenMinerZ.OriginalStateHost.exe.config')
    }
    Copy-Item -LiteralPath $FixtureExecutable -Destination (Join-Path $stage 'CastleMinerZ.exe')
    if (Test-Path -LiteralPath ($FixtureExecutable + '.config')) {
        Copy-Item -LiteralPath ($FixtureExecutable + '.config') -Destination (Join-Path $stage 'CastleMinerZ.exe.config')
    }

    $process = Start-Process -FilePath (Join-Path $stage 'OpenMinerZInteropLauncher.exe') `
        -WorkingDirectory $stage -WindowStyle Hidden -PassThru
    $endpoint = Join-Path $stage 'OpenMinerZInterop.endpoint.json'
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (-not (Test-Path -LiteralPath $endpoint) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 50
        if ($process.HasExited) { throw "The supervised fixture exited with $($process.ExitCode)." }
    }
    if (-not (Test-Path -LiteralPath $endpoint)) { throw 'The supervisor endpoint was not published.' }

    $status = $null
    $stateDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $status = (& $ControlScript -Endpoint $endpoint -Command status | ConvertFrom-Json)
        if ([bool] $status.state_host) { break }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $stateDeadline)
    if (-not [bool] $status.ok -or -not [bool] $status.running -or -not [bool] $status.state_host) {
        throw 'The supervisor did not discover the managed state host.'
    }
    $capture = (& $ControlScript -Endpoint $endpoint -Command dump-state -CenterX 0 -CenterZ 0 `
        -StateName supervised-fixture.json | ConvertFrom-Json)
    if (-not [bool] $capture.ok) { throw 'The supervisor did not forward the state request.' }
    $state = Get-Content -LiteralPath (Join-Path $stage 'InteropStates\supervised-fixture.json') -Raw | ConvertFrom-Json
    if ([string] $state.region.hash -cne 'ef433c9576e46e47' -or
        [string] $state.inventory_hash -cne '628130799efa3ca1') {
        throw 'The supervised state request did not preserve the managed adapter contract.'
    }
}
finally {
    New-Item -ItemType File -Path (Join-Path $stage 'OpenMinerZFixture.stop') -Force | Out-Null
    if ($null -ne $process -and -not $process.HasExited) {
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Output 'Secured supervisor-to-managed-state forwarding passed.'
