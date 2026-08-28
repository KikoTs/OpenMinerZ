param(
    [string]$GameDirectory = $env:OPENMINERZ_MODERN_GAME_DIR,
    [int]$Seed = 12345
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
    $common = [Reflection.Assembly]::LoadFrom((Join-Path $GameDirectory 'DNA.Common.dll'))
    $game = [Reflection.Assembly]::LoadFrom((Join-Path $GameDirectory 'CastleMinerZ.exe'))
    $binding = [Reflection.BindingFlags]'Static,Public,NonPublic'
    $lootType = $game.GetType('DNA.CastleMinerZ.Inventory.PossibleLootType', $true)
    $mathTools = $common.GetType('DNA.MathTools', $true)
    $randomField = $mathTools.GetField('Rnd', $binding)

    function Read-Catalog([string]$fieldName) {
        @($lootType.GetField($fieldName, $binding).GetValue($null) | ForEach-Object {
            $entry = $_
            [ordered]@{
                name = [string]$entry.Name
                itemID = [int]$entry.itemID
                weight = [int]$entry.weight
                lootTier = [int]$entry.lootTier
                minWorldLevel = [int]$entry.minWorldLevel
                maxWorldLevel = [int]$entry.maxWorldLevel
                minQuantity = [int]$entry.minQuantity
                maxQuantity = [int]$entry.maxQuantity
                lootGrade = [int]$entry.lootGrade
                worth = [int]$entry.worth
                spawnType = [int]$entry.spawnType
                enemyType = [int]$entry.enemyType
                packageFlags = [int]$entry.packageFlags
            }
        })
    }

    $getRandomLoot = $lootType.GetMethod('GetRandomLoot', [Reflection.BindingFlags]'Static,Public')
    $randomIntMaximum = $mathTools.GetMethod(
        'RandomInt',
        [Reflection.BindingFlags]'Static,Public',
        $null,
        [Type[]]@([int]),
        $null)
    $randomFloatRange = $mathTools.GetMethod(
        'RandomFloat',
        [Reflection.BindingFlags]'Static,Public',
        $null,
        [Type[]]@([single], [single]),
        $null)
    function Invoke-Loot([int]$worldLevel, [int]$minTier, [int]$maxTier, [bool]$rare, [int]$count) {
        $randomField.SetValue($null, [Random]::new($Seed))
        @((0..($count - 1)) | ForEach-Object {
            $result = $getRandomLoot.Invoke($null, @($worldLevel, $minTier, $maxTier, $rare))
            [ordered]@{
                itemID = [int]$result.lootItemID
                count = [int]$result.count
                value = [int]$result.value
                spawnID = [int]$result.spawnID
            }
        })
    }

    function Invoke-ProcessedBlock([int]$worldLevel, [bool]$lucky) {
        $randomField.SetValue($null, [Random]::new($Seed))
        $preRoll = [int]$randomIntMaximum.Invoke($null, @([int]100))
        $drops = [Collections.Generic.List[object]]::new()
        function Invoke-PlacementVector {
            [single]$x = $randomFloatRange.Invoke($null, @([single]-0.5, [single]0.501))
            [single]$z = $randomFloatRange.Invoke($null, @([single]-0.5, [single]0.501))
            [single]$length = [Math]::Sqrt($x * $x + [single]0.01 + $z * $z)
            [ordered]@{
                x = [single]($x / $length * [single]1.5)
                y = [single]([single]0.1 / $length * [single]1.5)
                z = [single]($z / $length * [single]1.5)
            }
        }
        if ($lucky) {
            $rare = $getRandomLoot.Invoke($null, @($worldLevel, 4, 10, $true))
            $drops.Add([ordered]@{
                itemID = [int]$rare.lootItemID
                count = [int]$rare.count
                rare = $true
                velocity = Invoke-PlacementVector
            })
            $regularCount = 2
            $minTier = 4
            $maxTier = 10
        }
        else {
            $regularCount = 1
            $minTier = 1
            $maxTier = 5
        }
        for ($index = 0; $index -lt $regularCount; ++$index) {
            $regular = $getRandomLoot.Invoke($null, @($worldLevel, $minTier, $maxTier, $false))
            $drops.Add([ordered]@{
                itemID = [int]$regular.lootItemID
                count = [int]$regular.count
                rare = $false
                velocity = Invoke-PlacementVector
            })
        }
        [ordered]@{
            preRoll = $preRoll
            drops = @($drops)
        }
    }

    [ordered]@{
        seed = $Seed
        loot = Read-Catalog '_PossibleLootTypes'
        spawns = Read-Catalog '_PossibleSpawnTypes'
        surfaceCommon = Invoke-Loot 100 1 5 $false 16
        caveCommon = Invoke-Loot 35 1 5 $false 16
        hellCommon = Invoke-Loot -45 1 5 $false 16
        caveRare = Invoke-Loot 35 1 5 $true 16
        processedNormal = Invoke-ProcessedBlock 35 $false
        processedLucky = Invoke-ProcessedBlock 35 $true
    } | ConvertTo-Json -Depth 8
}
finally {
    [Environment]::CurrentDirectory = $originalDirectory
}
