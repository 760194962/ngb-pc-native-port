# Packs a release zip: dist\ngb-hurricane-pc-<version>-win32.zip with one folder, NGB-Hurricane\, holding the
# launcher (the way in: it installs the game from the player's own image and starts it), Ninja Gaiden Black
# Hurricane.exe, blackrt.dll, README.md, README.en.md and LICENSE.txt. The launcher's background art is inside its
# .exe and both programs carry the Dragon Sword icon (launcher/art: the exceptions to "no game assets", README, Legal).
# The launcher installs the game into game\ inside that folder by default, where the game program also looks when it
# is double-clicked; what the programs write stays there too (savedata\, black.ini, cache\). The script checks that
# the game program has its icon (a build made with PORT_ICON empty has none). blackrt.pdb and blackrt.map go into a
# separate -symbols zip, kept to read crash reports.
# Build first (PORT_ICON defaults to launcher/art/icon.ico):
#   cmake -S loader -B ..\build\release -A Win32
#   cmake --build ..\build\release --config RelWithDebInfo
# then: tools\make_release.ps1 -Bin ..\build\release\RelWithDebInfo
param([string]$Bin = '', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$cmake = Get-Content "$root\loader\CMakeLists.txt" -Raw
if ($cmake -notmatch 'project\(\S+ VERSION (\d+\.\d+\.\d+)') { throw 'no project VERSION in loader\CMakeLists.txt' }
$version = $Matches[1]
if ($cmake -match 'set\(NGB_VERSION_STRING "([^"]+)"\)') {
    $version = $Matches[1] -replace '\$\{PROJECT_VERSION\}', $version
}
if (-not $Bin) { $Bin = "$root\loader\build\win32\$Config" }
$Bin = (Resolve-Path $Bin).Path
$game = "$Bin\Ninja Gaiden Black Any.exe"
$launcher = "$Bin\Ninja Gaiden Black Any Launcher.exe"
foreach ($f in $game, $launcher, "$Bin\blackrt.dll") { if (-not (Test-Path $f)) { throw "missing ${f}: build first" } }

# The programs' icon (icon group 1): a build made with PORT_ICON empty would ship without one
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class ReleaseRes {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr LoadLibraryExW(string f, IntPtr h, uint flags);
  [DllImport("kernel32.dll")] public static extern IntPtr FindResourceW(IntPtr m, IntPtr name, IntPtr type);
  [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr m);
}
"@
$m = [ReleaseRes]::LoadLibraryExW($game, [IntPtr]::Zero, 0x22) # LOAD_LIBRARY_AS_DATAFILE | AS_IMAGE_RESOURCE
if ($m -eq [IntPtr]::Zero) { throw "cannot read the resources of $game" }
$icon = [ReleaseRes]::FindResourceW($m, [IntPtr]1, [IntPtr]14) # RT_GROUP_ICON 1
[ReleaseRes]::FreeLibrary($m) | Out-Null
if ($icon -eq [IntPtr]::Zero) { throw "$game has no icon (built with PORT_ICON empty): rebuild with the default" }

# Windows Defender scan of the three programs: 1.1.11's launcher was a machine-learning false positive
# (Trojan:Win32/Bearfoos.A!ml) while a second build of the same source was clean. A flagged build is not shipped:
# rebuild and pack again.
$mp = "$env:ProgramFiles\Windows Defender\MpCmdRun.exe"
if (Test-Path $mp) {
    foreach ($f in $launcher, $game, "$Bin\blackrt.dll") {
        $r = & $mp -Scan -ScanType 3 -File $f -DisableRemediation | Out-String
        if ($r -notmatch 'found no threats') { throw "Windows Defender flags ${f}: rebuild it and pack again`n$r" }
    }
    'Windows Defender: the three programs are clean'
} else { Write-Warning 'MpCmdRun.exe not found: the programs were not scanned' }

$name = "ngb-hurricane-pc-$version-win32"
$dist = "$root\dist"
New-Item -ItemType Directory -Force $dist | Out-Null

# Staged inside dist so nothing is written outside the project (C: is off limits).
$stage = "$dist\_stage-$PID"
$sym = "$stage-symbols"
$out = "$stage\NGB-Hurricane"
New-Item -ItemType Directory -Force $out, $sym | Out-Null
Copy-Item $launcher, $game, "$Bin\blackrt.dll" $out
Copy-Item "$root\README.md", "$root\README.en.md" $out
Copy-Item "$root\LICENSE" "$out\LICENSE.txt"
Copy-Item "$Bin\blackrt.pdb", "$Bin\blackrt.map" $sym

Compress-Archive -Path "$stage\*" -DestinationPath "$dist\$name.zip" -Force
Compress-Archive -Path "$sym\*" -DestinationPath "$dist\$name-symbols.zip" -Force
Remove-Item -Recurse -Force $stage, $sym

foreach ($z in "$dist\$name.zip", "$dist\$name-symbols.zip") {
    $h = (Get-FileHash $z -Algorithm SHA256).Hash.ToLower()
    '{0}  {1:N0} bytes  sha256 {2}' -f (Split-Path $z -Leaf), (Get-Item $z).Length, $h
}
