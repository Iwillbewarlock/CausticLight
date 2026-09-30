param([string]$Path)
# Read-only: report BSTriShape vertex/triangle counts and bounds for an SSE NIF.
$bytes = [System.IO.File]::ReadAllBytes($Path)
$ms = New-Object System.IO.MemoryStream(,$bytes); $br = New-Object System.IO.BinaryReader($ms)
while ($br.ReadByte() -ne 10) {}
$null=$br.ReadUInt32(); $null=$br.ReadByte(); $null=$br.ReadUInt32(); $nb=$br.ReadUInt32(); $null=$br.ReadUInt32()
for($i=0;$i -lt 3;$i++){ $l=$br.ReadByte(); $null=$br.ReadBytes($l) }
$nt=$br.ReadUInt16(); $types=@(); for($i=0;$i -lt $nt;$i++){ $l=$br.ReadUInt32(); $types+=[Text.Encoding]::ASCII.GetString($br.ReadBytes($l)) }
$tidx=@(); for($i=0;$i -lt $nb;$i++){ $tidx+=($br.ReadUInt16() -band 0x7FFF) }
$sizes=@(); for($i=0;$i -lt $nb;$i++){ $sizes+=$br.ReadUInt32() }
$ns=$br.ReadUInt32(); $null=$br.ReadUInt32(); $strs=@(); for($i=0;$i -lt $ns;$i++){ $l=$br.ReadUInt32(); $strs+=[Text.Encoding]::ASCII.GetString($br.ReadBytes($l)) }
$ng=$br.ReadUInt32(); for($i=0;$i -lt $ng;$i++){ $null=$br.ReadUInt32() }
for($b=0;$b -lt $nb;$b++){
  $pos=$ms.Position; $t=$types[$tidx[$b]]
  if ($t -eq 'BSTriShape') {
    $ni=$br.ReadInt32(); $ne=$br.ReadUInt32(); for($k=0;$k -lt $ne;$k++){$null=$br.ReadInt32()}; $null=$br.ReadInt32()
    $null=$br.ReadUInt32(); $tx=$br.ReadSingle();$ty=$br.ReadSingle();$tz=$br.ReadSingle(); $null=$br.ReadBytes(36); $sc=$br.ReadSingle(); $null=$br.ReadInt32()
    $cx=$br.ReadSingle();$cy=$br.ReadSingle();$cz=$br.ReadSingle();$r=$br.ReadSingle()
    $skin=$br.ReadInt32(); $sh=$br.ReadInt32(); $al=$br.ReadInt32()
    $vd=$br.ReadUInt64(); $ntri=$br.ReadUInt16(); $nv=$br.ReadUInt16(); $ds=$br.ReadUInt32()
    $vsz=($vd -band 0xF)*4; $expect=$nv*$vsz + $ntri*6
    "  [$b] '$($strs[$ni])' verts=$nv tris=$ntri vertSize=$vsz dataSize=$ds (expect $expect) bound center=({0:0.#},{1:0.#},{2:0.#}) r={3:0.#} trans=({4:0.#},{5:0.#},{6:0.#}) scale={7}" -f $cx,$cy,$cz,$r,$tx,$ty,$tz,$sc
  }
  $ms.Position=$pos+$sizes[$b]
}
