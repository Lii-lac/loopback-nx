# Reference

## Stops on the map

| Stop | Meaning |
|---|---|
| Ready | Nothing is shared. Press Mount. |
| Reading | Scanning the card. The caption counts files. |
| Plug in | Waiting for the cable. "Connecting" shows while your PC sets the drive up. |
| Mounted | Your PC has the drive. It is named `SWITCH SD`. |
| Changes waiting | Your PC wrote something that is not on the card yet. |
| Saving | Applying your PC's changes to the card. Keep the cable in. |
| Unplugged | Ejected, or the cable came out. |

The board in the top left shows live **Writing** and **Reading** speeds in MB/s, and an **Eject** line:

- **Safe**: nothing is waiting.
- **Wait**: your PC wrote in the last two seconds, or a save is running.
- **Save first**: changes are waiting.

## Controls

Everything also works by touch.

| Button | Does |
|---|---|
| `X` | Mount. Once mounted, Eject. While reading, saving or waiting for the cable, Cancel. |
| `R` | Save now, when changes are waiting. |
| `Y` | Advanced. |
| `+` | Quit. |
| `A` | Press the focused control. |
| `B` | Back, or cancel a save. |

Access (read only or read and write), Share and Safety mode are chosen before you mount and are locked while mounted. Eject first to change them.

## Safety mode

Advanced, then **Safety mode**. It is off by default, and Loopback stays out of your way. With it off:

- Loopback mounts as soon as it opens, in Read and write, with the whole card shared.
- There is no warning before your PC may change the card.
- Large deletions apply without asking.
- Saves start two seconds after your PC goes quiet.
- Ejecting or quitting saves your changes first, with no prompt.

Turn it on to be asked first. With it on:

- Loopback waits for you to press Mount, and starts read only.
- Choosing Read and write on the whole card shows a warning first.
- A save that would delete or overwrite more than 50 files stops and asks on the Switch.
- Saves start five seconds after your PC goes quiet.
- Ejecting or quitting with changes waiting asks whether to save or discard them.

Back up your SD card first either way. The setting is kept in `sd:/switch/loopback/settings.txt`.

## Advanced

| Section | |
|---|---|
| Share | Whole card, only `sd:/nx-test`, or a small RAM disk for checking the cable. |
| Appearance | Match the console, light or dark. |
| Safety mode | See above. |
| Updates | Check for a newer release and install it. See Updating. |
| Connection | Link speed and access. |
| Activity | What happened this session. |

## Updating

Advanced, then **Updates**. It shows the version you have and the newest release, and has one button that changes with the state:

| Button | When |
|---|---|
| Check for updates | Nothing checked yet, up to date, or the last try failed. Needs Wi-Fi. |
| Download and install X | A newer release exists. |
| Cancel | While checking or downloading. |
| Restart Loopback | After installing. Starts the new version. |

- Checking works at any time. Installing and restarting need the drive to be ejected, because they replace the app file on the card.
- Nothing is changed unless the download is a Switch app and its SHA-256 matches the `loopback.nro.sha256` published with the release. The old version is kept as `sd:/switch/loopback/loopback.nro.bak`; to go back, copy it over `sd:/switch/loopback.nro`.
- The forwarder does not need reinstalling. It launches `sd:/switch/loopback.nro`, which is the file that gets replaced.
- It looks at the project's GitHub releases (`github.com/Lii-lac/loopback-nx`, set by `UPD_REPO_URL` in `source/update.h`). The repository has to be public, or the check says "No release found".

## Saving

Your PC's writes are held in a scratch file and applied to the card as one batch. A save starts when:

- you Safely Remove the drive on the PC (immediately),
- your PC flushes its cache and goes quiet for half a second,
- your PC has not written for two seconds (five with safety mode on), or
- you press `R`, or quit or eject on the Switch.

Windows caches writes. If a save has not started yet, run Safely Remove or wait a couple of seconds. While it saves, the drive drops out and comes back on the PC; give it a few seconds to list files again.

If you pull the cable mid-copy, Loopback does not save by itself. The screen says "Cable out. What arrived may be incomplete." `R` saves what arrived, and Eject throws it away.

## Eject and quit

- **Eject** (`X`) stops the drive and ends on Unplugged. If changes are waiting, it saves them first with safety mode off and asks whether to save or discard them with it on. Press Mount to go again.
- **Quit** (`+`) does the same, then opens the Homebrew Menu.

## Good to know

- The volume is a snapshot taken when you mount. Changes the Switch makes afterwards show up after the next save or remount, and a save will not overwrite a file that changed on the Switch after the scan.
- Windows' own `System Volume Information` and `$RECYCLE.BIN` are not copied to the card.
- `sd:/switch/loopback/` is hidden from the PC. Read `log.txt` from the card with a card reader; it is replaced at each launch.

## Known limitations

- **Files of 4 GiB or more cannot be saved.** That is a hard limit of FAT32 cards, and Loopback applies it to every card so the result is the same whichever format yours uses. The save is refused as a whole.
- **Wait for the drive to list again after a save before writing into the same folder.** After a save the card is rescanned and the drive remounts. Windows can still write back an out-of-date copy of a folder it had open. Loopback handles the usual result: it keeps the entry that is already on the card, or refuses the save until the file's data has arrived. One case it cannot tell apart: an out-of-date entry whose old location now belongs to a different file looks like a rename. That has not been seen on a real console, but it is possible.
- **Free space shown to the PC is about half the card's free space, capped at 3.5 GiB,** because a copy is held twice until it is saved. A bigger copy needs several rounds.
- **Timestamps are not kept** on files you copy in.
- **Some folders under `Nintendo/Contents` lose a flag the console uses** if the PC recreates them.
- **Files the console cannot read when Loopback scans do not show up on the PC.** They stay on the card, untouched. On a typical Atmosphere card that is a handful of files, such as the automatic backups under `atmosphere/automatic_backups`. `log.txt` lists each one as `skip stat failed`.
- **A crash or power loss during a save can leave a folder named `.nxusb-stage...` on the card.** The PC can see it, so you can recover from it or delete it. Loopback warns about one at the next write-mode launch.
