param(
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    # Where the records go. Never inside the repository's tracked files: the
    # frames show the owner's game content.
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    # Where the probe is built. Default: <OutputDirectory>\probe.
    [string]$ProbeDirectory = ''
)
# Records the front end and the in-game menu of the owned original builds.
#
# The probe runs the SHIPPED MainMenu (over MenuBackdropScreen), GameModeMenu,
# DifficultyLevelScreen, InGameMenu, 1.6.3.8's OptionsMenu and SettingsMenu and
# 1.9.9.8's OptionsScreen offline, at five back-buffer sizes, and writes per
# build:
#
#   draws/<build>.<menu>.<W>x<H>.txt   every quad and string, in drawing order
#   frames/<build>.<menu>.<W>x<H>.png  the composed frame (for looking at)
#   fonts/<build>.<font>.png           each font's glyph sheet
#   fonts.<build>.txt                  line spacing and sample measurements
#
# OpenMinerZ's menus are its own screens set in this look (src/app_menus.inc),
# so nothing is compared line by line; this is the reference the look was
# taken from. Exits 77 where there is no Direct3D device.
$ErrorActionPreference = 'Stop'
[void][IO.Directory]::CreateDirectory($OutputDirectory)
if ($ProbeDirectory -eq '') { $ProbeDirectory = Join-Path $OutputDirectory 'probe' }

$project = Join-Path $PSScriptRoot 'OpenMinerZ.MenusParityProbe.csproj'
& dotnet build $project --configuration Release --output $ProbeDirectory --nologo --verbosity quiet
if ($LASTEXITCODE -ne 0) { throw "the menus probe did not build (exit $LASTEXITCODE)" }
$probe = Join-Path $ProbeDirectory 'OpenMinerZ.MenusParityProbe.exe'

$sizes = @(@(1280, 720), @(1920, 1080), @(2560, 1440), @(1920, 1200), @(1024, 768))
foreach ($build in @(
    @{ Id = 'classic'; Directory = $ClassicGameDirectory },
    @{ Id = 'modern'; Directory = $ModernGameDirectory }
)) {
    $root = Join-Path $OutputDirectory $build.Id
    foreach ($size in $sizes) {
        & $probe $build.Directory $root $size[0] $size[1]
        if ($LASTEXITCODE -eq 77) { Write-Output 'SKIP no graphics device for the original menus'; exit 77 }
        if ($LASTEXITCODE -ne 0) { throw "menus probe failed for $($build.Id) at $($size[0])x$($size[1]) (exit $LASTEXITCODE)" }
    }
}
Write-Output "menus recorded under $OutputDirectory"
