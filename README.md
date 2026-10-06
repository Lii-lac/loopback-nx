# Loopback

<img src="docs/images/mounted.jpg" alt="Loopback on the Switch, mounted" width="720">

**Your Switch's SD card as a real drive on your PC.** Open Loopback, plug in a USB-C cable, and `SWITCH SD` appears in Explorer with its own drive letter. No reboot, no payload, nothing to install on the PC.

- **It is just a drive.** Drag and drop, right-click, "Open with", Send to, 7-Zip, edit a file in place and press save. Every program that can open a file can open one on your card, because as far as Windows knows it is a USB stick.
- **Fast.** About 30 to 40 MB/s each way, close to the practical limit of USB 2. A 1.5 GB game copies in well under two minutes.
- **No driver, no PC software.** It uses the mass storage support built into Windows.
- **Half a copy never lands.** Your PC's changes are held aside and applied to the card together, so pulling the cable mid-copy leaves the card as it was.
- **Stays out of your way.** In expert mode it mounts the moment it opens, saves a couple of seconds after your PC stops writing, and quits straight back to the Homebrew Menu.
- **Clear about what it is doing.** A small transit map shows where you are: reading the card, waiting for the cable, mounted, saving, unplugged.

## Install

You need a Switch with Atmosphere and the Homebrew Menu, a USB-C cable that carries data, and a Windows 10 or 11 PC.

1. Download `loopback.nro` from the [latest release](https://github.com/Lii-lac/loopback-nx/releases/latest) and copy it to `sd:/switch/loopback.nro`. (To build it yourself, see [docs/building.md](docs/building.md).)
2. Open **Loopback** from the Homebrew Menu.
3. Press `Y` for Advanced, open **Expert mode** and turn it on. This is the intended way to use Loopback, and from then on it mounts as soon as it opens.
4. Plug the Switch into your PC. `SWITCH SD` appears.

Back up your SD card before the first time. In expert mode your PC can change or delete anything on it, with no warning.

**Updating:** press `Y`, open **Updates** and check for a newer release. Install it with the drive ejected, then restart Loopback from the same screen. Copying a new `loopback.nro` over `sd:/switch/loopback.nro` works too.

**Home-screen icon (optional):** see [docs/forwarder.md](docs/forwarder.md). It only points at the `.nro`, so updating the app never needs a new one.

## Using Loopback

Press `X` to mount, plug in, and work. The map shows where you are and the caption says what comes next.

<table>
<tr>
<td><img src="docs/images/reading.jpg" alt="Reading the SD card" width="320"></td>
<td><img src="docs/images/connecting.jpg" alt="Waiting for the PC to set up the drive" width="320"></td>
<td><img src="docs/images/unplugged.jpg" alt="Ejected, safe to unplug" width="320"></td>
</tr>
<tr>
<td align="center">Reading the card</td>
<td align="center">Connecting</td>
<td align="center">Ejected</td>
</tr>
</table>

- `X` mounts, and ejects once mounted. `R` saves now. `Y` opens Advanced. `+` quits. Touch works too.
- Changes reach the card a couple of seconds after your PC stops writing, or at once when you Safely Remove the drive. Pulling the cable mid-copy saves nothing on its own.
- Files of 4 GiB or more cannot be saved (the card is FAT32), and timestamps are not kept on files you copy in.

Everything else, including expert mode in full, is in [docs/reference.md](docs/reference.md).

## More

| | |
|---|---|
| [docs/reference.md](docs/reference.md) | Every stop, control and setting, and what to know about saving |
| [docs/forwarder.md](docs/forwarder.md) | The optional Home-screen icon, with no startup logo |
| [docs/building.md](docs/building.md) | Building the `.nro`, running the tests, source layout |
| [docs/how-it-works.md](docs/how-it-works.md) | How a drive is made out of a folder tree, and how PC writes become file changes |

## License

MIT, see [LICENSE](LICENSE). `source/stb_truetype.h` is public domain (or MIT) and carries its own notice.
