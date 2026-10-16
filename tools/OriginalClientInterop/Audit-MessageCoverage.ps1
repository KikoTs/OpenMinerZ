[CmdletBinding()]
param(
    [string]$Repository = (Join-Path $PSScriptRoot '../..'),
    [string[]]$TracePaths = @(),
    [string]$OutputPath = ''
)

$ErrorActionPreference = 'Stop'
$Repository = (Resolve-Path -LiteralPath $Repository).Path
$manifest = Get-Content -LiteralPath (Join-Path $Repository 'src/net/protocol.cpp') -Raw
$codec = Get-Content -LiteralPath (Join-Path $Repository 'src/net/payload_codec.cpp') -Raw
$runtimeFiles = @('src/app.cpp', 'src/net/authoritative_bootstrap.cpp', 'src/net/legacy_steam_runtime_host.cpp',
                  'src/net/legacy_steam_runtime_client.cpp')
$runtime = ($runtimeFiles | ForEach-Object {
    Get-Content -LiteralPath (Join-Path $Repository $_) -Raw
}) -join "`n"

# This is a structural inventory and historical observation report. A named
# branch or an accepted transport event does not certify gameplay semantics.
$counts = @{}
$rowsRead = 0
foreach ($path in $TracePaths) {
    foreach ($line in [System.IO.File]::ReadLines((Resolve-Path -LiteralPath $path).Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $row = $line | ConvertFrom-Json
        if ($row.schema -ne 'openminerz.protocol-trace.v1') {
            throw "Unsupported trace schema in $path"
        }
        ++$rowsRead
        if ($row.direction -notin @('inbound', 'outbound')) { continue }
        if ($row.result -ne 'accepted') { continue }
        if ($row.role -notin @('host', 'client')) { continue }
        $key = "$($row.protocol)|$($row.event)|$($row.role)|$($row.direction)"
        $counts[$key] = 1 + $counts[$key]
    }
}

$rows = @(
    foreach ($profile in @(@{ Name = 'classic'; Protocol = 3; Count = 53 },
                            @{ Name = 'modern'; Protocol = 4; Count = 55 })) {
        $block = [regex]::Match($manifest, "(?s)$($profile.Name)_message_names\{(.*?)\};")
        $names = @([regex]::Matches($block.Groups[1].Value, '"DNA\.[^"]+\.([^".]+)"sv') |
            ForEach-Object { $_.Groups[1].Value })
        if ($names.Count -ne $profile.Count) { throw 'Stock message manifest count changed; review the audit.' }
        $id = 0
        foreach ($name in $names) {
            [pscustomobject][ordered]@{
                profile = $profile.Name
                protocol = $profile.Protocol
                id = $id++
                message = $name
                typed_name_present = $codec.Contains('"' + $name + '"')
                runtime_name_present = $runtime.Contains('"' + $name + '"')
                host_inbound = [int]$counts["$($profile.Protocol)|$name|host|inbound"]
                host_outbound = [int]$counts["$($profile.Protocol)|$name|host|outbound"]
                client_inbound = [int]$counts["$($profile.Protocol)|$name|client|inbound"]
                client_outbound = [int]$counts["$($profile.Protocol)|$name|client|outbound"]
            }
        }
    }
)
$report = [pscustomobject][ordered]@{
    schema = 'openminerz.message-coverage.v1'
    trace_files = $TracePaths.Count
    trace_rows = $rowsRead
    limitation = 'Named branches and historical accepted transport events only; not a live gameplay certificate.'
    messages = $rows
}
if ($OutputPath) {
    $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutputPath -Encoding utf8
}
$rows | Group-Object profile | ForEach-Object {
    [pscustomobject]@{
        profile = $_.Name
        messages = $_.Count
        typed_names = @($_.Group | Where-Object typed_name_present).Count
        runtime_names = @($_.Group | Where-Object runtime_name_present).Count
        observed_inbound = @($_.Group | Where-Object { $_.host_inbound + $_.client_inbound -gt 0 }).Count
        observed_outbound = @($_.Group | Where-Object { $_.host_outbound + $_.client_outbound -gt 0 }).Count
    }
}
