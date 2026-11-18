$ErrorActionPreference = "Stop"

$inputPath = Join-Path $PSScriptRoot "..\src\VoidInjector.App\Assets\logo.png"
$outputPath = Join-Path $PSScriptRoot "..\src\VoidInjector.App\Assets\logo.ico"

if (-not (Test-Path -LiteralPath $inputPath)) {
    throw "Original logo was not found: $inputPath"
}

Add-Type -AssemblyName System.Drawing

$source = [System.Drawing.Image]::FromFile($inputPath)
$frames = [System.Collections.Generic.List[object]]::new()
$fileStream = $null
$writer = $null

try {
    # Store several PNG-backed icon sizes in one ICO file. Windows 10/11
    # supports PNG frames in ICO files, including the 256px high-resolution frame.
    foreach ($size in @(16, 24, 32, 48, 64, 128, 256)) {
        $bitmap = [System.Drawing.Bitmap]::new(
            $size,
            $size,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
        )
        $graphics = $null
        $pngStream = [System.IO.MemoryStream]::new()

        try {
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            $graphics.Clear([System.Drawing.Color]::Transparent)
            $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

            # Fit the full original image without cropping or changing its design.
            $scale = [Math]::Min(
                $size / [double]$source.Width,
                $size / [double]$source.Height
            )
            $width = [Math]::Max(1, [int][Math]::Round($source.Width * $scale))
            $height = [Math]::Max(1, [int][Math]::Round($source.Height * $scale))
            $x = [int](($size - $width) / 2)
            $y = [int](($size - $height) / 2)

            $graphics.DrawImage($source, $x, $y, $width, $height)
            $bitmap.Save($pngStream, [System.Drawing.Imaging.ImageFormat]::Png)

            $frames.Add([pscustomobject]@{
                Size = $size
                Bytes = $pngStream.ToArray()
            })
        }
        finally {
            if ($graphics -ne $null) { $graphics.Dispose() }
            $pngStream.Dispose()
            $bitmap.Dispose()
        }
    }

    $fileStream = [System.IO.File]::Create($outputPath)
    $writer = [System.IO.BinaryWriter]::new($fileStream)

    # ICO header.
    $writer.Write([UInt16]0) # Reserved
    $writer.Write([UInt16]1) # Image type: icon
    $writer.Write([UInt16]$frames.Count)

    # Image directory. A dimension of 0 represents 256 pixels in ICO format.
    $imageOffset = 6 + (16 * $frames.Count)
    foreach ($frame in $frames) {
        $dimension = if ($frame.Size -eq 256) { 0 } else { [byte]$frame.Size }
        $writer.Write([byte]$dimension)       # Width
        $writer.Write([byte]$dimension)       # Height
        $writer.Write([byte]0)                # Palette colors
        $writer.Write([byte]0)                # Reserved
        $writer.Write([UInt16]1)              # Color planes
        $writer.Write([UInt16]32)             # Bits per pixel
        $writer.Write([UInt32]$frame.Bytes.Length)
        $writer.Write([UInt32]$imageOffset)
        $imageOffset += $frame.Bytes.Length
    }

    # PNG image data for each icon size.
    foreach ($frame in $frames) {
        $writer.Write([byte[]]$frame.Bytes)
    }

    $writer.Flush()
    Write-Host "Generated multi-resolution Windows icon from logo.png: $outputPath"
}
finally {
    if ($writer -ne $null) { $writer.Dispose() }
    elseif ($fileStream -ne $null) { $fileStream.Dispose() }
    $source.Dispose()
}
