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

Access (read only or read and write) and Share are chosen before you mount and are locked while mounted. Eject first to change them.

## Expert mode

Advanced, then **Expert mode**. It is off by default. When it is on:

- Loopback mounts as soon as it opens.
- Access is Read and write and Share is the whole card.
- There is no warning before your PC may change the card.
- Large deletions apply without asking. With expert mode off, a save that would delete or overwrite more than 50 files stops and asks on the Switch.
- Saves start two seconds after your PC goes quiet instead of five.
- Ejecting or quitting saves your changes first, with no prompt.

The setting is kept in `sd:/switch/loopback/settings.txt`.

## Advanced

| Section | |
|---|---|
| Share | Whole card, only `sd:/nx-test`, or a small RAM disk for checking the cable. |
| Appearance | Match the console, light or dark. |
| Expert mode | See above. |
| Connection | Link speed and access. |
| Activity | What happened this session. |

## Saving

Your PC's writes are held in a scratch file and applied to the card as one batch. A save starts when:

- you Safely Remove the drive on the PC (immediately),
- your PC flushes its cache and goes quiet for half a second,
- your PC has not written for two seconds (five outside expert mode), or
- you press `R`, or quit or eject on the Switch.

Windows caches writes. If a save has not started yet, run Safely Remove or wait a couple of seconds. While it saves, the drive drops out and comes back on the PC; give it a few seconds to list files again.

If you pull the cable mid-copy, Loopback does not save by itself. The screen says "Cable out. What arrived may be incomplete." `R` saves what arrived, and Eject throws it away.

## Eject and quit

- **Eject** (`X`) saves anything waiting in expert mode, stops the drive and ends on Unplugged. Press Mount to go again.
- **Quit** (`+`) does the same, then opens the Homebrew Menu. Outside expert mode, if changes are waiting, it asks first whether to save or discard.

## Good to know

- The volume is a snapshot taken when you mount. Changes the Switch makes afterwards show up after the next save or remount, and a save will not overwrite a file that changed on the Switch after the scan.
- Free space shown to the PC is about half the card's free space, capped at 3.5 GiB, because a copy is held twice until it is saved. A bigger copy needs several rounds.
- Files of 4 GiB or more cannot be saved to the card (it is FAT32).
- Timestamps are not kept on files you copy in.
- Windows' own `System Volume Information` and `$RECYCLE.BIN` are not copied to the card.
- Some folders under `Nintendo/Contents` lose a flag the console uses if the PC recreates them.
- `sd:/switch/loopback/` is hidden from the PC. Read `log.txt` from the card with a card reader; it is replaced at each launch.
