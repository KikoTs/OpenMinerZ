<#
.SYNOPSIS
Regenerates the main-menu background: a still of the origin tower in low sun.

.DESCRIPTION
Every world has the same tower at its centre (X 0, Z 0). This script renders
it from a chosen camera with the Enhanced ("Rebirth") presentation and writes

    <OutputDirectory>\menu-background.png        what the menus show (16:9)
    <OutputDirectory>\menu-background-wide.png   the same camera at 32:9, for
                                                 ultrawide windows
    <OutputDirectory>\candidates\<shot>.png      every shot it was asked for
    <OutputDirectory>\candidates\<shot>-wide.png

The default output directory is %LOCALAPPDATA%\OpenMinerZ\MenuBackground, which
is where the game looks. Nothing is written into the repository: the pictures
show terrain textured from the owner's game content and stay local.

To use a different picture, run this again with -Shot <name>, or copy the two
files of any candidate over menu-background.png and menu-background-wide.png.
Deleting them brings the built-in backdrop back.

Each run uses its own throw-away settings folder, so the player's settings,
worlds and progress are never read or written.

.EXAMPLE
tools\Capture-MenuBackground.ps1
Re-takes the chosen shot and installs it.

.EXAMPLE
tools\Capture-MenuBackground.ps1 -All
Re-takes every candidate, then installs the chosen one.

.EXAMPLE
tools\Capture-MenuBackground.ps1 -Shot sunset-ridge -Executable out\build\native-dev\RelWithDebInfo\openminerz.exe
Re-takes one candidate with a particular build and installs that one.

