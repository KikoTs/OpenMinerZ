param(
    [string]$GameDirectory = $env:OPENMINERZ_MODERN_GAME_DIR,
    [string]$AssemblyFile = 'CastleMinerZ.exe',
    [Parameter(Mandatory = $true)] [string]$TypePattern,
    [string]$MethodPattern = '.*',
    [switch]$MembersOnly
)

if ([string]::IsNullOrWhiteSpace($GameDirectory)) {
    throw 'Pass -GameDirectory or set OPENMINERZ_MODERN_GAME_DIR.'
}

$assemblyPath = Join-Path $GameDirectory $AssemblyFile
$resolver = [ResolveEventHandler]{
    param($sender, $eventArgs)
    $name = (New-Object Reflection.AssemblyName($eventArgs.Name)).Name + '.dll'
    $candidate = Join-Path $GameDirectory $name
    if (Test-Path -LiteralPath $candidate) {
        return [Reflection.Assembly]::ReflectionOnlyLoadFrom($candidate)
    }
    try {
        return [Reflection.Assembly]::ReflectionOnlyLoad($eventArgs.Name)
    } catch {
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

$opcodes = @{}
foreach ($field in [Reflection.Emit.OpCodes].GetFields([Reflection.BindingFlags]'Public,Static')) {
    $opcode = [Reflection.Emit.OpCode]$field.GetValue($null)
    $key = ([int]$opcode.Value) -band 0xffff
    $opcodes[$key] = $opcode
}

function Read-I16([byte[]]$bytes, [ref]$offset) {
    $value = [BitConverter]::ToInt16($bytes, $offset.Value)
    $offset.Value += 2
    return $value
}

function Read-I32([byte[]]$bytes, [ref]$offset) {
    $value = [BitConverter]::ToInt32($bytes, $offset.Value)
    $offset.Value += 4
    return $value
}

function Read-I64([byte[]]$bytes, [ref]$offset) {
    $value = [BitConverter]::ToInt64($bytes, $offset.Value)
    $offset.Value += 8
    return $value
}

function Resolve-Token($module, [int]$token, [string]$kind) {
    try {
        if ($kind -eq 'string') {
            return '"' + $module.ResolveString($token).Replace('"', '\"') + '"'
        }
        if ($kind -eq 'signature') {
            return [BitConverter]::ToString($module.ResolveSignature($token))
        }
        $member = $module.ResolveMember($token)
        return $member.DeclaringType.FullName + '::' + $member.Name
    } catch {
        return ('token(0x{0:X8})' -f $token)
    }
}

function Format-Method($method) {
    try {
        $parameters = ($method.GetParameters() | ForEach-Object { $_.ParameterType.FullName + ' ' + $_.Name }) -join ', '
        return ('{0} {1}({2})' -f $method.ReturnType.FullName, $method.Name, $parameters)
    } catch {
        return $method.Name + '(...)'
    }
}

function Disassemble-Method($method) {
    $body = try { $method.GetMethodBody() } catch { $null }
    if ($null -eq $body) {
        Write-Output '  <no method body>'
        return
    }
    $bytes = $body.GetILAsByteArray()
    $offset = 0
    while ($offset -lt $bytes.Length) {
        $instructionOffset = $offset
        $first = [int]$bytes[$offset]
        $offset++
        if ($first -eq 0xfe) {
            $key = 0xfe00 -bor [int]$bytes[$offset]
            $offset++
        } else {
            $key = $first
        }
        $opcode = $opcodes[$key]
        if ($null -eq $opcode) {
            Write-Output ('  IL_{0:X4}: <unknown 0x{1:X4}>' -f $instructionOffset, $key)
            break
        }
        $operand = $null
        switch ($opcode.OperandType.ToString()) {
            'ShortInlineI' {
                $raw = [int]$bytes[$offset]
                $operand = if ($raw -ge 128) { $raw - 256 } else { $raw }
                $offset++
            }
            'InlineI' { $operand = Read-I32 $bytes ([ref]$offset) }
            'InlineI8' { $operand = Read-I64 $bytes ([ref]$offset) }
            'ShortInlineR' {
                $operand = [BitConverter]::ToSingle($bytes, $offset)
                $offset += 4
            }
            'InlineR' {
                $operand = [BitConverter]::ToDouble($bytes, $offset)
                $offset += 8
            }
            'ShortInlineVar' {
                $operand = [int]$bytes[$offset]
                $offset++
            }
            'InlineVar' { $operand = Read-I16 $bytes ([ref]$offset) }
            'ShortInlineBrTarget' {
                $raw = [int]$bytes[$offset]
                $delta = if ($raw -ge 128) { $raw - 256 } else { $raw }
                $offset++
                $operand = ('IL_{0:X4}' -f ($offset + $delta))
            }
            'InlineBrTarget' {
                $delta = Read-I32 $bytes ([ref]$offset)
                $operand = ('IL_{0:X4}' -f ($offset + $delta))
            }
            'InlineSwitch' {
                $count = Read-I32 $bytes ([ref]$offset)
                $base = $offset + 4 * $count
                $targets = @()
                for ($index = 0; $index -lt $count; ++$index) {
                    $delta = Read-I32 $bytes ([ref]$offset)
                    $targets += ('IL_{0:X4}' -f ($base + $delta))
                }
                $operand = $targets -join ', '
            }
            'InlineString' {
                $token = Read-I32 $bytes ([ref]$offset)
                $operand = Resolve-Token $method.Module $token 'string'
            }
            'InlineSig' {
                $token = Read-I32 $bytes ([ref]$offset)
                $operand = Resolve-Token $method.Module $token 'signature'
            }
            { $_ -in @('InlineField', 'InlineMethod', 'InlineTok', 'InlineType') } {
                $token = Read-I32 $bytes ([ref]$offset)
                $operand = Resolve-Token $method.Module $token 'member'
            }
        }
        if ($null -eq $operand) {
            Write-Output ('  IL_{0:X4}: {1}' -f $instructionOffset, $opcode.Name)
        } else {
            Write-Output ('  IL_{0:X4}: {1,-12} {2}' -f $instructionOffset, $opcode.Name, $operand)
        }
    }
}

$flags = [Reflection.BindingFlags]'Public,NonPublic,Static,Instance,DeclaredOnly'
foreach ($type in ($types | Where-Object FullName -Match $TypePattern | Sort-Object FullName)) {
    Write-Output ('TYPE ' + $type.FullName)
    foreach ($field in $type.GetFields($flags)) {
        $suffix = if ($field.IsLiteral) { ' = ' + $field.GetRawConstantValue() } else { '' }
        Write-Output ('  FIELD {0} {1}{2}' -f $field.FieldType.FullName, $field.Name, $suffix)
    }
    foreach ($property in $type.GetProperties($flags)) {
        Write-Output ('  PROPERTY {0} {1}' -f $property.PropertyType.FullName, $property.Name)
    }
    $methods = @($type.GetMethods($flags)) + @($type.GetConstructors($flags))
    foreach ($method in ($methods | Where-Object Name -Match $MethodPattern)) {
        Write-Output ('  METHOD ' + (Format-Method $method))
        if (-not $MembersOnly) {
            Disassemble-Method $method
        }
    }
}
