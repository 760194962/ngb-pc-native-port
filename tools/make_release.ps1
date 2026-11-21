# Packs a release zip: dist\ngb-hurricane-pc-<version>-win32.zip with one folder, NGB-Hurricane\, holding the
# launcher (Ninja Gaiden Black&2004 PC Launcher.exe, the way in: it installs the game from the player's own image and
# starts it), the game programs Ninja Gaiden Black.exe and Ninja Gaiden.exe (2004), blackrt.dll, the Chinese and English
# smoothness guides,
# and LICENSE.txt. The launcher's background art is inside its
# .exe and both programs carry the Dragon Sword icon (launcher/art: the exceptions to "no game assets", README, Legal).
# The launcher installs the game into game\ inside that folder by default, where the game program also looks when it
# is double-clicked; what the programs write stays there too (savedata\, black.ini, cache\). The script checks that
# the game program has its icon (a build made with PORT_ICON empty has none). blackrt.pdb and blackrt.map go into a
# separate -symbols zip, kept to read crash reports. seed\shader_seed.bin (tools\make_shader_seed.py) goes beside the
# game program: the shaders made at the first start instead of as each area first shows them; check it before a
# release with tools\make_shader_seed.py --check <a test run's cache\shaders>. seed\texture_pool.txt
# (tools\make_texture_seed.py) goes there too: the textures made ahead by shape (d3d\texture.cpp). So does
# seed\draw_states.bin (tools\make_draw_seed.py): the draw states drawn once at start for the driver (d3d\drawwarm.cpp).
# And seed\vertex_programs.bin (tools\make_vsh_seed.py): where the game's vertex programs are in default.xbe, compiled
# at a first start from the player's copy (d3d\vshader.cpp); it holds no game code.
# And seed\cpu_textures.bin (tools\make_cputex_seed.py): the ripple textures the game computes on the CPU, as
# it computes them (ngb_cpu_textures.cpp), so a first start does not (data, no code; the user, 2026-11-16).
# Build first (PORT_ICON defaults to launcher/art/icon.ico):
#   cmake -S loader -B ..\build\release -A Win32
#   cmake --build ..\build\release --config RelWithDebInfo
# then: tools\make_release.ps1 -Bin ..\build\release\RelWithDebInfo
param([string]$Bin = '', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$cmake = Get-Content "$root\loader\CMakeLists.txt" -Raw
if ($cmake -notmatch 'project\(\S+ VERSION (\d+\.\d+\.\d+(\.\d+)?)') { throw 'no project VERSION in loader\CMakeLists.txt' }
$version = $Matches[1]
if ($cmake -match 'set\(NGB_VERSION_STRING "([^"]+)"\)') {
    $version = $Matches[1] -replace '\$\{PROJECT_VERSION\}', $version
}
if (-not $Bin) { $Bin = "$root\loader\build\win32\$Config" }
$Bin = (Resolve-Path $Bin).Path
# The game programs, one per game (2026-11-18; before: Ninja Gaiden Black Any.exe), and the launcher
$games = @("$Bin\Ninja Gaiden Black.exe", "$Bin\Ninja Gaiden.exe")
$launcher = "$Bin\Ninja Gaiden Black&2004 PC Launcher.exe"
foreach ($f in $games + @($launcher, "$Bin\blackrt.dll")) { if (-not (Test-Path -LiteralPath $f)) { throw "missing ${f}: build first" } }
$seed = "$root\seed\shader_seed.bin"
if (-not (Test-Path $seed)) { throw "missing ${seed}: tools\make_shader_seed.py makes it" }
$texseed = "$root\seed\texture_pool.txt"
if (-not (Test-Path $texseed)) { throw "missing ${texseed}: tools\make_texture_seed.py makes it" }
$drawseed = "$root\seed\draw_states.bin"
if (-not (Test-Path $drawseed)) { throw "missing ${drawseed}: tools\make_draw_seed.py makes it" }
$vshseed = "$root\seed\vertex_programs.bin"
if (-not (Test-Path $vshseed)) { throw "missing ${vshseed}: tools\make_vsh_seed.py makes it" }
$cputexseed = "$root\seed\cpu_textures.bin"
if (-not (Test-Path $cputexseed)) { throw "missing ${cputexseed}: tools\make_cputex_seed.py makes it" }

# The programs' icon (icon group 1): a build made with PORT_ICON empty would ship without one
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class ReleaseRes {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern IntPtr LoadLibraryExW(string f, IntPtr h, uint flags);
  [DllImport("kernel32.dll")] public static extern IntPtr FindResourceW(IntPtr m, IntPtr name, IntPtr type);
  [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr m);
}
"@
foreach ($game in $games) {
    $m = [ReleaseRes]::LoadLibraryExW($game, [IntPtr]::Zero, 0x22) # LOAD_LIBRARY_AS_DATAFILE | AS_IMAGE_RESOURCE
    if ($m -eq [IntPtr]::Zero) { throw "cannot read the resources of $game" }
    $icon = [ReleaseRes]::FindResourceW($m, [IntPtr]1, [IntPtr]14) # RT_GROUP_ICON 1
    [ReleaseRes]::FreeLibrary($m) | Out-Null
    if ($icon -eq [IntPtr]::Zero) { throw "$game has no icon (built with PORT_ICON empty): rebuild with the default" }
}

# Windows Defender scan of the four programs: 1.1.11's launcher was a machine-learning false positive
# (Trojan:Win32/Bearfoos.A!ml) while a second build of the same source was clean. A flagged build is not shipped:
# rebuild and pack again.
$mp = "$env:ProgramFiles\Windows Defender\MpCmdRun.exe"
# Defender turned off (another antivirus, or by the user): MpCmdRun fails with "Product/Feature disabled", which is not
# a finding; the scan is skipped with a warning (scan the zip with the antivirus in use instead)
$av = Get-MpComputerStatus -ErrorAction SilentlyContinue
if ((Test-Path $mp) -and $av -and -not $av.AntivirusEnabled) {
    Write-Warning 'Windows Defender is turned off on this PC: the programs were not scanned'
} elseif (Test-Path $mp) {
    foreach ($f in @($launcher) + $games + @("$Bin\blackrt.dll")) {
        $r = & $mp -Scan -ScanType 3 -File $f -DisableRemediation | Out-String
        if ($r -notmatch 'found no threats') { throw "Windows Defender flags ${f}: rebuild it and pack again`n$r" }
    }
    'Windows Defender: the four programs are clean'
} else { Write-Warning 'MpCmdRun.exe not found: the programs were not scanned' }

$name = "ngb-hurricane-pc-$version-win32"
$dist = "$root\dist"
New-Item -ItemType Directory -Force $dist | Out-Null

# Staged inside dist so nothing is written outside the project (C: is off limits).
$stage = "$dist\_stage-$PID"
$sym = "$stage-symbols"
$out = "$stage\NGB-Hurricane"
New-Item -ItemType Directory -Force $out, $sym | Out-Null
Copy-Item -LiteralPath (@($launcher) + $games + @("$Bin\blackrt.dll", $seed, $texseed, $drawseed, $vshseed, $cputexseed)) $out
Copy-Item "$root\project\docs\18_Smoothness_Settings_CN.md" "$out\Smoothness-Settings-CN.md"
Copy-Item "$root\project\docs\18_Smoothness_Settings_EN.md" "$out\Smoothness-Settings-EN.md"
Copy-Item "$root\LICENSE" "$out\LICENSE.txt"
Copy-Item "$Bin\blackrt.pdb", "$Bin\blackrt.map" $sym

Compress-Archive -Path "$stage\*" -DestinationPath "$dist\$name.zip" -Force
Compress-Archive -Path "$sym\*" -DestinationPath "$dist\$name-symbols.zip" -Force
Remove-Item -Recurse -Force $stage, $sym

foreach ($z in "$dist\$name.zip", "$dist\$name-symbols.zip") {
    $h = (Get-FileHash $z -Algorithm SHA256).Hash.ToLower()
    '{0}  {1:N0} bytes  sha256 {2}' -f (Split-Path $z -Leaf), (Get-Item $z).Length, $h
}
