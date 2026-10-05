# Run in an ELEVATED PowerShell. Watches the jobs folder for *.vhd files, mounts each one, probes what
# Windows does with it (read-only? can it write?), optionally runs <name>.ops.ps1 against the drive,
# then dismounts and writes <name>.result.txt. Stop it by creating the file "stop" in the jobs folder.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$jobs = Join-Path $root 'jobs'
New-Item -ItemType Directory -Force $jobs | Out-Null
Write-Host "watching $jobs (Ctrl+C or create 'stop' to end)"
$done = @{}
while (-not (Test-Path (Join-Path $jobs 'stop'))) {
    foreach ($vhd in Get-ChildItem $jobs -Filter *.vhd -ErrorAction SilentlyContinue) {
        $name = $vhd.BaseName
        $stamp = "$name|$($vhd.LastWriteTimeUtc.Ticks)"
        if ($done[$stamp]) { continue }
        if (-not (Test-Path (Join-Path $jobs "$name.go"))) { continue }   # job is ready when <name>.go exists
        $done[$stamp] = $true
        Remove-Item (Join-Path $jobs "$name.go") -ErrorAction SilentlyContinue   # one run per request
        $res = Join-Path $jobs "$name.result.txt"
        Remove-Item $res -ErrorAction SilentlyContinue
        $log = { param($m) Add-Content -Path $res -Value $m; Write-Host $m }
        try {
            & $log "== $name  $(Get-Date -Format s)"
            $img = Mount-DiskImage -ImagePath $vhd.FullName -StorageType VHD -PassThru -ErrorAction Stop
            $disk = $img | Get-Disk
            & $log "disk $($disk.Number) IsReadOnly=$($disk.IsReadOnly) size=$($disk.Size)"
            $vol = $null
            for ($i = 0; $i -lt 30 -and -not $vol; $i++) {
                Start-Sleep -Milliseconds 500
                $vol = Get-Disk -Number $disk.Number | Get-Partition -ErrorAction SilentlyContinue | Get-Volume -ErrorAction SilentlyContinue | Where-Object DriveLetter | Select-Object -First 1
            }
            if (-not $vol) {
                $p = Get-Disk -Number $disk.Number | Get-Partition -ErrorAction SilentlyContinue
                & $log "no volume with a drive letter appeared. partitions: $($p | Out-String)"
            } else {
                $d = "$($vol.DriveLetter):"
                & $log "drive $d fs=$($vol.FileSystem) label=$($vol.FileSystemLabel) size=$($vol.Size)"
                & $log ((fsutil fsinfo volumeinfo $d 2>&1) -join "`n")
                & $log ((fsutil dirty query $d 2>&1) -join "`n")
                try { New-Item -ItemType Directory -Path "$d\probe_dir" -ErrorAction Stop | Out-Null; & $log "mkdir probe_dir: ok" } catch { & $log "mkdir probe_dir: FAILED $($_.Exception.Message)" }
                try { 'probe' | Set-Content "$d\probe.txt" -ErrorAction Stop; & $log "write probe.txt: ok" } catch { & $log "write probe.txt: FAILED $($_.Exception.Message)" }
                $ops = Join-Path $jobs "$name.ops.ps1"
                if (Test-Path $ops) {
                    & $log "running ops"
                    try { $out = & $ops -Drive $d 2>&1 | Out-String; & $log $out } catch { & $log "ops FAILED $($_.Exception.Message)" }
                }
                Start-Sleep -Seconds 2
            }
        } catch {
            & $log "ERROR: $($_.Exception.Message)"
        } finally {
            try { Dismount-DiskImage -ImagePath $vhd.FullName -ErrorAction Stop | Out-Null; & $log "dismounted" } catch { & $log "dismount: $($_.Exception.Message)" }
            Add-Content -Path $res -Value "DONE"
        }
    }
    Start-Sleep -Seconds 2
}
Write-Host 'stopped'
