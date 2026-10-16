[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $AuditScript
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function New-Record([int] $Sequence, [string] $Direction, [string] $Event) {
    [ordered] @{
        schema = 'openminerz.protocol-trace.v1'
        sequence = $Sequence
        unix_ms = 1700000000000L + $Sequence
        elapsed_us = 1000L * $Sequence
        role = 'client'
        direction = $Direction
        result = 'accepted'
        protocol = 3
        event = $Event
        sender_gid = 0
        recipient_gid = $null
        delivery = 'reliable_ordered'
        channel = 0
        packet_bytes = 0
        reason_hash = $null
    }
}

function Write-Fixture([string] $Destination, $Records) {
    $lines = foreach ($record in $Records) {
        $record | ConvertTo-Json -Compress
    }
    # Not Set-Content -Encoding utf8NoBOM: Windows PowerShell 5.1 has no such
    # encoding name, and CMake runs this with it when pwsh is not on PATH.
    [System.IO.File]::WriteAllLines($Destination, [string[]] @($lines),
        [System.Text.UTF8Encoding]::new($false))
}

function Invoke-Audit([string] $Trace) {
    $engine = (Get-Process -Id $PID).Path
    $output = & $engine -NoProfile -File $AuditScript -Path $Trace -Profile Gameplay 2>&1
    [pscustomobject] @{
        ExitCode = $LASTEXITCODE
        Certificate = (($output -join "`n") | ConvertFrom-Json)
    }
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    'openminerz-trace-audit-' + [guid]::NewGuid().ToString('N'))
[System.IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null
try {
    $events = @(
        @('internal', 'TraceStarted'),
        @('internal', 'DiscoveryStarted'),
        @('internal', 'LobbyJoinStarted'),
        @('internal', 'StockCompatibilitySelected'),
        @('outbound', 'AdmissionRequestSent'),
        @('inbound', 'Connected'),
        @('outbound', 'RequestWorldInfoMessage'),
        @('inbound', 'WorldInfoMessage'),
        @('outbound', 'PlayerExistsMessage'),
        @('inbound', 'PlayerExistsMessage'),
        @('outbound', 'RequestInventoryMessage'),
        @('inbound', 'InventoryRetrieveFromServerMessage'),
        @('outbound', 'ClientReadyForChunksMessage'),
        @('inbound', 'ProvideDeltaListMessage'),
        @('outbound', 'PlayerUpdateMessage'),
        @('inbound', 'PlayerUpdateMessage')
    )
    $records = [System.Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $events.Count; ++$index) {
        $records.Add((New-Record ($index + 1) $events[$index][0] $events[$index][1]))
    }

    $validPath = Join-Path $temporaryRoot 'valid.jsonl'
    Write-Fixture $validPath $records
    $valid = Invoke-Audit $validPath
    if ($valid.ExitCode -ne 0 -or -not [bool] $valid.Certificate.compatible -or
        [string] $valid.Certificate.schema -cne 'openminerz.protocol-trace-audit.v1') {
        throw 'A complete ordered client transcript did not receive a compatible certificate.'
    }

    $outOfOrder = [System.Collections.Generic.List[object]]::new()
    foreach ($record in $records) {
        $outOfOrder.Add($record)
    }
    $outOfOrder[14], $outOfOrder[15] = $outOfOrder[15], $outOfOrder[14]
    $outOfOrder[14].sequence = 15
    $outOfOrder[15].sequence = 16
    $orderingPath = Join-Path $temporaryRoot 'out-of-order.jsonl'
    Write-Fixture $orderingPath $outOfOrder
    $ordering = Invoke-Audit $orderingPath
    if ($ordering.ExitCode -ne 1 -or [bool] $ordering.Certificate.compatible -or
        @($ordering.Certificate.missing_requirements) -cnotcontains 'PlayerUpdateInbound') {
        throw 'The auditor did not reject an out-of-order gameplay transcript.'
    }

    $leaky = [System.Collections.Generic.List[object]]::new()
    foreach ($record in $records) {
        $copy = [ordered] @{}
        foreach ($key in $record.Keys) {
            $copy[$key] = $record[$key]
        }
        $leaky.Add($copy)
    }
    $leaky[0]['steam_id'] = '76561198000000000'
    $leakyPath = Join-Path $temporaryRoot 'leaky.jsonl'
    Write-Fixture $leakyPath $leaky
    $leakResult = Invoke-Audit $leakyPath
    if ($leakResult.ExitCode -ne 1 -or [bool] $leakResult.Certificate.compatible -or
        @($leakResult.Certificate.violations) -cnotcontains 'TraceSchemaFieldsMismatch') {
        throw 'The auditor accepted a trace containing a forbidden identity field.'
    }

    'OpenMinerZ protocol trace audit self-test passed.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
