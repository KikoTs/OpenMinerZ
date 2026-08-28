[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $Path,

    [ValidateSet('Bootstrap', 'Gameplay')]
    [string] $Profile = 'Gameplay',

    [switch] $AllowRejected,

    [string] $OutFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$allowedProperties = @(
    'schema', 'sequence', 'unix_ms', 'elapsed_us', 'role', 'direction', 'result',
    'protocol', 'event', 'sender_gid', 'recipient_gid', 'delivery', 'channel',
    'packet_bytes', 'reason_hash'
)

function Add-Code([System.Collections.Generic.List[string]] $Codes, [string] $Code) {
    if (-not $Codes.Contains($Code)) {
        $Codes.Add($Code)
    }
}

function Has-ExactSchema($Record) {
    $properties = @($Record.PSObject.Properties.Name)
    if ($properties.Count -ne $allowedProperties.Count) {
        return $false
    }
    foreach ($name in $allowedProperties) {
        if ($properties -cnotcontains $name) {
            return $false
        }
    }
    return $true
}

function Is-IntegerInRange($Value, [long] $Minimum, [long] $Maximum) {
    if ($null -eq $Value) {
        return $false
    }
    $parsed = 0L
    if (-not [long]::TryParse([string] $Value, [ref] $parsed)) {
        return $false
    }
    return $parsed -ge $Minimum -and $parsed -le $Maximum -and
           [string] $Value -cmatch '^-?[0-9]+$'
}

function Record-Matches($Record, $Requirement) {
    if ([string] $Record.result -cne 'accepted') {
        return $false
    }
    $requiredEvents = if ($Requirement.ContainsKey('events')) {
        @($Requirement.events)
    }
    else {
        @([string] $Requirement.event)
    }
    if ($requiredEvents -cnotcontains [string] $Record.event) {
        return $false
    }
    if ($null -ne $Requirement.direction -and
        [string] $Record.direction -cne [string] $Requirement.direction) {
        return $false
    }
    return $true
}

function Find-OrderedRequirement($Records, [int] $After, $Requirement) {
    for ($index = $After + 1; $index -lt $Records.Count; ++$index) {
        if (Record-Matches $Records[$index] $Requirement) {
            return $index
        }
    }
    return -1
}

function Write-Certificate($Certificate, [string] $Destination) {
    $json = $Certificate | ConvertTo-Json -Depth 7
    if (-not [string]::IsNullOrWhiteSpace($Destination)) {
        $parent = Split-Path -Parent $Destination
        if (-not [string]::IsNullOrWhiteSpace($parent)) {
            [System.IO.Directory]::CreateDirectory($parent) | Out-Null
        }
        Set-Content -LiteralPath $Destination -Value $json -Encoding utf8NoBOM
    }
    $json
}

$violations = [System.Collections.Generic.List[string]]::new()
$missing = [System.Collections.Generic.List[string]]::new()
$sessions = [System.Collections.Generic.List[object]]::new()
$current = $null

try {
    foreach ($line in @(Get-Content -LiteralPath $Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            Add-Code $violations 'BlankTraceRecord'
            continue
        }
        try {
            $record = $line | ConvertFrom-Json
        }
        catch {
            Add-Code $violations 'MalformedJsonRecord'
            continue
        }
        if (-not (Has-ExactSchema $record)) {
            Add-Code $violations 'TraceSchemaFieldsMismatch'
            continue
        }
        if ([string] $record.schema -cne 'openminerz.protocol-trace.v1') {
            Add-Code $violations 'UnsupportedTraceSchema'
            continue
        }
        if ([string] $record.event -ceq 'TraceStarted' -and [string] $record.sequence -ceq '1') {
            $current = [System.Collections.Generic.List[object]]::new()
            $sessions.Add($current)
        }
        if ($null -eq $current) {
            Add-Code $violations 'RecordBeforeTraceStarted'
            continue
        }
        $current.Add($record)
    }
}
catch {
    Add-Code $violations 'TraceReadFailure'
}

if ($sessions.Count -eq 0) {
    Add-Code $violations 'NoTraceSession'
    $records = @()
}
else {
    $records = @($sessions[$sessions.Count - 1])
}

$role = ''
$protocol = 0
$rejectedCount = 0
$failureCount = 0
$eventCounts = [ordered] @{}
$firstUnixMs = 0L
$lastUnixMs = 0L
$durationUs = 0L

if ($records.Count -gt 0) {
    $role = [string] $records[0].role
    if ($role -cne 'host' -and $role -cne 'client') {
        Add-Code $violations 'InvalidRole'
    }
    if (Is-IntegerInRange $records[0].protocol 3 4) {
        $protocol = [int] $records[0].protocol
    }
    else {
        Add-Code $violations 'InvalidProtocol'
    }

    $previousElapsed = -1L
    $previousUnix = -1L
    for ($index = 0; $index -lt $records.Count; ++$index) {
        $record = $records[$index]
        if (-not (Is-IntegerInRange $record.sequence 1 2000000) -or
            [long] $record.sequence -ne $index + 1) {
            Add-Code $violations 'NonContiguousSequence'
        }
        if (-not (Is-IntegerInRange $record.unix_ms 0 ([long]::MaxValue)) -or
            -not (Is-IntegerInRange $record.elapsed_us 0 ([long]::MaxValue))) {
            Add-Code $violations 'InvalidTimestamp'
        }
        else {
            $elapsed = [long] $record.elapsed_us
            $unix = [long] $record.unix_ms
            if ($elapsed -lt $previousElapsed) {
                Add-Code $violations 'NonMonotonicElapsedTime'
            }
            if ($unix -lt $previousUnix) {
                Add-Code $violations 'NonMonotonicWallTime'
            }
            $previousElapsed = $elapsed
            $previousUnix = $unix
        }
        if ([string] $record.role -cne $role) {
            Add-Code $violations 'RoleChangedWithinSession'
        }
        if (-not (Is-IntegerInRange $record.protocol 3 4) -or
            [int] $record.protocol -ne $protocol) {
            Add-Code $violations 'ProtocolChangedWithinSession'
        }
        if (@('inbound', 'outbound', 'internal') -cnotcontains [string] $record.direction) {
            Add-Code $violations 'InvalidDirection'
        }
        if (@('accepted', 'rejected', 'failure') -cnotcontains [string] $record.result) {
            Add-Code $violations 'InvalidResult'
        }
        if ([string] $record.event -cnotmatch '^[A-Za-z0-9_.-]{1,128}$') {
            Add-Code $violations 'InvalidEventLabel'
        }
        if ([string] $record.event -ceq 'InvalidEventLabel') {
            Add-Code $violations 'SanitizedInvalidEventPresent'
        }
        if (@('unreliable', 'reliable_unordered', 'unreliable_sequenced',
              'reliable_sequenced', 'reliable_ordered', 'chat', 'player_update_state',
              'dragon_update_warmup') -cnotcontains [string] $record.delivery) {
            Add-Code $violations 'InvalidDelivery'
        }
        if (-not (Is-IntegerInRange $record.sender_gid 0 255) -or
            ($null -ne $record.recipient_gid -and
             -not (Is-IntegerInRange $record.recipient_gid 0 255)) -or
            -not (Is-IntegerInRange $record.channel 0 255) -or
            -not (Is-IntegerInRange $record.packet_bytes 0 16777216)) {
            Add-Code $violations 'UnboundedRecordValue'
        }
        if ($null -ne $record.reason_hash -and
            [string] $record.reason_hash -cnotmatch '^[0-9a-f]{16}$') {
            Add-Code $violations 'InvalidReasonFingerprint'
        }
        $event = [string] $record.event
        if (-not $eventCounts.Contains($event)) {
            $eventCounts[$event] = 0
        }
        $eventCounts[$event] = [int] $eventCounts[$event] + 1
        if ([string] $record.result -ceq 'rejected') {
            ++$rejectedCount
        }
        elseif ([string] $record.result -ceq 'failure') {
            ++$failureCount
        }
    }
    $firstUnixMs = [long] $records[0].unix_ms
    $lastUnixMs = [long] $records[$records.Count - 1].unix_ms
    $durationUs = [long] $records[$records.Count - 1].elapsed_us
}

if ($failureCount -gt 0) {
    Add-Code $violations 'FailureRecordPresent'
}
if (-not $AllowRejected -and $rejectedCount -gt 0) {
    Add-Code $violations 'RejectedRecordPresent'
}

$requirements = @()
if ($role -ceq 'client') {
    $requirements = @(
        @{ code = 'TraceStarted'; event = 'TraceStarted'; direction = 'internal' },
        @{ code = 'DiscoveryStarted'; event = 'DiscoveryStarted'; direction = 'internal' },
        @{ code = 'LobbyJoinStarted'; event = 'LobbyJoinStarted'; direction = 'internal' },
        @{ code = 'CompatibilityAdmission';
           events = @('StockCompatibilitySelected', 'ModHandshakeAccepted');
           direction = $null },
        @{ code = 'AdmissionRequestSent'; event = 'AdmissionRequestSent'; direction = 'outbound' },
        @{ code = 'Connected'; event = 'Connected'; direction = 'inbound' },
        @{ code = 'RequestWorldInfoOutbound'; event = 'RequestWorldInfoMessage'; direction = 'outbound' },
        @{ code = 'WorldInfoInbound'; event = 'WorldInfoMessage'; direction = 'inbound' },
        @{ code = 'PlayerExistsOutbound'; event = 'PlayerExistsMessage'; direction = 'outbound' },
        @{ code = 'PlayerExistsInbound'; event = 'PlayerExistsMessage'; direction = 'inbound' },
        @{ code = 'RequestInventoryOutbound'; event = 'RequestInventoryMessage'; direction = 'outbound' },
        @{ code = 'InventoryInbound'; event = 'InventoryRetrieveFromServerMessage'; direction = 'inbound' },
        @{ code = 'ClientReadyOutbound'; event = 'ClientReadyForChunksMessage'; direction = 'outbound' },
        @{ code = 'DeltaListInbound'; event = 'ProvideDeltaListMessage'; direction = 'inbound' }
    )
    if ($Profile -ceq 'Gameplay') {
        $requirements += @(
            @{ code = 'PlayerUpdateOutbound'; event = 'PlayerUpdateMessage'; direction = 'outbound' },
            @{ code = 'PlayerUpdateInbound'; event = 'PlayerUpdateMessage'; direction = 'inbound' }
        )
    }
}
elseif ($role -ceq 'host') {
    $requirements = @(
        @{ code = 'TraceStarted'; event = 'TraceStarted'; direction = 'internal' },
        @{ code = 'HostStarted'; event = 'HostStarted'; direction = 'internal' },
        @{ code = 'PeerConnected'; event = 'PeerConnected'; direction = 'inbound' },
        @{ code = 'RequestWorldInfoInbound'; event = 'RequestWorldInfoMessage'; direction = 'inbound' },
        @{ code = 'WorldInfoOutbound'; event = 'WorldInfoMessage'; direction = 'outbound' },
        @{ code = 'PlayerExistsInbound'; event = 'PlayerExistsMessage'; direction = 'inbound' },
        @{ code = 'PlayerExistsOutbound'; event = 'PlayerExistsMessage'; direction = 'outbound' },
        @{ code = 'RequestInventoryInbound'; event = 'RequestInventoryMessage'; direction = 'inbound' },
        @{ code = 'InventoryOutbound'; event = 'InventoryRetrieveFromServerMessage'; direction = 'outbound' },
        @{ code = 'ClientReadyInbound'; event = 'ClientReadyForChunksMessage'; direction = 'inbound' },
        @{ code = 'DeltaListOutbound'; event = 'ProvideDeltaListMessage'; direction = 'outbound' }
    )
    if ($Profile -ceq 'Gameplay') {
        $requirements += @(
            @{ code = 'PlayerUpdateInbound'; event = 'PlayerUpdateMessage'; direction = 'inbound' },
            @{ code = 'PlayerUpdateOutbound'; event = 'PlayerUpdateMessage'; direction = 'outbound' }
        )
    }
}

$cursor = -1
foreach ($requirement in $requirements) {
    $found = Find-OrderedRequirement $records $cursor $requirement
    if ($found -lt 0) {
        Add-Code $missing ([string] $requirement.code)
    }
    else {
        $cursor = $found
    }
}

$compatible = $violations.Count -eq 0 -and $missing.Count -eq 0
$certificate = [ordered] @{
    schema = 'openminerz.protocol-trace-audit.v1'
    compatible = $compatible
    profile = $Profile.ToLowerInvariant()
    role = $role
    protocol = $protocol
    session_count = $sessions.Count
    selected_session = if ($sessions.Count -gt 0) { $sessions.Count } else { 0 }
    record_count = $records.Count
    first_unix_ms = $firstUnixMs
    last_unix_ms = $lastUnixMs
    duration_us = $durationUs
    rejected_records = $rejectedCount
    failure_records = $failureCount
    event_counts = $eventCounts
    missing_requirements = @($missing)
    violations = @($violations)
}
Write-Certificate $certificate $OutFile
if ($compatible) {
    exit 0
}
exit 1
