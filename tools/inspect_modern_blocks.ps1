param(
    [string]$GameDirectory = $env:OPENMINERZ_MODERN_GAME_DIR
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($GameDirectory)) {
    throw 'Pass -GameDirectory or set OPENMINERZ_MODERN_GAME_DIR.'
}
if ([Environment]::Is64BitProcess) {
    throw 'Run with 32-bit Windows PowerShell so the owned XNA assemblies can load.'
}

$originalDirectory = [Environment]::CurrentDirectory
[Environment]::CurrentDirectory = $GameDirectory
try {
    $game = [Reflection.Assembly]::LoadFrom((Join-Path $GameDirectory 'CastleMinerZ.exe'))
    $blockType = $game.GetType('DNA.CastleMinerZ.Terrain.BlockType', $true)
    $blockEnum = $game.GetType('DNA.CastleMinerZ.Terrain.BlockTypeEnum', $true)
    $binding = [Reflection.BindingFlags]'Static,Public,NonPublic'
    $getType = $blockType.GetMethod(
        'GetType',
        $binding,
        $null,
        [Type[]]@($blockEnum),
        $null)
    $isContainer = $blockType.GetMethod(
        'IsContainer',
        $binding,
        $null,
        [Type[]]@($blockEnum),
        $null)

    @([Enum]::GetValues($blockEnum) | ForEach-Object {
        # Windows PowerShell wraps values from an enum type loaded at runtime in
        # PSObject. Reflection needs the actual boxed enum as its argument.
        $enumValue = $_.PSObject.BaseObject
        $entry = $getType.Invoke($null, @($enumValue))
        [ordered]@{
            id = [int]$enumValue
            enum = [string]$enumValue
            name = [string]$entry.Name
            hardness = [int]$entry.Hardness
            lightTransmission = [int]$entry.LightTransmission
            selfIllumination = [int]$entry.SelfIllumination
            damageMask = [int]$entry.DamageMask
            damageTransmission = [single]$entry.DamageTransmision
            isItemEntity = [bool]$entry.IsItemEntity
            blocksPlayer = [bool]$entry.BlockPlayer
            canBeDug = [bool]$entry.CanBeDug
            canBeTouched = [bool]$entry.CanBeTouched
            canBuildOn = [bool]$entry.CanBuildOn
            isContainer = [bool]$isContainer.Invoke($null, @($enumValue))
            hasAlpha = [bool]$entry.HasAlpha
            interiorFaces = [bool]$entry.InteriorFaces
            spawnEntity = [bool]$entry.SpawnEntity
            bouncesLasers = [bool]$entry.BouncesLasers
            bounceRestitution = [single]$entry.BounceRestitution
            allowSlopes = [bool]$entry.AllowSlopes
            tileIndices = @($entry.TileIndices | ForEach-Object { [int]$_ })
        }
    }) | Sort-Object id | ConvertTo-Json -Depth 5
}
finally {
    [Environment]::CurrentDirectory = $originalDirectory
}
