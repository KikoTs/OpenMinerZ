param(
    [string]$GameDirectory = $env:OPENMINERZ_MODERN_GAME_DIR
)

if ([string]::IsNullOrWhiteSpace($GameDirectory)) {
    throw 'Pass -GameDirectory or set OPENMINERZ_MODERN_GAME_DIR.'
}

$assemblyPath = Join-Path $GameDirectory 'CastleMinerZ.exe'
$resolver = [ResolveEventHandler]{
    param($sender, $eventArgs)
    $name = (New-Object Reflection.AssemblyName($eventArgs.Name)).Name + '.dll'
    $candidate = Join-Path $GameDirectory $name
    if (Test-Path -LiteralPath $candidate) {
        return [Reflection.Assembly]::ReflectionOnlyLoadFrom($candidate)
    }
    return $null
}
[AppDomain]::CurrentDomain.add_ReflectionOnlyAssemblyResolve($resolver)
$assembly = [Reflection.Assembly]::ReflectionOnlyLoadFrom($assemblyPath)
$types = try {
    $assembly.GetTypes()
} catch [Reflection.ReflectionTypeLoadException] {
    $_.Exception.Types | Where-Object { $_ -ne $null }
}
$interesting = @(
    'DNA.CastleMinerZ.Inventory.InventoryItemIDs',
    'DNA.CastleMinerZ.Inventory.LootGrade',
    'DNA.CastleMinerZ.Inventory.LootResult',
    'DNA.CastleMinerZ.Inventory.PackageBitFlags',
    'DNA.CastleMinerZ.Inventory.PossibleLootType',
    'DNA.CastleMinerZ.Inventory.PlayerInventory',
    'DNA.CastleMinerZ.Inventory.InventoryTrayManager'
)
foreach ($fullName in $interesting) {
    $type = $types | Where-Object FullName -eq $fullName | Select-Object -First 1
    if ($null -eq $type) { continue }
    Write-Output "TYPE $fullName"
    foreach ($field in $type.GetFields([Reflection.BindingFlags]'Public,NonPublic,Static,Instance')) {
        if ($field.IsLiteral) {
            Write-Output ("ENUM {0}={1}" -f $field.Name, $field.GetRawConstantValue())
        } else {
            Write-Output ("FIELD {0} {1}" -f $field.FieldType.FullName, $field.Name)
        }
    }
    foreach ($property in $type.GetProperties([Reflection.BindingFlags]'Public,NonPublic,Static,Instance')) {
        Write-Output ("PROPERTY {0} {1}" -f $property.PropertyType.FullName, $property.Name)
    }
    foreach ($method in $type.GetMethods([Reflection.BindingFlags]'Public,NonPublic,Static,Instance,DeclaredOnly')) {
        Write-Output ("METHOD {0}" -f $method.ToString())
    }
}
