[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $HostExecutable,
    [Parameter(Mandatory = $true)][string] $ClassicGame,
    [Parameter(Mandatory = $true)][string] $ModernGame
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-GameContract(
    [string] $Game,
    [string] $ExpectedProfile,
    [int] $ExpectedProtocol,
    [string] $ExpectedVersion,
    [string] $ExpectedHash
) {
    $source = Split-Path -Parent $Game
    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ('openminerz-contract-' + [Guid]::NewGuid().ToString('N'))
    [System.IO.Directory]::CreateDirectory($stage) | Out-Null
    try {
        Copy-Item -LiteralPath $HostExecutable -Destination (Join-Path $stage 'OpenMinerZ.OriginalStateHost.exe')
        if (Test-Path -LiteralPath ($HostExecutable + '.config')) {
            Copy-Item -LiteralPath ($HostExecutable + '.config') -Destination (Join-Path $stage 'OpenMinerZ.OriginalStateHost.exe.config')
        }
        Copy-Item -LiteralPath $Game -Destination (Join-Path $stage 'CastleMinerZ.exe')
        foreach ($name in @('DNA.Common.dll', 'DNA.Steam.dll', 'DNA.Twitch.dll', 'Facebook.dll',
                            'HiDefContentLib.dll', 'ReachContentLib.dll', 'Services.Client.dll',
                            'TwitchSdkWrapper.dll')) {
            $dependency = Join-Path $source $name
            if (Test-Path -LiteralPath $dependency) {
                Copy-Item -LiteralPath $dependency -Destination (Join-Path $stage $name)
            }
        }
        $text = & (Join-Path $stage 'OpenMinerZ.OriginalStateHost.exe') --probe-game CastleMinerZ.exe
        if ($LASTEXITCODE -ne 0) {
            throw "The $ExpectedProfile assembly contract probe failed: $($text -join '')"
        }
        $result = $text | ConvertFrom-Json
        if ([string] $result.schema -cne 'openminerz.original-contract.v1' -or
            -not [bool] $result.ok -or [string] $result.profile -cne $ExpectedProfile -or
            [int] $result.protocol -ne $ExpectedProtocol -or
            [string] $result.assembly_version -cne $ExpectedVersion -or
            [string] $result.sha256 -cne $ExpectedHash) {
            throw "The $ExpectedProfile assembly did not match its pinned clean-room contract."
        }
        if (Test-Path -LiteralPath (Join-Path $stage 'OpenMinerZOriginalState.endpoint.json')) {
            throw 'A metadata-only contract probe unexpectedly started the runtime controller.'
        }
    }
    finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-GameContract -Game $ClassicGame -ExpectedProfile classic -ExpectedProtocol 3 `
    -ExpectedVersion '1.6.3.8' `
    -ExpectedHash '64c950cdc7ac9194ea633acc84b27350ffadf8030a26a26b731c3b7a43be673d'
Test-GameContract -Game $ModernGame -ExpectedProfile modern -ExpectedProtocol 4 `
    -ExpectedVersion '1.9.9.8' `
    -ExpectedHash '45d951391f204a20803c5e249d7f43fec0acbf320f5cf6c3e42ccccfe778cea3'
Write-Output 'Pinned Classic and Modern original-assembly contracts passed without launch.'
