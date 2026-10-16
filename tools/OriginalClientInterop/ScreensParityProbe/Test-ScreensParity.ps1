param(
    [Parameter(Mandatory = $true)][string]$Probe,
    [Parameter(Mandatory = $true)][string]$NativeTests,
    [Parameter(Mandatory = $true)][string]$ClassicGameDirectory,
    [Parameter(Mandatory = $true)][string]$ModernGameDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    # Also rewrite tests/screens_parity_pinned.hpp from this recording.
    [string]$PinHeader = ''
)
# Differential test of the inventory, crate and crafting screens against the
# owned original assemblies.
#
# The probe runs the SHIPPED screens (1.6.3.8's BlockPickerScreen,
# CraftingUIScreen and CrateScreen; 1.9.9.8's CraftingScreen and CrateScreen)
# with scripted inventories at five back-buffer sizes and writes, per build:
#
#   draws/<scenario>.<W>x<H>.txt   every quad and string the screen submitted
#   frames/<scenario>.<W>x<H>.png  the composed frame (for looking at)
#   interaction.<build>.txt        the inventory after each step of every
#                                  input script (1280x720)
#   measures.<build>.<W>x<H>.txt   every SpriteFont.MeasureString it made
#
# The native test binary then replays the same scenarios and scripts through
# screens::Session and must produce the same records and the same states.
# Everything stays under the output directory; nothing is committed.
$ErrorActionPreference = 'Stop'
[void][IO.Directory]::CreateDirectory($OutputDirectory)

$sizes = @(@(1280, 720), @(1920, 1080), @(2560, 1440), @(1920, 1200), @(1024, 768))
foreach ($build in @(
    @{ Id = 'classic'; Directory = $ClassicGameDirectory },
    @{ Id = 'modern'; Directory = $ModernGameDirectory }
)) {
    $root = Join-Path $OutputDirectory $build.Id
    foreach ($stale in @('draws', 'frames')) {
        $directory = Join-Path $root $stale
        if (Test-Path $directory) { Remove-Item -Recurse -Force $directory }
    }
    foreach ($size in $sizes) {
        & $Probe $build.Directory $root $size[0] $size[1]
        if ($LASTEXITCODE -eq 77) { Write-Output 'SKIP no graphics device for the original screens'; exit 77 }
        if ($LASTEXITCODE -ne 0) { throw "screens probe failed for $($build.Id) at $($size[0])x$($size[1]) (exit $LASTEXITCODE)" }
    }
}

if ($PinHeader -ne '') {
    & $NativeTests --pin $OutputDirectory $PinHeader
    if ($LASTEXITCODE -ne 0) { throw "pinning failed (exit $LASTEXITCODE)" }
}

& $NativeTests --compare $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw "the native screens differ from the original (exit $LASTEXITCODE)" }
