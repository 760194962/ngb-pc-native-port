# Packs a release zip: dist\ngb-hurricane-x21-pc-<version>-win32.zip with one folder, NGB-HurricaneX21\, holding the
# launcher (the way in: it installs the game from the player's own image and starts it), Ninja Gaiden Black
# HurricaneX21.exe, blackrt.dll, README.md, README.en.md and LICENSE.txt. The launcher's background art is inside its
# .exe (launcher/art, the one exception to "no game assets": README, Legal). The launcher installs the game into
# game\x21 inside that folder by default; what the programs write stays there too (savedata\, black.ini, cache\).
# Nothing game-derived: the build must be made without PORT_ICON (the program icon comes from the player's game),
# which this script checks. blackrt.pdb and blackrt.map go into a separate -symbols zip, kept to read crash reports.
# Build first, without PORT_ICON:
#   cmake -S loader -B ..\build\release -A Win32 -DPORT_ICON=
#   cmake --build ..\build\release --config RelWithDebInfo
# then: tools\make_release.ps1 -Bin ..\build\release\RelWithDebInfo
param([string]$Bin = '', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$cmake = Get-Content "$root\loader\CMakeLists.txt" -Raw
if ($cmake -notmatch 'project\(\S+ VERSION (\d+\.\d+\.\d+)') { throw 'no project VERSION in loader\CMakeLists.txt' }
$version = $Matches[1]
if (-not $Bin) { $Bin = "$root\loader\build\win32\$Config" }
$Bin = (Resolve-Path $Bin).Path
$game = "$Bin\Ninja Gaiden Black HurricaneX21.exe"
$launcher = "$Bin\Ninja Gaiden Black HurricaneX21 Launcher.exe"
foreach ($f in $game, $launcher, "$Bin\blackrt.dll") { if (-not (Test-Path $f)) { throw "missing ${f}: build first" } }

# A build with PORT_ICON carries an icon made from the game (icon group 1 in the game program): not for release
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
if ($icon -ne [IntPtr]::Zero) { throw "$game has an icon (built with PORT_ICON, made from the game): rebuild without it" }

$name = "ngb-hurricane-x21-pc-$version-win32"
$dist = "$root\dist"
New-Item -ItemType Directory -Force $dist | Out-Null

# Staged under %TEMP% so nothing in the project folder has to be deleted.
$stage = Join-Path $env:TEMP "$name-$PID"
$sym = "$stage-symbols"
$out = "$stage\NGB-HurricaneX21"
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
