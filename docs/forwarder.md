# Home-screen icon

A forwarder is a tiny NSP that puts Loopback on the Home menu and opens `sd:/switch/loopback.nro` when you select it. It does not contain the app, so updating the `.nro` never needs a new forwarder.

To start Loopback from Home with no startup logo, build the forwarder with [NTON](https://github.com/rlaphoenix/nton) and these changes. Stock NTON shows "Licensed by Nintendo". The app fades in from black on its own at launch.

1. `pip install nton` in a virtual environment.
2. In NTON's `main.py` make three changes:
   - Add `"--nologo"` and `"--nopatchnacplogo"` to the list of arguments passed to hacBrewPack. Dropping the logo section is what removes the startup logo.
   - After the NACP is read, set `control_file_data[0x30F0] = 0x02` and `control_file_data[0x30F1] = 0x00`.
   - Change the version write to `control_file_data[0x3060:0x3070]`. Stock NTON uses a 15-byte slice for a 16-byte value, which shifts every later NACP field by one.
3. Put your own console's `prod.keys` in the folder you build from. Dump them from your own Switch. They are never part of this repository.
4. Build from PowerShell, not Git Bash. Git Bash rewrites `/switch/loopback.nro` into a Windows path and the forwarder then fails at launch:

   ```
   nton build loopback.nro --sdmc sdmc:/switch/loopback.nro --id <title id> --name Loopback --publisher <you> --version 0.2.4 --icon icon.jpg
   ```

5. Check the result before installing it. Extract the NSP with `nstool -x`, decrypt the Program NCA with `nstool -k prod.keys --part1 <dir> <nca>` and check that `nextNroPath` reads `sdmc:/switch/loopback.nro` and that no logo files exist.
6. Install the NSP with DBI or Goldleaf.

Keep the same `--id` and raise `--version` each time you rebuild, and uninstall the old forwarder first, or the console keeps the old settings. The icon is `icon.jpg`; the console caches Home icons by title ID, so a new icon needs a new `--id` or an uninstall first.
