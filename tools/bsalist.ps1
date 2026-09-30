param([string]$Path)
# Minimal read-only BSA (v104/v105) name lister. Outputs "folder\file" per line.
$fs = [System.IO.File]::OpenRead($Path)
$br = New-Object System.IO.BinaryReader($fs)
try {
  $magic = $br.ReadUInt32()
  $ver = $br.ReadUInt32()
  $folderOff = $br.ReadUInt32()
  $flags = $br.ReadUInt32()
  $folderCount = $br.ReadUInt32()
  $fileCount = $br.ReadUInt32()
  $totFolderNameLen = $br.ReadUInt32()
  $totFileNameLen = $br.ReadUInt32()
  $fs.Position = $folderOff
  $counts = New-Object 'int[]' $folderCount
  $recSize = if ($ver -eq 105) { 24 } else { 16 }
  for ($i=0; $i -lt $folderCount; $i++) {
    $null = $br.ReadUInt64(); $counts[$i] = $br.ReadUInt32()
    $null = $br.ReadBytes($recSize - 12)
  }
  $folders = New-Object 'string[]' $folderCount
  for ($i=0; $i -lt $folderCount; $i++) {
    $len = $br.ReadByte()
    $bytes = $br.ReadBytes($len)
    $folders[$i] = [System.Text.Encoding]::ASCII.GetString($bytes, 0, $len - 1)
    $null = $br.ReadBytes(16 * $counts[$i])
  }
  $nameBytes = $br.ReadBytes($totFileNameLen)
  $names = [System.Text.Encoding]::ASCII.GetString($nameBytes).Split([char]0)
  $k = 0
  $out = New-Object System.Collections.Generic.List[string]
  for ($i=0; $i -lt $folderCount; $i++) {
    for ($j=0; $j -lt $counts[$i]; $j++) { $out.Add($folders[$i] + '\' + $names[$k]); $k++ }
  }
  $out
} finally { $br.Close() }
