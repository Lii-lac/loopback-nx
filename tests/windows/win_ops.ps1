param([string]$Drive)
# Runs on the mounted volume as the host. Ends by writing a manifest of what Windows sees.
$jobs = $PSScriptRoot  # the server runs this from its jobs folder
$d = "$Drive\"
$tmp = Join-Path $env:TEMP 'nx_payload'
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$tmp\deep\er\est" | Out-Null
1..120 | ForEach-Object { "payload $_" | Set-Content "$tmp\deep\f_$_.txt" }
$r = New-Object Random 7
$b = New-Object byte[] 5000000; $r.NextBytes($b); [IO.File]::WriteAllBytes("$tmp\deep\er\five_meg.bin", $b)
$b = New-Object byte[] 32768; $r.NextBytes($b); [IO.File]::WriteAllBytes("$tmp\deep\er\est\one_cluster.bin", $b)
New-Item "$tmp\deep\er\est\empty.txt" -ItemType File | Out-Null
"unicode" | Set-Content ("$tmp\deep\" + [char]0x00FC + 'n' + [char]0x00EF + '-' + [char]0x65E5 + [char]0x672C + '.txt')
('n' * 200 + '.txt') | ForEach-Object { 'long name' | Set-Content "$tmp\deep\$_" }

# 1. copy a tree and a big file in
Copy-Item "$tmp\deep" "$d\incoming" -Recurse
Copy-Item "$tmp\deep\er\five_meg.bin" "$d\top_level_copy.bin"
robocopy "$tmp\deep\er" "$d\robo" /E /NFL /NDL /NJH /NJS | Out-Null
Start-Sleep -Seconds 1

# 2. rename, move, delete, overwrite, append
Rename-Item "$d\a.txt" 'A_renamed.txt'
Move-Item "$d\docs\notes.txt" "$d\sub\notes_moved.txt"
Remove-Item "$d\sub\b.txt"
Add-Content "$d\sub\notes_moved.txt" 'appended line'
$b = New-Object byte[] 40000; $r.NextBytes($b); [IO.File]::WriteAllBytes("$d\docs\blob.bin", $b)   # overwrite with smaller
Rename-Item "$d\docs" 'Docs_Renamed'
New-Item -ItemType Directory "$d\fresh_dir" | Out-Null
Move-Item "$d\incoming\deep" "$d\fresh_dir\deep_moved" -ErrorAction SilentlyContinue
Remove-Item "$d\robo\est" -Recurse
Start-Sleep -Seconds 1

# 3. things Windows adds on its own
New-Item -ItemType Directory -Force "$d\System Volume Information" -ErrorAction SilentlyContinue | Out-Null
'guid' | Set-Content "$d\System Volume Information\IndexerVolumeGuid" -ErrorAction SilentlyContinue

# manifest of what the host sees
Start-Sleep -Seconds 1
$root = (Get-Item $d).FullName.TrimEnd('\')
$lines = New-Object System.Collections.Generic.List[string]
Get-ChildItem -LiteralPath $d -Recurse -Force | Where-Object { $_.FullName -notmatch '\\System Volume Information|\\\$RECYCLE\.BIN' } | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($root.Length).Replace('\', '/')
    if ($_.PSIsContainer) { $lines.Add("D $rel") }
    else { $lines.Add("F $rel $($_.Length) $((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower())") }
}
[IO.File]::WriteAllLines("$jobs\manifest_windows.txt", $lines, (New-Object Text.UTF8Encoding($false)))
"manifest: $($lines.Count) entries"
