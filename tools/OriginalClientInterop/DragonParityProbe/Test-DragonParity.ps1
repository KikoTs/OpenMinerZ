param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
# Differential dragon test against the owned original assemblies.
#
# For each build it produces three pairings over the same 180-second dragon
# life (approach, strafing runs, hover volleys, loiter circuits) with every
# message delivered 6 ticks (100 ms) late:
#
#   baseline  original owner   -> original watcher
#   forward   OpenMinerZ owner -> original watcher   (we host, a stock client watches)
#   reverse   original owner   -> OpenMinerZ watcher (a stock client owns, we watch)
#
# and requires both mixed pairings to stay within the original's own
# disagreement. Traces stay under the output directory; nothing is committed.
$ErrorActionPreference = 'Stop'
[void][IO.Directory]::CreateDirectory($OutputDirectory)

function Invoke-Step([string]$What, [string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -eq 77) { Write-Output "SKIP $What"; exit 77 }
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

# presented gap, path error, fireball ticks, fireball origin, fireball velocity.
#
# The original disagrees with itself by latency * speed (5 ticks * 20 m/s =
# 1.67 blocks) and fires 5 ticks later on the watcher, from the same point.
# A mixed pairing may not present the dragon any further away than that. Its
# fireballs leave the head's MEAN position rather than the animated ponytail
# bone, which is where the origin and direction allowances come from.
$baseline = @('1.75', '0.05', '5', '0.01', '0.01')
$mixed = @('1.75', '0.05', '7', '4.0', '8')

foreach ($profile in @(
    @{ Id = 'classic'; Directory = $ClassicGameDirectory; Modern = '0' },
    @{ Id = 'modern'; Directory = $ModernGameDirectory; Modern = '1' }
)) {
    $root = Join-Path $OutputDirectory $profile.Id
    [void][IO.Directory]::CreateDirectory($root)
    $tables = Join-Path $root 'original-tables.txt'
    $originalOwner = Join-Path $root 'original-owner.trace'
    $ourOwner = Join-Path $root 'openminerz-owner.trace'
    $originalWatchesOriginal = Join-Path $root 'original-owner.original-watcher.trace'
    $weWatchOriginal = Join-Path $root 'original-owner.openminerz-watcher.trace'
    $originalWatchesUs = Join-Path $root 'openminerz-owner.original-watcher.trace'

    Invoke-Step 'original DragonType tables' $Probe @('tables', $profile.Directory, $tables)
    Invoke-Step 'DragonType constants' $NativeTests @('--tables', $tables)

    # Undead dragon (60 m/s fireballs, the one players actually fight), spawner gid 1.
    Invoke-Step 'original owner' $Probe @('owner', $profile.Directory, $originalOwner, '4', '180', '12345', '1')
    Invoke-Step 'original watcher of the original' $Probe @('replica', $profile.Directory, $originalOwner, $originalWatchesOriginal, '6', '2')
    Invoke-Step 'OpenMinerZ watcher of the original' $NativeTests @('--replica', $originalOwner, $weWatchOriginal, '6')
    Invoke-Step 'OpenMinerZ owner' $NativeTests @('--write-owner', $ourOwner, '4', '180', '88172645463325252', '0')
    Invoke-Step 'original watcher of OpenMinerZ' $Probe @('replica', $profile.Directory, $ourOwner, $originalWatchesUs, '6', '2')

    # A short hand-written stream ending in a KillDragonMessage: where the
    # original watcher and ours put the dragon, its quick shot, and the corpse
    # during the ballistic first reaction of the death fall (the collision
    # phase that follows needs BlockTerrain and is not run by the probe).
    $golden = Join-Path $root 'golden.trace'
    $originalGolden = Join-Path $root 'golden.original-watcher.trace'
    $ourGolden = Join-Path $root 'golden.openminerz-watcher.trace'
    Invoke-Step 'golden stream' $NativeTests @('--write-golden', $golden)
    Invoke-Step 'original watcher of the golden stream' $Probe @('replica', $profile.Directory, $golden, $originalGolden, '0', '2')
    Invoke-Step 'OpenMinerZ watcher of the golden stream' $NativeTests @('--replica', $golden, $ourGolden, '0')
    Write-Output "[$($profile.Id)] same stream, kill and death fall: original watcher vs OpenMinerZ watcher"
    Invoke-Step 'death fall comparison' $NativeTests @('--compare', $originalGolden, $ourGolden, '0.05', '0.05', '1', '0.5', '1.5')

    # The brain itself: the original DragonEntity (all seven states, its timers,
    # its use of MathTools.Rnd and its send path) beside world/dragon_brain.cpp
    # on the same random seed. Every tick must be in the same state with the
    # same animation and shot bookkeeping, and every message must leave on the
    # same tick with the same kind, index and target; positions and waypoint
    # fields may differ only by float noise.
    $ourBrain = Join-Path $root 'openminerz-brain.trace'
    Invoke-Step 'OpenMinerZ brain' $NativeTests @('--write-retail-owner', $ourBrain, '4', '180', '12345', '1', $profile.Modern)
    Write-Output "[$($profile.Id)] brain: original DragonEntity vs OpenMinerZ, undead, seed 12345"
    Invoke-Step 'brain comparison' $NativeTests @('--compare-brain', $originalOwner, $ourBrain, '0.01')
    $originalFire = Join-Path $root 'original-owner-fire.trace'
    $ourFire = Join-Path $root 'openminerz-brain-fire.trace'
    Invoke-Step 'original owner (fire)' $Probe @('owner', $profile.Directory, $originalFire, '0', '240', '777', '3')
    Invoke-Step 'OpenMinerZ brain (fire)' $NativeTests @('--write-retail-owner', $ourFire, '0', '240', '777', '3', $profile.Modern)
    Write-Output "[$($profile.Id)] brain: original DragonEntity vs OpenMinerZ, fire, seed 777"
    Invoke-Step 'brain comparison (fire)' $NativeTests @('--compare-brain', $originalFire, $ourFire, '2.5', '0.03')

    Write-Output "[$($profile.Id)] baseline: original owner -> original watcher"
    Invoke-Step 'baseline comparison' $NativeTests (@('--compare', $originalOwner, $originalWatchesOriginal) + $baseline)
    Write-Output "[$($profile.Id)] OpenMinerZ owner -> original watcher"
    Invoke-Step 'forward comparison' $NativeTests (@('--compare', $ourOwner, $originalWatchesUs) + $mixed)
    Write-Output "[$($profile.Id)] original owner -> OpenMinerZ watcher"
    Invoke-Step 'reverse comparison' $NativeTests (@('--compare', $originalOwner, $weWatchOriginal) + $mixed)
}
Write-Output 'PASS dragon parity against the original assemblies'
