[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $Launcher,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $Fixture,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $ControlScript
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'openminerz-original-supervisor-' + [guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null
$process = $null
$gameProcess = $null
try {
    $stagedLauncher = Join-Path $temporaryRoot 'OpenMinerZInteropLauncher.exe'
    $stagedFixture = Join-Path $temporaryRoot 'CastleMinerZ.exe'
    Copy-Item -LiteralPath $Launcher -Destination $stagedLauncher
    Copy-Item -LiteralPath $Fixture -Destination $stagedFixture
    $process = Start-Process -FilePath $stagedLauncher -WorkingDirectory $temporaryRoot -PassThru
    $endpoint = Join-Path $temporaryRoot 'OpenMinerZInterop.endpoint.json'
    for ($attempt = 0; $attempt -lt 100 -and
         -not (Test-Path -LiteralPath $endpoint -PathType Leaf); ++$attempt) {
        Start-Sleep -Milliseconds 50
    }
    if (-not (Test-Path -LiteralPath $endpoint -PathType Leaf)) {
        throw 'The supervisor did not publish its bounded local endpoint.'
    }
    $endpointData = Get-Content -LiteralPath $endpoint -Raw | ConvertFrom-Json
    $gameProcess = Get-Process -Id ([int] $endpointData.game_pid) -ErrorAction Stop

    $statusText = & $ControlScript -Endpoint $endpoint -Command status
    $status = ($statusText -join "`n") | ConvertFrom-Json
    if (-not [bool] $status.ok -or -not [bool] $status.running -or
        -not [bool] $status.window) {
        throw 'The supervisor did not report the owned fixture window as ready.'
    }

    $captureText = & $ControlScript -Endpoint $endpoint -Command capture `
        -CaptureName 'self-test.bmp'
    $capture = ($captureText -join "`n") | ConvertFrom-Json
    $capturePath = Join-Path $temporaryRoot 'InteropCaptures\self-test.bmp'
    if (-not [bool] $capture.ok -or -not (Test-Path -LiteralPath $capturePath) -or
        (Get-Item -LiteralPath $capturePath).Length -le 54) {
        throw 'The supervisor did not create a valid screenshot barrier.'
    }

    $quitText = & $ControlScript -Endpoint $endpoint -Command quit
    $quit = ($quitText -join "`n") | ConvertFrom-Json
    if (-not [bool] $quit.ok) {
        throw 'The supervisor did not issue a graceful close to its owned fixture.'
    }
    if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) {
        throw 'The supervisor or fixture did not exit cleanly after graceful close.'
    }
    'OpenMinerZ original-client supervisor self-test passed.'
}
finally {
    if ($null -ne $gameProcess -and -not $gameProcess.HasExited) {
        Stop-Process -Id $gameProcess.Id -Force
        $gameProcess.WaitForExit()
    }
    if ($null -ne $process -and -not $process.HasExited) {
        if (-not $process.WaitForExit(2000)) {
            Stop-Process -Id $process.Id -Force
            $process.WaitForExit()
        }
    }
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
