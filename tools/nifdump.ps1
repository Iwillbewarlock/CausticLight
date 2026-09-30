param([string]$Path)
# Read-only minimal SSE NIF (20.2.0.7, bsver 100) dumper: block list + details for effect-shader animation blocks.
$bytes = [System.IO.File]::ReadAllBytes($Path)
$ms = New-Object System.IO.MemoryStream(,$bytes)
$br = New-Object System.IO.BinaryReader($ms)
# header line
while ($br.ReadByte() -ne 10) {}
$ver = $br.ReadUInt32(); $null = $br.ReadByte(); $uver = $br.ReadUInt32(); $nb = $br.ReadUInt32()
$bsver = $br.ReadUInt32()
function RdShort { $l = $br.ReadByte(); if ($l -gt 0) { [Text.Encoding]::ASCII.GetString($br.ReadBytes($l)).TrimEnd([char]0) } else { '' } }
function RdSized { $l = $br.ReadUInt32(); if ($l -gt 0) { [Text.Encoding]::ASCII.GetString($br.ReadBytes($l)) } else { '' } }
$null = RdShort; $null = RdShort; $null = RdShort
$ntypes = $br.ReadUInt16()
$types = @(); for ($i=0;$i -lt $ntypes;$i++){ $types += RdSized }
$tidx = @(); for ($i=0;$i -lt $nb;$i++){ $tidx += ($br.ReadUInt16() -band 0x7FFF) }
$sizes = @(); for ($i=0;$i -lt $nb;$i++){ $sizes += $br.ReadUInt32() }
$nstr = $br.ReadUInt32(); $null = $br.ReadUInt32()
$strs = @(); for ($i=0;$i -lt $nstr;$i++){ $strs += RdSized }
$ngroups = $br.ReadUInt32(); for ($i=0;$i -lt $ngroups;$i++){ $null=$br.ReadUInt32() }
"FILE $Path  ver=0x{0:X} user={1} bsver={2} blocks={3}" -f $ver,$uver,$bsver,$nb
$interpNames = @{1='LINEAR';2='QUADRATIC';3='TBC';4='XYZ';5='CONST'}
$effVar = @('EmissiveMult','FalloffStartAngle','FalloffStopAngle','FalloffStartOpacity','FalloffStopOpacity','Alpha','U_Offset','U_Scale','V_Offset','V_Scale')
$lightVar = @{0='RefractionStrength';8='ReflectionStrength';9='Glossiness';10='SpecularStrength';11='EmissiveMult';12='Alpha';20='U_Offset';21='U_Scale';22='V_Offset';23='V_Scale'}
$start = $ms.Position
for ($b=0;$b -lt $nb;$b++){
  $t = $types[$tidx[$b]]; $pos = $ms.Position; $line = "  [$b] $t"
  try {
    switch -Regex ($t) {
      '^(BSEffectShaderProperty|NiAlphaProperty|NiNode|BSFadeNode|BSTriShape|NiTriShape|BSXFlags|NiStringExtraData)$' {
        $ni = $br.ReadInt32(); $nm = if ($ni -ge 0) { $strs[$ni] } else { '' }; $line += " name='$nm'"
      }
    }
    switch ($t) {
      'BSEffectShaderProperty' {
        $ne=$br.ReadUInt32(); for($k=0;$k -lt $ne;$k++){$null=$br.ReadInt32()}; $ctrl=$br.ReadInt32()
        $f1=$br.ReadUInt32(); $f2=$br.ReadUInt32(); $uo=$br.ReadSingle(); $vo=$br.ReadSingle(); $us=$br.ReadSingle(); $vs=$br.ReadSingle()
        $tex = RdSized; $clamp=$br.ReadByte(); $li=$br.ReadByte(); $lod=$br.ReadByte(); $ub=$br.ReadByte()
        $fa=@($br.ReadSingle(),$br.ReadSingle(),$br.ReadSingle(),$br.ReadSingle())
        $col=@($br.ReadSingle(),$br.ReadSingle(),$br.ReadSingle(),$br.ReadSingle()); $cs=$br.ReadSingle(); $sfd=$br.ReadSingle(); $grey = RdSized
        $line += " ctrl=$ctrl flags1=0x{0:X8} flags2=0x{1:X8} uvOff=({2},{3}) uvScale=({4},{5}) tex='{6}' clamp={7} baseColor=({8}) scale={9} grey='{10}'" -f $f1,$f2,$uo,$vo,$us,$vs,$tex,$clamp,($col -join ','),$cs,$grey
      }
      'BSLightingShaderProperty' {
        $null=$br.ReadUInt32() # shader type
        $ni2=$br.ReadInt32(); $ne=$br.ReadUInt32(); for($k=0;$k -lt $ne;$k++){$null=$br.ReadInt32()}; $ctrl=$br.ReadInt32()
        $f1=$br.ReadUInt32(); $f2=$br.ReadUInt32(); $line += " ctrl=$ctrl flags1=0x{0:X8} flags2=0x{1:X8}" -f $f1,$f2
      }
      'NiAlphaProperty' {
        $ne=$br.ReadUInt32(); for($k=0;$k -lt $ne;$k++){$null=$br.ReadInt32()}; $null=$br.ReadInt32()
        $fl=$br.ReadUInt16(); $th=$br.ReadByte(); $src=($fl -shr 1) -band 0xF; $dst=($fl -shr 5) -band 0xF
        $line += " flags=0x{0:X4} blend={1} src={2} dst={3} test={4} thr={5}" -f $fl,($fl -band 1),$src,$dst,(($fl -shr 9) -band 1),$th
      }
      {$_ -in 'BSEffectShaderPropertyFloatController','BSEffectShaderPropertyColorController','BSLightingShaderPropertyFloatController','BSLightingShaderPropertyColorController','NiTextureTransformController'} {
        $next=$br.ReadInt32(); $fl=$br.ReadUInt16(); $freq=$br.ReadSingle(); $ph=$br.ReadSingle(); $st=$br.ReadSingle(); $sp=$br.ReadSingle(); $tgt=$br.ReadInt32(); $interp=$br.ReadInt32()
        $cv=$br.ReadUInt32()
        $vn = if ($t -like 'BSEffect*Float*') { $effVar[$cv] } elseif ($t -like 'BSLighting*Float*') { $lightVar[[int]$cv] } else { "$cv" }
        $line += " next=$next flags=0x{0:X4} freq={1} phase={2} start={3} stop={4} target={5} interp={6} var={7}({8})" -f $fl,$freq,$ph,$st,$sp,$tgt,$interp,$cv,$vn
      }
      'NiFloatInterpolator' { $v=$br.ReadSingle(); $d=$br.ReadInt32(); $line += " value=$v data=$d" }
      'NiPoint3Interpolator' { $null=$br.ReadBytes(12); $d=$br.ReadInt32(); $line += " data=$d" }
      'NiFloatData' {
        $n=$br.ReadUInt32(); $line += " numKeys=$n"
        if ($n -gt 0) { $it=$br.ReadUInt32(); $line += " interp=$it($($interpNames[[int]$it]))"
          $keys=@(); for($k=0;$k -lt [math]::Min($n,12);$k++){ $tt=$br.ReadSingle(); $vv=$br.ReadSingle(); if($it -eq 2){$null=$br.ReadBytes(8)} elseif($it -eq 3){$null=$br.ReadBytes(12)}; $keys += ("{0:0.###}:{1:0.###}" -f $tt,$vv) }
          $line += " keys=[" + ($keys -join ' ') + "]" }
      }
      'NiControllerSequence' { $ni=$br.ReadInt32(); $line += " name='$($strs[$ni])'" }
      'NiStringExtraData' { $si=$br.ReadInt32(); $line += " value='$($strs[$si])'" }
    }
  } catch { $line += " <parse err>" }
  $line
  $ms.Position = $pos + $sizes[$b]
}
