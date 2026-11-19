# Generates src/app/nexplay.ico with multi-resolution PNG frames (16, 32, 48, 64, 256 px).
# Design: rounded navy squircle (#0B1220 -> #131F33) with a subtle border (#2B3B52)
# and a crisp geometric 'N' in electric blue (#3B82F6) transitioning to cyan (#22D3EE).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-RoundedRectPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2.0
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Render-IconFrame([int]$sz) {
    $bmp = New-Object System.Drawing.Bitmap($sz, $sz, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)

    $pad = [Math]::Max(1.0, $sz * 0.04)
    $boxW = $sz - 2.0 * $pad
    $radius = $sz * 0.22
    $bgPath = New-RoundedRectPath $pad $pad $boxW $boxW $radius

    $cBgTop = [System.Drawing.Color]::FromArgb(255, 19, 31, 51)     # #131F33
    $cBgBot = [System.Drawing.Color]::FromArgb(255, 11, 18, 32)     # #0B1220
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF(0, 0)),
        (New-Object System.Drawing.PointF($sz, $sz)),
        $cBgTop, $cBgBot)
    $g.FillPath($bgBrush, $bgPath)

    if ($sz -ge 24) {
        $borderPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(210, 59, 130, 246), [Math]::Max(1.0, $sz * 0.03))
        $g.DrawPath($borderPen, $bgPath)
        $borderPen.Dispose()
    }

    [float]$leftX   = $sz * 0.23
    [float]$rightX  = $sz * 0.77
    [float]$topY    = $sz * 0.21
    [float]$botY    = $sz * 0.79
    [float]$strokeW = $sz * 0.145
    [float]$rightStart = $rightX - $strokeW
    [float]$pillarH = $botY - $topY

    $cBlue = [System.Drawing.Color]::FromArgb(255, 59, 130, 246)    # #3B82F6
    $cCyan = [System.Drawing.Color]::FromArgb(255, 34, 211, 238)    # #22D3EE

    # Left vertical pillar (electric blue)
    $leftBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF($leftX, $topY)),
        (New-Object System.Drawing.PointF($leftX, $botY)),
        $cCyan, $cBlue)
    $g.FillRectangle($leftBrush, $leftX, $topY, $strokeW, $pillarH)

    # Right vertical pillar (blue -> cyan)
    $rightBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF($rightStart, $topY)),
        (New-Object System.Drawing.PointF($rightX, $botY)),
        $cCyan, $cBlue)
    $g.FillRectangle($rightBrush, $rightStart, $topY, $strokeW, $pillarH)

    # Diagonal bar connecting top-left to bottom-right (bright cyan highlight)
    $diagPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    [float]$diagW = $strokeW * 1.05
    [float]$lx2 = $leftX + $diagW
    [float]$rx2 = $rightX - $diagW
    [System.Drawing.PointF[]]$pts = @(
        (New-Object System.Drawing.PointF($leftX, $topY)),
        (New-Object System.Drawing.PointF($lx2, $topY)),
        (New-Object System.Drawing.PointF($rightX, $botY)),
        (New-Object System.Drawing.PointF($rx2, $botY))
    )
    $diagPath.AddPolygon($pts)
    $diagBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF($leftX, $topY)),
        (New-Object System.Drawing.PointF($rightX, $botY)),
        $cCyan, $cBlue)
    $g.FillPath($diagBrush, $diagPath)

    $diagBrush.Dispose()
    $diagPath.Dispose()
    $leftBrush.Dispose()
    $rightBrush.Dispose()
    $bgBrush.Dispose()
    $bgPath.Dispose()
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

$sizes = @(16, 32, 48, 64, 256)
$frames = @()
foreach ($s in $sizes) {
    $frames += ,(Render-IconFrame $s)
}

$outPath = Join-Path $PSScriptRoot "..\src\app\nexplay.ico"
$fs = [System.IO.File]::Create($outPath)
$bw = New-Object System.IO.BinaryWriter($fs)

# ICONDIR header
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$sizes.Count)

$offset = 6 + (16 * $sizes.Count)
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $data = $frames[$i]
    $dimByte = if ($s -ge 256) { [byte]0 } else { [byte]$s }
    $bw.Write($dimByte)          # width
    $bw.Write($dimByte)          # height
    $bw.Write([byte]0)           # color count
    $bw.Write([byte]0)           # reserved
    $bw.Write([uint16]1)         # color planes
    $bw.Write([uint16]32)        # bits per pixel
    $bw.Write([uint32]$data.Length)
    $bw.Write([uint32]$offset)
    $offset += $data.Length
}

for ($i = 0; $i -lt $sizes.Count; $i++) {
    $bw.Write($frames[$i])
}

$bw.Close()
$fs.Close()
Write-Host "Generated $outPath ($($sizes -join ', ') px)"
