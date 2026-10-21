# Compare consecutive PNG frames of a screenshot directory: how many pixels changed (beyond a tolerance) and
# where (bounding box, in a 1/8-scale grid). Static scenes should show ~0; flicker shows up as alternating counts.
# Usage: powershell -NoProfile -File tools\frame_diff.ps1 <dir> [tolerance 0-255, default 24] [prefix frame_]
param([string]$Dir, [int]$Tol = 24, [string]$Prefix = 'frame_')
Add-Type -AssemblyName System.Drawing
$files = Get-ChildItem -Path $Dir -Filter "$Prefix*.png" | Sort-Object Name
$prev = $null; $prevName = ''
foreach ($f in $files) {
    $bmp = New-Object System.Drawing.Bitmap $f.FullName
    $w = $bmp.Width; $h = $bmp.Height
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object byte[] ($data.Stride * $h)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data); $bmp.Dispose()
    if ($prev -ne $null -and $prev.Length -eq $bytes.Length) {
        $changed = 0; $minx = $w; $miny = $h; $maxx = -1; $maxy = -1
        $grid = New-Object int[] 64
        for ($y = 0; $y -lt $h; $y += 2) {
            $row = $y * $data.Stride
            for ($x = 0; $x -lt $w; $x += 2) {
                $i = $row + $x * 4
                $d = [Math]::Abs($bytes[$i] - $prev[$i]) + [Math]::Abs($bytes[$i+1] - $prev[$i+1]) + [Math]::Abs($bytes[$i+2] - $prev[$i+2])
                if ($d -gt $Tol) {
                    $changed++
                    if ($x -lt $minx) { $minx = $x }; if ($x -gt $maxx) { $maxx = $x }
                    if ($y -lt $miny) { $miny = $y }; if ($y -gt $maxy) { $maxy = $y }
                    $grid[[Math]::Floor($y * 8 / $h) * 8 + [Math]::Floor($x * 8 / $w)]++
                }
            }
        }
        $total = [int](($w / 2) * ($h / 2))
        $pct = [Math]::Round(100.0 * $changed / $total, 2)
        $hot = ($grid | ForEach-Object -Begin { $k = 0 } -Process { [PSCustomObject]@{ k = $k; n = $_ }; $k++ } | Sort-Object n -Descending | Select-Object -First 3 | ForEach-Object { "r$([Math]::Floor($_.k / 8))c$($_.k % 8):$($_.n)" }) -join " "
        "{0} vs {1}: {2}% of sampled pixels changed (>{3}), bbox {4},{5}-{6},{7}, hottest 8x8 cells {8}" -f $prevName, $f.Name, $pct, $Tol, $minx, $miny, $maxx, $maxy, $hot
    }
    $prev = $bytes; $prevName = $f.Name
}
