Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $root '..\..')
$srcPath = Join-Path $repo 'assets\noble\noble-icon.png'
$src = [System.Drawing.Image]::FromFile($srcPath)

function New-Canvas([int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    return @($bmp, $g)
}

# Adaptive icon foreground: transparent canvas with the emblem inside the 66% safe zone.
$size = 512
$list = New-Canvas $size $size
$bmp = $list[0]; $g = $list[1]
$inner = [int]($size * 0.70)
$off = [int](($size - $inner) / 2)
$g.DrawImage($src, $off, $off, $inner, $inner)
$g.Dispose()
$bmp.Save((Join-Path $repo 'android\app\src\main\res\drawable-nodpi\ic_launcher_foreground.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

# A standalone logo for the in-app splash (no padding, transparent).
$size = 512
$list = New-Canvas $size $size
$bmp = $list[0]; $g = $list[1]
$g.DrawImage($src, 0, 0, $size, $size)
$g.Dispose()
$bmp.Save((Join-Path $repo 'android\app\src\main\res\drawable-nodpi\noble_splash_logo.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

function New-LegacyIcon([int]$px, [bool]$round) {
    $scale = 4
    $big = $px * $scale
    $list = New-Canvas $big $big
    $bmp = $list[0]; $g = $list[1]

    # deep blue-black plate with a soft centre light
    $rect = New-Object System.Drawing.Rectangle(0, 0, $big, $big)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect,
        [System.Drawing.Color]::FromArgb(255, 10, 28, 32),
        [System.Drawing.Color]::FromArgb(255, 3, 8, 10), 90)
    if ($round) {
        $path = New-Object System.Drawing.Drawing2D.GraphicsPath
        $path.AddEllipse(0, 0, $big, $big)
        $g.SetClip($path)
    } else {
        $radius = [int]($big * 0.22)
        $path = New-Object System.Drawing.Drawing2D.GraphicsPath
        $d = $radius * 2
        $path.AddArc(0, 0, $d, $d, 180, 90)
        $path.AddArc($big - $d, 0, $d, $d, 270, 90)
        $path.AddArc($big - $d, $big - $d, $d, $d, 0, 90)
        $path.AddArc(0, $big - $d, $d, $d, 90, 90)
        $path.CloseFigure()
        $g.SetClip($path)
    }
    $g.FillRectangle($brush, $rect)
    $brush.Dispose()

    # emblem with a little breathing room
    $inner = [int]($big * 0.82)
    $off = [int](($big - $inner) / 2)
    $g.DrawImage($src, $off, $off, $inner, $inner)

    # thin gold rim
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(190, 216, 168, 78), [float]($big * 0.012))
    if ($round) {
        $g.DrawEllipse($pen, [float]($pen.Width / 2), [float]($pen.Width / 2), [float]($big - $pen.Width), [float]($big - $pen.Width))
    }
    $pen.Dispose()
    $g.ResetClip()
    $g.Dispose()

    $out = New-Object System.Drawing.Bitmap($px, $px, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $og = [System.Drawing.Graphics]::FromImage($out)
    $og.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $og.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $og.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $og.DrawImage($bmp, 0, 0, $px, $px)
    $og.Dispose()
    return $out
}

$densities = @(
    @('mdpi', 48),
    @('hdpi', 72),
    @('xhdpi', 96),
    @('xxhdpi', 144),
    @('xxxhdpi', 192)
)
foreach ($d in $densities) {
    $name = $d[0]; $px = [int]$d[1]
    $dir = Join-Path $repo "android\app\src\main\res\mipmap-$name"
    $square = New-LegacyIcon $px $false
    $square.Save((Join-Path $dir 'ic_launcher.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $square.Dispose()
    $round = New-LegacyIcon $px $true
    $round.Save((Join-Path $dir 'ic_launcher_round.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $round.Dispose()
}

$src.Dispose()
Write-Output "Wrote Android launcher icons and splash logo."
