# Builds every generated asset into dist/CausticLight and runs the offline checks.
#   pwsh tools/build-all.ps1            full build
#   pwsh tools/build-all.ps1 -SkipAtlas reuse work/*.png (NIF and DDS only)
param([switch]$SkipAtlas)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
# Tool paths: texconv (DirectXTex) and Blender from PATH, or set them in tools/local-paths.ps1 (not in git):
#   $texconv = 'C:\path\to\texconv.exe'; $blender = 'C:\path\to\blender.exe'; $validate = 'C:\path\to\blender-nif-validate.py'
$texconv = 'texconv.exe'
$blender = 'blender.exe'
$validate = $null   # optional: a Blender script that imports the NIF to check it
$local = Join-Path $PSScriptRoot 'local-paths.ps1'
if (Test-Path $local) { . $local }
$work = Join-Path $root 'work'
$texOut = Join-Path $root 'dist\CausticLight\textures\CausticLight'
New-Item -ItemType Directory -Force $work, $texOut, (Join-Path $work 'dds') | Out-Null

# the three water states: wave height, short-wave (turbulence) scale, fastest wave's turns per loop,
# and focus distance (chosen by eye: calm and normal as a clean net, rough as broken light).
# Calm and normal gather the waves in a 70 degree fan with a counter-running copy of each wave: 30 real videos showed
# mostly stripes and bands that change shape in place rather than sliding (Muse analysis, 2026-09-28).
# RGB: the same water as a mirror (the walls): light reflected onto screens 0.5, 1.5 and 4 m above it, from a sea of
# its own at the RMS slope each layer stands for (AgitationFromSlope: calm 3, normal 8.5, rough 14 deg). Calm and
# normal are long waves (sheltered water: the broad bands of the Cycles reference, fitted to its cv and lit share);
# rough keeps the short chop, which washes out on a far wall.
$mirror = @('--mirror=0.5,1.5,4')
$sets = [ordered]@{
  calm   = @('--A=0.75', '--T=0.5', '--J=5', '--D=2.8', '--spread=70', '--stand=1') + $mirror + @('--mA=0.153', '--mT=1', '--mnw=6',  '--mlmax=2.4', '--mlratio=0.33', '--mknee=3')
  normal = @('--A=1',    '--T=1',   '--J=6', '--D=2.0', '--spread=70', '--stand=1') + $mirror + @('--mA=0.5',   '--mT=1', '--mnw=10', '--mlmax=2.4', '--mlratio=0.2',  '--mknee=2')
  rough  = @('--A=1.0',  '--T=4.5', '--J=8', '--D=1.4') + $mirror + @('--mA=1.13')
}
# 2.4 m tile (168 units) at 248 px: 2048 atlas. 4096 was tried and looked no better; it cost 4x the memory.
$common = @('--L=2.4')

foreach ($name in $sets.Keys) {
  $p = $sets[$name]
  $check = node tools/gen.cjs --mode=check --S=128 @common @p | ConvertFrom-Json
  if ($check.quadrantMaxDiff -gt 1e-9 -or $check.loopMaxDiff -gt 1e-9) { throw "$name does not tile or loop: $($check | ConvertTo-Json -Compress)" }
  Write-Host "$name tiles and loops (tile diff $($check.quadrantMaxDiff), loop diff $($check.loopMaxDiff))"
  $png = Join-Path $work "$name.png"
  if (-not $SkipAtlas) { node tools/gen.cjs --mode=atlas --S=248 --G=4 --F=64 @common @p "--out=$png" | Select-Object -Last 1 }
  node tools/check-atlas.cjs $png 4 | Select-Object -Last 1
  if ($LASTEXITCODE) { throw "$name atlas seams" }
  # two files, so BC7 spends each block on one pattern (sharing a block, the floor's alpha lost up to 90/255 on rough):
  # <name>.dds the floor's refraction under white RGB (as before the reflection channels), refl_<name>.dds the walls'
  $split = Join-Path $work 'split'
  New-Item -ItemType Directory -Force $split | Out-Null
  node tools/split-atlas.cjs $png (Join-Path $split "$name.png") (Join-Path $split "refl_$name.png") | Out-Null
  if ($LASTEXITCODE) { throw "split failed on $name" }
  foreach ($f in @($name, "refl_$name")) {
    # -sepalpha: RGB mips filtered on their own (without it texconv weights RGB by alpha)
    & $texconv -nologo -y -f BC7_UNORM -m 6 -sepalpha -o (Join-Path $work 'dds') (Join-Path $split "$f.png") | Out-Null
    if ($LASTEXITCODE) { throw "texconv failed on $f" }
    Copy-Item (Join-Path $work "dds\$f.DDS") (Join-Path $texOut "$f.dds") -Force
  }
}

node tools/build-esp.cjs
if ($LASTEXITCODE) { throw 'ESP build failed' }
node tools/build-empty.cjs   # retired panel model, kept so old saves load (earlier approaches: tools/retired/)
if ($LASTEXITCODE) { throw 'NIF build failed' }
$nif = Join-Path $root 'dist\CausticLight\meshes\CausticLight\causticpanel.nif'
if ($validate) {
  $log = & $blender --background --python $validate -- $nif 2>&1
  if ($LASTEXITCODE) { $log | Select-Object -Last 20; throw 'Blender could not import the NIF' }
  $log | Where-Object { $_ -match 'objects:|VALIDATION' }
}
# the SKSE plugin and its default settings, so dist/CausticLight is the whole mod
$skse = Join-Path $root 'dist\CausticLight\SKSE\Plugins'
New-Item -ItemType Directory -Force (Join-Path $skse 'CausticLight') | Out-Null
Copy-Item (Join-Path $root 'plugin\build\release\CausticLight.dll') $skse -Force
Copy-Item (Join-Path $root 'plugin\data\SKSE\Plugins\CausticLight\zones.json') (Join-Path $skse 'CausticLight') -Force
Copy-Item (Join-Path $root 'plugin\data\SKSE\Plugins\CausticLight.ini') $skse -Force
Copy-Item (Join-Path $root 'docs\caustic-volume-LICENSE.txt') (Join-Path $root 'dist\CausticLight\CausticLight - caustic-volume LICENSE.txt') -Force
Copy-Item (Join-Path $root 'docs\README.md') (Join-Path $root 'dist\CausticLight\CausticLight - README.md') -Force
Copy-Item (Join-Path $root 'docs\README.en.md') (Join-Path $root 'dist\CausticLight\CausticLight - README (English).md') -Force
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $root 'dist\CausticLight\CausticLight - LICENSE (GPL-3.0).txt') -Force
Copy-Item (Join-Path $root 'NOTICE') (Join-Path $root 'dist\CausticLight\CausticLight - NOTICE.txt') -Force
Write-Host 'build ok'
