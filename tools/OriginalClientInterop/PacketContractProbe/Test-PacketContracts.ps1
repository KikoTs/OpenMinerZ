param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
[void][IO.Directory]::CreateDirectory($OutputDirectory)
& $NativeTests --write-fixtures $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw 'Native packet fixture export failed.' }
foreach ($profile in @(
    @{ Id = '3'; Directory = $ClassicGameDirectory },
    @{ Id = '4'; Directory = $ModernGameDirectory }
)) {
    & $Probe $profile.Directory (Join-Path $OutputDirectory $profile.Id) |
        Tee-Object -FilePath (Join-Path $OutputDirectory ($profile.Id + '-original-parity.txt'))
    if ($LASTEXITCODE -ne 0) { throw ('Original serializer parity failed for protocol ' + $profile.Id) }
}