.EXAMPLE
tools\Capture-MenuBackground.ps1 -List
#>
[CmdletBinding()]
param(
    # openminerz.exe. Default: $env:OPENMINERZ_EXE, then the most recently
    # built openminerz.exe under out\build.
    [string]$Executable = '',
    # One shot by name (see $Shots below). Default: the chosen shot.
    [string]$Shot = '',
    # Every candidate.
    [switch]$All,
    # List the shots and exit.
    [switch]$List,
    # Leave menu-background*.png as they are; only write candidates.
    [switch]$NoInstall,
    # Skip the 32:9 render.
    [switch]$NoWide,
    [string]$OutputDirectory = '',
    # Size of the finished 16:9 picture, and of the 32:9 one.
    [int]$Width = 3840,
    [int]$Height = 2160,
    [int]$WideWidth = 7680,
    [int]$WideHeight = 2160,
    # The frame is rendered this much larger and reduced, which smooths edges
    # and foliage. A render is capped at about 30 million pixels.
    [double]$Supersample = 1.5,
    # Content pack root (the folder that holds classic-hd, classic, modern).
    # Default: the installed packs under %LOCALAPPDATA%\OpenMinerZ\ContentPacks.
    [string]$ContentPacks = '',
    [ValidateSet('classic', 'classic-hd', 'modern')][string]$TextureStyle = 'classic-hd',
    # Extra lines for the run's settings file, e.g. @{ bloom = '1' }.
    [hashtable]$Settings = @{},
    # Seconds to wait for one render before giving up.
    [int]$TimeoutSeconds = 300
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot

# ---------------------------------------------------------------------- shots
# Camera is the eye position in world blocks. The tower stands at X 0, Z 0; the
# ground round it is a few blocks above Y 0 and the lantern on top is at Y 61.
# +X is east (where the sun rises), -X west (where it sets); the snow-capped
# hill is west of the tower. Yaw 0 looks along -Z and 90 along +X; positive
# pitch looks up. Time is the hour of the day. Fov is the vertical field of
# view (the game's narrowest is 50); Crop then keeps the centre 1/Crop of that
# frame, which is how a longer lens would be made. The terrain is drawn to
# about 120 blocks and is in mist well before that, so every camera is within
# 80 blocks of the tower. All shots use one seed, so they are one world.
$Seed = 1337
$Shots = [ordered]@{
    # The tower in the middle of the frame with the sun going down behind its
    # shoulder: shafts of light fan out round the spire, the snow-capped hill
    # to the west is in silhouette and mist lies in the valley. The tower
    # stands where the retail backdrops put it, between the menu and its text.
    'sunset-rays'   = @{ Camera = '60,36,2';   Yaw = 270; Pitch = 5; Time = 17.35; Fov = 66
                         Note = 'Tower centred, sun behind its shoulder, light shafts (the chosen shot)' }
    # A few minutes later and a step to the north: the sun clear of the tower
    # and lower, the whole sky orange.
    'sunset-glow'   = @{ Camera = '62,34,-3';  Yaw = 269; Pitch = 6; Time = 17.5;  Fov = 66
                         Note = 'Later and warmer: the sun beside the tower, an orange sky' }
    # From further south: the sun sets behind the hill and the tower stands to
    # the right of it, its east face in shade.
    'sunset-ridge'  = @{ Camera = '50,34,14';  Yaw = 278; Pitch = 4; Time = 17.2;  Fov = 66
                         Note = 'Sun behind the snow-capped hill, tower to its right' }
    # From the south-east an hour before sunset: the tower lit from the side,
    # the hill catching the light, mist filling the low ground to the north.
    'golden-side'   = @{ Camera = '38,36,38';  Yaw = 318; Pitch = 3; Time = 17.0;  Fov = 66
                         Note = 'Side light from the south-east, mist to the right' }
    # A longer lens from further east: the tower smaller, layers of mist.
    'misty-evening' = @{ Camera = '78,40,6';   Yaw = 272; Pitch = 0; Time = 17.1;  Fov = 55
                         Note = 'Longer lens from further away: layers of evening mist' }
    # Early morning from the north, the sun low in the east: cool light, the
    # snow on the hill just catching it.
    'cool-morning'  = @{ Camera = '0,36,-54';  Yaw = 180; Pitch = 4; Time = 6.7;   Fov = 66
                         Note = 'Cool morning light from the north' }
    # The shots below rely on far terrain (set OPENMINERZ_FAR_TERRAIN=12 or use
    # a preset that enables it): the camera is raised so the landscape runs
    # out to the mountain ring on the horizon. The tower stays within about 85
    # blocks so it is still drawn at full detail.
    # Above and east of the tower at sunset, looking down past it to the west.
    'high-sunset'   = @{ Camera = '80,72,8';   Yaw = 270; Pitch = -14; Time = 17.3; Fov = 66
                         Note = 'From above at sunset: the tower with the land stretching to the horizon' }
    # From the south-east and higher, an hour before sunset: long shadows.
    'high-golden'   = @{ Camera = '60,78,60';  Yaw = 315; Pitch = -16; Time = 17.0; Fov = 66
                         Note = 'High from the south-east: long golden shadows across the plains' }
    # Only slightly raised, so the tower still rises through the horizon.
    'vista-sunset'  = @{ Camera = '84,50,4';   Yaw = 270; Pitch = -4;  Time = 17.3; Fov = 60
                         Note = 'Raised a little: the tower against a far horizon at sunset' }
    # From the north in the morning, the sun low in the east.
    'high-morning'  = @{ Camera = '0,74,-80';  Yaw = 180; Pitch = -14; Time = 6.9;  Fov = 66
                         Note = 'From above in the morning: side light from the east' }
}
$Chosen = 'sunset-rays'

if ($List) {
    foreach ($name in $Shots.Keys) {
        $mark = if ($name -eq $Chosen) { '*' } else { ' ' }
        '{0} {1,-14} {2}' -f $mark, $name, $Shots[$name].Note
    }
    return
}

# ----------------------------------------------------------------- executable
function Resolve-Executable {
    if ($Executable -ne '') {
        if (-not (Test-Path -LiteralPath $Executable)) { throw "No such executable: $Executable" }
        return (Resolve-Path -LiteralPath $Executable).Path
    }
    if ($env:OPENMINERZ_EXE -and (Test-Path -LiteralPath $env:OPENMINERZ_EXE)) {
        return (Resolve-Path -LiteralPath $env:OPENMINERZ_EXE).Path
    }
    # Always the newest build: a fixed preference once picked a months-old
    # native-dev executable that predated the capture switches.
    $built = Get-ChildItem -Path (Join-Path $repository 'out\build') -Filter openminerz.exe -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $built) { throw 'openminerz.exe was not found; build the game or pass -Executable.' }
    return $built.FullName
}

