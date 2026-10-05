# Building and testing

## Build the app

You need Docker. devkitPro does not have to be installed.

```
docker run --rm -v "%cd%:/src" -w /src devkitpro/devkita64 make
```

In Git Bash use `MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/src" -w /src devkitpro/devkita64 make`.

Output: `loopback.nro` in the project root. `make clean` removes it and `build/`. `make` does not track `icon.jpg`; delete `loopback.nro` after changing the icon.

## Tests

None of these need a Switch. The last three need `--privileged` because they mount volumes.

| Test | Covers |
|---|---|
| `tests/host_test.c` | SCSI decoding, and that the RAM disk is a valid FAT image. Build with `gcc tests/host_test.c source/scsi.c source/backend_ram.c`, run it with an output path and check that with `fsck.fat -n`. |
| `tests/run_parse_test.sh` | The exFAT parser against volumes made by `mkfs.exfat`. |
| `tests/run_synth_test.sh` | The generated volume against `fsck.exfat` and a real mount. |
| `tests/run_commit_test.sh` | The write path end to end, with the Linux exFAT driver standing in for Windows. 31 scenarios; pass names to run only some. |
| `tests/ui_render.c` | Renders every screen to images with the same drawing code the console runs. Needs gcc and a TTF font. |
| `tests/windows/` | The write path against the real Windows exFAT driver, through a VHD. See its README. |

Run the shell tests in a container:

```
docker run --rm --privileged -m 6g -v "<project>:/src" -w /src ubuntu:24.04 bash tests/run_commit_test.sh
docker run --rm --privileged -v "<project>:/src" -w /src ubuntu:24.04 bash tests/run_parse_test.sh
docker run --rm --privileged -m 6g -v "<project>:/src" -w /src ubuntu:24.04 bash tests/run_synth_test.sh
```

Re-run them after any change under `source/` that touches the volume or the write path. `source/main.c`, `ui.c` and `usb_msc.c` are not covered by them; those are checked on a console.

## Source layout

| | |
|---|---|
| `source/main.c` | App entry: the UI thread and the USB thread, mounting, save triggers, prompts |
| `source/ui.c`, `ui.h` | The screens, the map, the activity board, input |
| `source/gfx.c`, `gfx.h` | Software renderer: shapes, text, blur |
| `source/stb_truetype.h` | Font rasteriser (public domain, vendored) |
| `source/usb_msc.c`, `usb_msc.h` | USB mass storage transport over `usb:ds`, read-ahead, write-behind |
| `source/scsi.c`, `scsi.h` | SCSI commands |
| `source/backend.h` | The interface between SCSI and whatever holds the data |
| `source/backend_ram.c` | RAM disk backend |
| `source/exfat_synth.c`, `synth_priv.h`, `upcase_table.h` | The generated exFAT volume |
| `source/overlay.c` | Scratch store for the PC's writes |
| `source/exfat_parse.c` | Reads an exFAT image back into a tree |
| `source/synth_commit.c` | Turns the PC's changes into file operations on the card |
| `source/log.c` | Thread-safe logger |
| `tests/` | The tests above |
| `icon.jpg` | The app icon. `make` does not track it, so delete `loopback.nro` after changing it |
