$ErrorActionPreference = "Stop"

$inputPath = Join-Path $PSScriptRoot "..\src\VoidInjector.App\Assets\logo.png"
$outputPath = Join-Path $PSScriptRoot "..\src\VoidInjector.App\Assets\logo.ico"

if (-not (Test-Path -LiteralPath $inputPath)) {
    throw "Original logo was not found: $inputPath"
}

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class VoidIconNativeMethods
{
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool DestroyIcon(IntPtr handle);
}
"@

$source = [System.Drawing.Image]::FromFile($inputPath)
$bitmap = $null
$graphics = $null
$iconHandle = [IntPtr]::Zero
$icon = $null
$stream = $null

try {
    $size = 256
    $bitmap = New-Object System.Drawing.Bitmap(
        $size,
        $size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
    )

    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

    $scale = [Math]::Min(
        $size / [double]$source.Width,
        $size / [double]$source.Height
    )
    $width = [Math]::Max(1, [int][Math]::Round($source.Width * $scale))
    $height = [Math]::Max(1, [int][Math]::Round($source.Height * $scale))
    $x = [int](($size - $width) / 2)
    $y = [int](($size - $height) / 2)

    $graphics.DrawImage($source, $x, $y, $width, $height)

    $iconHandle = $bitmap.GetHicon()
    $icon = [System.Drawing.Icon]::FromHandle($iconHandle)
    $stream = [System.IO.File]::Create($outputPath)
    $icon.Save($stream)

    Write-Host "Generated app icon: $outputPath"
}
finally {
    if ($stream -ne $null) { $stream.Dispose() }
    if ($icon -ne $null) { $icon.Dispose() }
    if ($iconHandle -ne [IntPtr]::Zero) {
        [VoidIconNativeMethods]::DestroyIcon($iconHandle) | Out-Null
    }
    if ($graphics -ne $null) { $graphics.Dispose() }
    if ($bitmap -ne $null) { $bitmap.Dispose() }
    $source.Dispose()
}