$exe = Resolve-Executable
if ($OutputDirectory -eq '') {
    $OutputDirectory = Join-Path $env:LOCALAPPDATA 'OpenMinerZ\MenuBackground'
}
$candidates = Join-Path $OutputDirectory 'candidates'
[void][IO.Directory]::CreateDirectory($candidates)

$install = if ($Shot -ne '') { $Shot } else { $Chosen }
$names = @(if ($All) { $Shots.Keys } else { $install })
foreach ($name in $names + @($install)) {
    if (-not $Shots.Contains($name)) { throw "Unknown shot '$name'. Use -List to see them." }
}

# A settings folder of this run's own: the best picture the renderer makes,
# and nothing of the player's is touched.
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('openminerz-menu-background-' + [Guid]::NewGuid().ToString('N'))
$settingsDirectory = Join-Path $scratch 'OpenMinerZ'
[void][IO.Directory]::CreateDirectory($settingsDirectory)
$quality = [ordered]@{
    presentation      = 'rebirth'
    render_scale      = '1'
    texture_quality   = 'high'
    shadow_quality    = 'ultra'
    anti_aliasing     = 'taa'
    ambient_occlusion = '1'
    particle_quality  = 'ultra'
    tone_mapping      = 'aces'
    auto_exposure     = '1'
    color_grading     = '1'
    master_volume     = '0'
    reduced_motion    = '0'
}
foreach ($key in $Settings.Keys) { $quality[$key] = [string]$Settings[$key] }
$lines = @('# OpenMinerZ settings v1') + ($quality.GetEnumerator() | ForEach-Object { '{0}={1}' -f $_.Key, $_.Value })
[IO.File]::WriteAllLines((Join-Path $settingsDirectory 'settings.cfg'), $lines)

Add-Type -AssemblyName System.Drawing

# The middle 1/crop of a frame, reduced to the finished size.
function Convert-Frame([string]$Source, [string]$Target, [double]$Crop, [int]$FinalWidth, [int]$FinalHeight) {
    $frame = [System.Drawing.Image]::FromFile($Source)
    try {
        # A window the desktop could not give at the size asked for would have
        # another shape, and the picture would come out stretched.
        if ([Math]::Abs($frame.Width / $frame.Height - $FinalWidth / $FinalHeight) -gt 0.01) {
            throw "The frame came back as $($frame.Width)x$($frame.Height), not the shape asked for. Try -Supersample 1."
        }
        $cropWidth = [int][Math]::Round($frame.Width / $Crop)
        $cropHeight = [int][Math]::Round($frame.Height / $Crop)
        $region = New-Object System.Drawing.Rectangle (($frame.Width - $cropWidth) / 2), (($frame.Height - $cropHeight) / 2), $cropWidth, $cropHeight
        $picture = New-Object System.Drawing.Bitmap $FinalWidth, $FinalHeight, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($picture)
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
            $attributes = New-Object System.Drawing.Imaging.ImageAttributes
            # Without this the resampler fades the outermost pixels into nothing.
            $attributes.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)
            $graphics.DrawImage($frame, (New-Object System.Drawing.Rectangle 0, 0, $FinalWidth, $FinalHeight),
                                $region.X, $region.Y, $region.Width, $region.Height,
                                [System.Drawing.GraphicsUnit]::Pixel, $attributes)
            $attributes.Dispose()
            $graphics.Dispose()
            $picture.Save($Target, [System.Drawing.Imaging.ImageFormat]::Png)
        }
        finally { $picture.Dispose() }
    }
    finally { $frame.Dispose() }
}

