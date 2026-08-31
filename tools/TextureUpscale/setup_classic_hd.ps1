param(
    [string]$SourcePack,
    [string]$OutputPack,
    [int]$Gpu = 0,
    [switch]$KeepWork
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..\..')).Path
$toolRoot = Join-Path $repoRoot 'out\tools\classic-hd'
$downloadRoot = Join-Path $toolRoot 'downloads'
$archivePath = Join-Path $downloadRoot 'realesrgan-ncnn-vulkan-20220424-windows.zip'
$upscalerRoot = Join-Path $toolRoot 'realesrgan-ncnn-vulkan'
$upscalerExe = Join-Path $upscalerRoot 'realesrgan-ncnn-vulkan.exe'
$venvRoot = Join-Path $toolRoot 'venv'
$venvPython = Join-Path $venvRoot 'Scripts\python.exe'
$downloadUri = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip'
$expectedArchiveHash = 'ABC02804E17982A3BE33675E4D471E91EA374E65B70167ABC09E31ACB412802D'

if ([string]::IsNullOrWhiteSpace($SourcePack)) {
    $SourcePack = Join-Path $repoRoot 'out\build\native-release\content-packs\classic'
}
if ([string]::IsNullOrWhiteSpace($OutputPack)) {
    if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        throw 'LOCALAPPDATA is unavailable; pass -OutputPack explicitly.'
    }
    $OutputPack = Join-Path $env:LOCALAPPDATA 'OpenMinerZ\ContentPacks\classic-hd'
}

$sourceAtlas = Join-Path $SourcePack 'DerivedData\terrain_diffuse.bmp'
if (-not (Test-Path -LiteralPath $sourceAtlas -PathType Leaf)) {
    throw "Classic content is not imported: $sourceAtlas"
}

New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
    Write-Host 'Downloading the pinned official Real-ESRGAN Vulkan package...'
    Invoke-WebRequest -Uri $downloadUri -OutFile $archivePath
}
$actualArchiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash
if ($actualArchiveHash -ne $expectedArchiveHash) {
    throw "Real-ESRGAN archive hash mismatch. Expected $expectedArchiveHash, got $actualArchiveHash."
}

if (-not (Test-Path -LiteralPath $upscalerExe -PathType Leaf)) {
    Write-Host 'Extracting Real-ESRGAN...'
    New-Item -ItemType Directory -Force -Path $upscalerRoot | Out-Null
    Expand-Archive -LiteralPath $archivePath -DestinationPath $upscalerRoot -Force
}

if (-not (Test-Path -LiteralPath $venvPython -PathType Leaf)) {
    Write-Host 'Creating the private Python environment...'
    & py -3.12 -m venv $venvRoot
    if ($LASTEXITCODE -ne 0) {
        throw 'Python 3.12 could not create the Classic HD environment.'
    }
}

Write-Host 'Checking the image-processing dependency...'
& $venvPython -m pip install --disable-pip-version-check --quiet `
    -r (Join-Path $scriptRoot 'requirements.txt')
if ($LASTEXITCODE -ne 0) {
    throw 'Pillow installation failed.'
}

$arguments = @(
    (Join-Path $scriptRoot 'classic_hd.py'),
    '--source-pack', $SourcePack,
    '--output-pack', $OutputPack,
    '--upscaler', $upscalerExe,
    '--gpu', $Gpu.ToString()
)
if ($KeepWork) {
    $arguments += '--keep-work'
}

Write-Host 'Generating the Classic HD pack...'
& $venvPython @arguments
if ($LASTEXITCODE -ne 0) {
    throw 'Classic HD generation failed.'
}

Write-Host "Classic HD installed: $OutputPack"
Write-Host 'Select Graphics > Texture style > CLASSIC HD, then restart OpenMinerZ.'
