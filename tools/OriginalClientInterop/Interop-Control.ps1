[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $Endpoint,

    [Parameter(Mandatory = $true)]
    [ValidateSet('status', 'capture', 'dump-state', 'quit')]
    [string] $Command,

    [ValidatePattern('^[A-Za-z0-9_.-]{1,92}\.bmp$')]
    [string] $CaptureName = 'barrier.bmp',

    [ValidateRange(-1000000, 1000000)]
    [int] $CenterX = 0,

    [ValidateRange(-1000000, 1000000)]
    [int] $CenterZ = 0,

    [ValidatePattern('^[A-Za-z0-9_.-]{1,91}\.json$')]
    [string] $StateName = 'original-state.json'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$endpointData = Get-Content -LiteralPath $Endpoint -Raw | ConvertFrom-Json
if ([string] $endpointData.schema -cne 'openminerz.original-endpoint.v1' -or
    [string] $endpointData.pipe -cnotmatch '^OpenMinerZ\.Interop\.[0-9]+$') {
    throw 'The original-client supervisor endpoint is malformed or unsupported.'
}

$request = switch ($Command) {
    'capture' { "capture $CaptureName" }
    'dump-state' { 'dump-state {0} {1} {2}' -f $CenterX, $CenterZ, $StateName }
    default { $Command }
}
$pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
    '.', [string] $endpointData.pipe,
    [System.IO.Pipes.PipeDirection]::InOut,
    [System.IO.Pipes.PipeOptions]::None)
try {
    $pipe.Connect(3000)
    $requestBytes = [System.Text.Encoding]::ASCII.GetBytes($request)
    $pipe.Write($requestBytes, 0, $requestBytes.Length)
    $pipe.Flush()
    $buffer = [byte[]]::new(4096)
    $responseBytes = [System.IO.MemoryStream]::new()
    try {
        while (($read = $pipe.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $responseBytes.Write($buffer, 0, $read)
            if ($responseBytes.Length -gt 16384) {
                throw 'The original-client supervisor response exceeded its bound.'
            }
        }
        $responseText = [System.Text.Encoding]::ASCII.GetString($responseBytes.ToArray())
    }
    finally {
        $responseBytes.Dispose()
    }
}
finally {
    $pipe.Dispose()
}

$response = $responseText | ConvertFrom-Json
if ([string] $response.schema -cne 'openminerz.original-control.v1' -and
    [string] $response.schema -cne 'openminerz.original-state-control.v1') {
    throw 'The original-client supervisor returned an unsupported response.'
}
$response | ConvertTo-Json -Depth 4
if (-not [bool] $response.ok) {
    exit 1
}
