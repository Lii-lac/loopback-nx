# Real-Windows write test

Runs the write path against the actual Windows exFAT driver instead of Linux. Needs Docker, Git Bash and
an elevated PowerShell (mounting a disk image needs admin).

- `make_vhd.ps1`: wraps a raw volume image in an MBR partition and a fixed-VHD footer so Windows will mount it.
- `server.ps1`: run once in an **elevated** PowerShell. Watches a jobs folder, mounts each `<name>.vhd` when
  `<name>.go` appears, runs `<name>.ops.ps1` against the drive and writes `<name>.result.txt`.
- `win_ops.ps1`: the file operations (copy, robocopy, rename, move, delete, overwrite) plus a manifest of what
  Windows sees afterwards.
- `wintest.sh <name>`: dumps the synthesized volume, queues it, then replays the changed sectors into the
  backend, commits onto a scratch tree and compares the tree with Windows' manifest.

Set `WINTEST_DIR` to a scratch folder that holds `make_vhd.ps1` and `server.ps1`; the server's jobs folder is
`$WINTEST_DIR/vhdsrv/jobs`.

This caught a real bug: with the original minimal up-case table Windows mounted the volume read-only.
