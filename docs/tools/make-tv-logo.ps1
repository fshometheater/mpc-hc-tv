# Draws this build's TV logo (Options > Player > Logo, "TV"): 1280 x 720, transparent background.
# Output: src\mpc-hc\res\logo.tv.png and logo.tv-blank.png (same size, empty). Run from anywhere: powershell -File docs\tools\make-tv-logo.ps1
#
# MPC-HC copies the logo onto its black background without blending, so the soft edges are stored
# pre-blended with black (colour x alpha): exact on black, still transparent elsewhere.
# Kept dim on purpose (no pure white): it is seen in a dark room on projectors and HDR displays.

Add-Type -AssemblyName System.Drawing
$root = Split-Path (Split-Path $PSScriptRoot)
$out = Join-Path $root 'src\mpc-hc\res\logo.tv.png'

$W = 1280; $H = 720
$bmp = New-Object Drawing.Bitmap $W, $H, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'
$g.TextRenderingHint = 'AntiAliasGridFit'
$g.Clear([Drawing.Color]::Transparent)

function New-RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# The TV menus' default highlight: accent "Cyan" RGB(0, 164, 220) at brightness 100 % = 70 % of the
# colour (CTvMode::Accent) = RGB(0, 115, 154). The gradient starts a little lighter (82 %).
$accent  = [Drawing.Color]::FromArgb(255, 0, 115, 154)
$accent2 = [Drawing.Color]::FromArgb(255, 0, 134, 180)
$text    = [Drawing.Color]::FromArgb(255, 200, 204, 212)   # soft grey, not white

# Wordmark sizes first, so emblem + text can be centred as one group
$fontMain = New-Object Drawing.Font('Segoe UI Semibold', 104, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
$fontTv   = New-Object Drawing.Font('Segoe UI Semibold', 60, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
$fmt = [Drawing.StringFormat]::GenericTypographic
$mainW = $g.MeasureString('MPC-HC', $fontMain, 2000, $fmt).Width
$tvW   = $g.MeasureString('TV', $fontTv, 2000, $fmt).Width

$emW = 210; $emH = 150; $gap = 56; $pillPadX = 26; $pillH = 84; $pillGap = 28
$groupW = $emW + $gap + $mainW + $pillGap + $tvW + 2 * $pillPadX
$x0 = ($W - $groupW) / 2
$cy = $H / 2

# Emblem: a screen outline on a small stand, with a play triangle
$sx = $x0; $sy = $cy - $emH / 2 - 10
$screen = New-RoundRect $sx $sy $emW $emH 26
$grad = New-Object Drawing.Drawing2D.LinearGradientBrush((New-Object Drawing.PointF($sx, $sy)), (New-Object Drawing.PointF(($sx + $emW), ($sy + $emH))), $accent2, $accent)
$pen = New-Object Drawing.Pen($grad, 12)
$pen.LineJoin = 'Round'
$g.DrawPath($pen, $screen)
$stand = New-RoundRect ($sx + $emW / 2 - 46) ($sy + $emH + 18) 92 12 6
$g.FillPath($grad, $stand)
$tx = $sx + $emW / 2 - 22; $ty = $sy + $emH / 2
$tri = New-Object Drawing.Drawing2D.GraphicsPath
$tri.AddPolygon([Drawing.PointF[]]@(
    (New-Object Drawing.PointF($tx, ($ty - 34))),
    (New-Object Drawing.PointF(($tx + 58), $ty)),
    (New-Object Drawing.PointF($tx, ($ty + 34)))))
$g.FillPath($grad, $tri)

# Wordmark "MPC-HC", then "TV" in an accent pill
$textX = $sx + $emW + $gap
$mainH = $fontMain.GetHeight($g)
$brushText = New-Object Drawing.SolidBrush $text
$g.DrawString('MPC-HC', $fontMain, $brushText, $textX, ($cy - $mainH / 2), $fmt)
$px = $textX + $mainW + $pillGap; $py = $cy - $pillH / 2
$pill = New-RoundRect $px $py ($tvW + 2 * $pillPadX) $pillH ($pillH / 2)
$g.FillPath($grad, $pill)
$tvH = $fontTv.GetHeight($g)
$brushTv = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 236, 240, 248))
$g.DrawString('TV', $fontTv, $brushTv, ($px + $pillPadX), ($cy - $tvH / 2 + 2), $fmt)
$g.Dispose()

# Pre-blend the edges with black (straight colour x alpha), see the note at the top
$rect = New-Object Drawing.Rectangle 0, 0, $W, $H
$data = $bmp.LockBits($rect, 'ReadWrite', [Drawing.Imaging.PixelFormat]::Format32bppArgb)
$bytes = New-Object byte[] ($data.Stride * $H)
[Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
for ($i = 0; $i -lt $bytes.Length; $i += 4) {
    $a = $bytes[$i + 3]
    if ($a -lt 255) {
        $bytes[$i]     = [byte][Math]::Round($bytes[$i] * $a / 255)
        $bytes[$i + 1] = [byte][Math]::Round($bytes[$i + 1] * $a / 255)
        $bytes[$i + 2] = [byte][Math]::Round($bytes[$i + 2] * $a / 255)
    }
}
[Runtime.InteropServices.Marshal]::Copy($bytes, 0, $data.Scan0, $bytes.Length)
$bmp.UnlockBits($data)
$bmp.Save($out, [Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved $out"

# The blank one: same size, nothing drawn (keeps the window size, unlike MPC-HC's 1 x 1 "no logo")
$outBlank = Join-Path $root 'src\mpc-hc\res\logo.tv-blank.png'
$blank = New-Object Drawing.Bitmap $W, $H, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
$gb = [Drawing.Graphics]::FromImage($blank)
$gb.Clear([Drawing.Color]::Transparent)
$gb.Dispose()
$blank.Save($outBlank, [Drawing.Imaging.ImageFormat]::Png)
$blank.Dispose()
"saved $outBlank"