# One render of one shot at one shape.
function Invoke-Render([string]$Name, [hashtable]$Take, [int]$FinalWidth, [int]$FinalHeight, [string]$Target) {
    $crop = if ($Take.Contains('Crop')) { [double]$Take.Crop } else { 1.0 }
    $scale = [Math]::Min($Supersample * $crop, [Math]::Sqrt(30000000.0 / ($FinalWidth * $FinalHeight)))
    $scale = [Math]::Max($scale, $crop)
    $renderWidth = [int]([Math]::Round($FinalWidth * $scale / 2) * 2)
    $renderHeight = [int]([Math]::Round($FinalHeight * $scale / 2) * 2)
    $label = [IO.Path]::GetFileNameWithoutExtension($Target)
    $frame = Join-Path $scratch ($label + '.bmp')
    $arguments = @(
        '--world', '--no-enemies', '--clean-view',
        '--size', ('{0}x{1}' -f $renderWidth, $renderHeight),
        '--seed', $Seed,
        '--camera', $Take.Camera,
        '--yaw', $Take.Yaw, '--pitch', $Take.Pitch,
        '--time', $Take.Time,
        '--camera-fov', $Take.Fov,
        '--presentation', 'rebirth',
        '--texture-style', $TextureStyle,
        '--view-distance', 32,
        '--screenshot', $frame
    ) | ForEach-Object { [string]$_ }
    Write-Host ('{0}: rendering {1}x{2} ...' -f $label, $renderWidth, $renderHeight)
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $scratch ($label + '.out.txt')) `
        -RedirectStandardError (Join-Path $scratch ($label + '.err.txt'))
    # Windows PowerShell only reports the exit code of a process whose handle
    # it took before the process ended.
    $null = $process.Handle
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        throw "$label did not finish within $TimeoutSeconds seconds."
    }
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $frame)) {
        $errors = Get-Content -LiteralPath (Join-Path $scratch ($label + '.err.txt')) -ErrorAction SilentlyContinue
        throw "$label failed (exit $($process.ExitCode)). $($errors -join ' ')"
    }
    Convert-Frame $frame $Target $crop $FinalWidth $FinalHeight
    Remove-Item -LiteralPath $frame -Force
    Write-Host ('{0}: {1}' -f $label, $Target)
}

$previousAppData = $env:APPDATA
$previousPacks = $env:OPENMINERZ_CONTENT_PACKS
try {
    $env:APPDATA = $scratch
    if ($ContentPacks -ne '') { $env:OPENMINERZ_CONTENT_PACKS = (Resolve-Path -LiteralPath $ContentPacks).Path }
    foreach ($name in $names) {
        Invoke-Render $name $Shots[$name] $Width $Height (Join-Path $candidates ($name + '.png'))
        if (-not $NoWide) {
            Invoke-Render $name $Shots[$name] $WideWidth $WideHeight (Join-Path $candidates ($name + '-wide.png'))
        }
    }
}
finally {
    $env:APPDATA = $previousAppData
    $env:OPENMINERZ_CONTENT_PACKS = $previousPacks
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

if (-not $NoInstall) {
    foreach ($kind in @('', '-wide')) {
        $source = Join-Path $candidates ($install + $kind + '.png')
        $installed = Join-Path $OutputDirectory ('menu-background' + $kind + '.png')
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination $installed -Force
            Write-Host ('menu background: {0}' -f $installed)
        }
        elseif ($kind -ne '' -and (Test-Path -LiteralPath $installed)) {
            # An old wide render of another shot must not outlive this one.
            Remove-Item -LiteralPath $installed -Force
        }
    }
}
