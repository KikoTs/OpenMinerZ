[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $Left,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $Right,

    [ValidateRange(0.0, 10.0)]
    [double] $PositionTolerance = 0.35,

    [ValidateRange(0.0, 3.141593)]
    [double] $AngleTolerance = 0.20,

    [switch] $AllowOffline,

    [switch] $AllowPartialRegion,

    [switch] $AllowRegionOnlyEdits,

    [string] $OutFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-State([string] $Path) {
    $state = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($state.schema -ne 'openminerz.interop-state.v1') {
        throw "Unsupported interop-state schema in $Path"
    }
    if ($null -eq $state.region -or $null -eq $state.players) {
        throw "Incomplete interop-state document in $Path"
    }
    return $state
}

function Add-Mismatch([System.Collections.Generic.List[string]] $Mismatches,
                      [string] $Code) {
    if (-not $Mismatches.Contains($Code)) {
        $Mismatches.Add($Code)
    }
}

function Compare-Exact($LeftValue, $RightValue, [string] $Code,
                       [System.Collections.Generic.List[string]] $Mismatches) {
    if ([string] $LeftValue -cne [string] $RightValue) {
        Add-Mismatch $Mismatches $Code
    }
}

function Angle-Distance([double] $LeftAngle, [double] $RightAngle) {
    $difference = $LeftAngle - $RightAngle
    return [Math]::Abs([Math]::Atan2([Math]::Sin($difference), [Math]::Cos($difference)))
}

$leftState = Read-State $Left
$rightState = Read-State $Right
$mismatches = [System.Collections.Generic.List[string]]::new()

if (-not $AllowOffline) {
    $roles = @([string] $leftState.role, [string] $rightState.role) | Sort-Object
    if ($roles.Count -ne 2 -or $roles[0] -ne 'client' -or $roles[1] -ne 'host') {
        Add-Mismatch $mismatches 'RolePairNotHostClient'
    }
    foreach ($state in @($leftState, $rightState)) {
        if ([string] $state.role -ceq 'host' -and [string] $state.phase -cne 'hosting') {
            Add-Mismatch $mismatches 'HostNotAtReadyBarrier'
        }
        if ([string] $state.role -ceq 'client' -and [string] $state.phase -cne 'connected') {
            Add-Mismatch $mismatches 'ClientNotAtReadyBarrier'
        }
    }
}

if (-not $AllowPartialRegion) {
    foreach ($state in @($leftState, $rightState)) {
        if (([long] $state.region.max_x - [long] $state.region.min_x) -ne 32 -or
            ([long] $state.region.max_z - [long] $state.region.min_z) -ne 32) {
            Add-Mismatch $mismatches 'RegionExtentNot33x33'
        }
    }
}

Compare-Exact $leftState.protocol $rightState.protocol 'ProtocolMismatch' $mismatches
Compare-Exact $leftState.world_seed $rightState.world_seed 'WorldSeedMismatch' $mismatches
Compare-Exact $leftState.region.min_x $rightState.region.min_x 'RegionMinXMismatch' $mismatches
Compare-Exact $leftState.region.max_x $rightState.region.max_x 'RegionMaxXMismatch' $mismatches
Compare-Exact $leftState.region.min_z $rightState.region.min_z 'RegionMinZMismatch' $mismatches
Compare-Exact $leftState.region.max_z $rightState.region.max_z 'RegionMaxZMismatch' $mismatches
Compare-Exact $leftState.region.hash $rightState.region.hash 'RegionHashMismatch' $mismatches
Compare-Exact $leftState.inventory_hash $rightState.inventory_hash 'InventoryHashMismatch' $mismatches
$globalEditEvidence = $null -ne $leftState.edit_hash -and $null -ne $rightState.edit_hash -and
                      $null -ne $leftState.edit_count -and $null -ne $rightState.edit_count
if ($globalEditEvidence) {
    Compare-Exact $leftState.edit_hash $rightState.edit_hash 'EditHashMismatch' $mismatches
    Compare-Exact $leftState.edit_count $rightState.edit_count 'EditCountMismatch' $mismatches
}
elseif (-not $AllowRegionOnlyEdits) {
    Add-Mismatch $mismatches 'GlobalEditEvidenceUnavailable'
}
Compare-Exact $leftState.remote_players $rightState.remote_players 'RemotePlayerCountMismatch' $mismatches
Compare-Exact $leftState.enemies $rightState.enemies 'EnemyCountMismatch' $mismatches
Compare-Exact $leftState.pickups $rightState.pickups 'PickupCountMismatch' $mismatches
Compare-Exact $leftState.dragon_active $rightState.dragon_active 'DragonStateMismatch' $mismatches

$leftPlayers = @{}
foreach ($player in @($leftState.players)) {
    $gid = [int] $player.gid
    if ($leftPlayers.ContainsKey($gid)) {
        Add-Mismatch $mismatches 'DuplicateLeftPlayerGid'
    }
    $leftPlayers[$gid] = $player
}
$rightPlayers = @{}
foreach ($player in @($rightState.players)) {
    $gid = [int] $player.gid
    if ($rightPlayers.ContainsKey($gid)) {
        Add-Mismatch $mismatches 'DuplicateRightPlayerGid'
    }
    $rightPlayers[$gid] = $player
}

$allGids = @($leftPlayers.Keys + $rightPlayers.Keys | Sort-Object -Unique)
foreach ($gid in $allGids) {
    if (-not $leftPlayers.ContainsKey($gid) -or -not $rightPlayers.ContainsKey($gid)) {
        Add-Mismatch $mismatches "PlayerGid${gid}Missing"
        continue
    }
    $leftPlayer = $leftPlayers[$gid]
    $rightPlayer = $rightPlayers[$gid]
    $dx = [double] $leftPlayer.x - [double] $rightPlayer.x
    $dy = [double] $leftPlayer.y - [double] $rightPlayer.y
    $dz = [double] $leftPlayer.z - [double] $rightPlayer.z
    if ([Math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz) -gt $PositionTolerance) {
        Add-Mismatch $mismatches "PlayerGid${gid}PositionMismatch"
    }
    if ((Angle-Distance ([double] $leftPlayer.yaw) ([double] $rightPlayer.yaw)) -gt
        $AngleTolerance) {
        Add-Mismatch $mismatches "PlayerGid${gid}YawMismatch"
    }
    if ([Math]::Abs([double] $leftPlayer.pitch - [double] $rightPlayer.pitch) -gt
        $AngleTolerance) {
        Add-Mismatch $mismatches "PlayerGid${gid}PitchMismatch"
    }
    Compare-Exact $leftPlayer.dead $rightPlayer.dead "PlayerGid${gid}DeathMismatch" $mismatches
    if ($null -ne $leftPlayer.health -and $null -ne $rightPlayer.health -and
        [Math]::Abs([double] $leftPlayer.health - [double] $rightPlayer.health) -gt 0.01) {
        Add-Mismatch $mismatches "PlayerGid${gid}HealthMismatch"
    }
}

$certificate = [ordered] @{
    schema = 'openminerz.interop-compare.v1'
    compatible = $mismatches.Count -eq 0
    left_role = [string] $leftState.role
    right_role = [string] $rightState.role
    protocol = [int] $leftState.protocol
    world_seed = [int] $leftState.world_seed
    region_hash = [string] $leftState.region.hash
    edit_hash = if ($globalEditEvidence) { [string] $leftState.edit_hash } else { $null }
    global_edit_evidence = $globalEditEvidence
    region_only_edits_allowed = [bool] $AllowRegionOnlyEdits
    compared_player_gids = @($allGids)
    position_tolerance = $PositionTolerance
    angle_tolerance = $AngleTolerance
    exact_region_required = -not [bool] $AllowPartialRegion
    mismatches = @($mismatches)
}
$json = $certificate | ConvertTo-Json -Depth 5
if (-not [string]::IsNullOrWhiteSpace($OutFile)) {
    $parent = Split-Path -Parent $OutFile
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        [System.IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    Set-Content -LiteralPath $OutFile -Value $json -Encoding utf8NoBOM
}
$json
if ($mismatches.Count -eq 0) {
    exit 0
}
exit 1
