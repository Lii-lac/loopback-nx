param([string]$Img, [string]$Vhd)
# Wraps a raw filesystem image in an MBR partition (starting at 1 MiB) and a fixed-VHD footer.
$imgBytes = (Get-Item $Img).Length
$startSec = 2048
$size = $startSec * 512 + $imgBytes
$total = [uint64]($size / 512)
$cap = [uint64](65535 * 16 * 255)
if ($total -gt $cap) { $total = $cap }
if ($total -ge 65535 * 16 * 63) { $spt = 255; $heads = 16; $cth = [math]::Floor($total / $spt) }
else {
  $spt = 17; $cth = [math]::Floor($total / $spt); $heads = [math]::Floor(($cth + 1023) / 1024)
  if ($heads -lt 4) { $heads = 4 }
  if ($cth -ge $heads * 1024 -or $heads -gt 16) { $spt = 31; $heads = 16; $cth = [math]::Floor($total / $spt) }
  if ($cth -ge $heads * 1024) { $spt = 63; $heads = 16; $cth = [math]::Floor($total / $spt) }
}
$cyl = [math]::Floor($cth / $heads)
function PutBE($arr, $off, $val, $n) { for ($i = 0; $i -lt $n; $i++) { $arr[$off + $i] = [byte](($val -shr (8 * ($n - 1 - $i))) -band 0xFF) } }
function PutLE($arr, $off, $val, $n) { for ($i = 0; $i -lt $n; $i++) { $arr[$off + $i] = [byte](($val -shr (8 * $i)) -band 0xFF) } }

# MBR + gap
$head = New-Object byte[] ($startSec * 512)
PutLE $head 0x1B8 (Get-Random -Maximum 2147483647) 4
$head[0x1BE] = 0x00
$head[0x1BF] = 0xFE; $head[0x1C0] = 0xFF; $head[0x1C1] = 0xFF
$head[0x1C2] = 0x07
$head[0x1C3] = 0xFE; $head[0x1C4] = 0xFF; $head[0x1C5] = 0xFF
PutLE $head 0x1C6 $startSec 4
PutLE $head 0x1CA ([uint32]($imgBytes / 512)) 4
$head[510] = 0x55; $head[511] = 0xAA

# footer
$f = New-Object byte[] 512
[Text.Encoding]::ASCII.GetBytes('conectix').CopyTo($f, 0)
PutBE $f 8 2 4
PutBE $f 12 0x00010000 4
for ($i = 16; $i -lt 24; $i++) { $f[$i] = 0xFF }
PutBE $f 24 ([uint32]([DateTimeOffset]::UtcNow.ToUnixTimeSeconds() - 946684800)) 4
[Text.Encoding]::ASCII.GetBytes('win ').CopyTo($f, 28)
PutBE $f 32 0x000A0000 4
[Text.Encoding]::ASCII.GetBytes('Wi2k').CopyTo($f, 36)
PutBE $f 40 ([uint64]$size) 8
PutBE $f 48 ([uint64]$size) 8
PutBE $f 56 $cyl 2
$f[58] = $heads; $f[59] = $spt
PutBE $f 60 2 4
[Guid]::NewGuid().ToByteArray().CopyTo($f, 68)
$sum = [uint64]0
foreach ($b in $f) { $sum += $b }
PutBE $f 64 ([uint32]([uint64]4294967295 - $sum)) 4

$out = [IO.File]::Create($Vhd)
$out.Write($head, 0, $head.Length)
$in = [IO.File]::OpenRead($Img)
$in.CopyTo($out)
$in.Close()
$out.Write($f, 0, 512)
$out.Close()
"vhd ok: $((Get-Item $Vhd).Length) bytes"
