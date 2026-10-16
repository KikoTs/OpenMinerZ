param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$Bridge,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)
# Character rig parity against the owned original assemblies, per generation:
#   1. the importer (tools/ContentBridge) writes a private content pack under the
#      build tree - the whole pack the first time, so the avatar tests also find
#      the held-item models there, and only the character afterwards;
#   2. RigParityProbe runs the ORIGINAL engine (Avatar, AnimBlender,
#      AnimationPlayer, AnimationClip, the body's SkinnedModelEntity, every item
#      class's CreateEntity) and records its poses;
#   3. the native test replays the same calls through rig::Animator /
#      rig::CharacterPose on the imported character and compares every bone.
# Everything stays under the build tree; nothing is copied into the repository
# and the user's installed content packs are not touched.
$ErrorActionPreference = 'Stop'
foreach ($generation in @(
    @{ Id = 'classic'; Directory = $ClassicGameDirectory },
    @{ Id = 'modern'; Directory = $ModernGameDirectory }
)) {
    $derived = Join-Path $OutputDirectory ('packs\' + $generation.Id + '\DerivedData')
    [void][IO.Directory]::CreateDirectory($derived)
    if (Test-Path -LiteralPath (Join-Path $derived 'pack-manifest.txt')) {
        & $Bridge $generation.Directory $derived --character-only
    } else {
        & $Bridge $generation.Directory $derived
    }
    if ($LASTEXITCODE -ne 0) { throw ('Importing the ' + $generation.Id + ' character failed.') }

    $original = Join-Path $OutputDirectory ('original-' + $generation.Id)
    & $Probe $generation.Directory $original
    if ($LASTEXITCODE -eq 77) {
        Write-Output 'SKIP: no graphics device for the original content pipeline'
        exit 77
    }
    if ($LASTEXITCODE -ne 0) { throw ('The original rig probe failed for ' + $generation.Id) }

    & $NativeTests (Join-Path $derived 'character.omrc') (Join-Path $original 'rig-reference.bin') |
        Tee-Object -FilePath (Join-Path $OutputDirectory ($generation.Id + '-rig-parity.txt'))
    if ($LASTEXITCODE -ne 0) { throw ('The character rig differs from the original for ' + $generation.Id) }
}
