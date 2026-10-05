# How it works

Windows needs a block device, but the Switch's SD card is owned by the console's filesystem and cannot be handed out raw. So Loopback builds a drive out of the files.

## Transport

Loopback registers a USB mass storage interface (SCSI over bulk-only transport) through `usb:ds`, with High Speed and SuperSpeed descriptors. The USB thread answers SCSI commands. The link runs at whatever the cable and port allow; High Speed is about 40 MB/s.

## The volume

At mount the app scans the card once. From that scan it generates an exFAT volume on demand: boot sector, allocation bitmap, up-case table and directory entries. Every file is given one contiguous run of clusters, so finding the file behind a cluster is a lookup in a sorted list, and no FAT has to be held in memory. File data is read straight from the real file when the PC asks for it. The volume is sized to the card's used data plus headroom, not the whole card.

The up-case table is the standard one. Windows mounts the volume read-only if it is not.

The app's own folder, `sd:/switch/loopback/`, is left out of the volume.

## Writes

The PC sends raw sector writes: directory entries, bitmap updates and file data, in whatever order the driver likes. Interpreting each one as it arrives would act on half-written states, so nothing is applied until a consistent image exists.

1. **Overlay.** Sector writes go into an overlay. Reads check the overlay first and fall back to the generated volume. Small writes are stored in a slot file. Long cluster-aligned runs from large copies are kept in separate segment files laid out like the volume. A write-behind buffer acknowledges the PC at once so the copy runs at USB speed.
2. **Save.** At a save point the overlay is applied on top of the generated image and parsed as exFAT. The new tree is compared with the scan: a node is the same file if it starts at the same cluster, and empty files match by name inside their parent.
3. **Apply.** New contents are written to a stage folder first, after checking the card has room, so a failure leaves the card unchanged. Then every moved or rewritten old file is parked in the stage folder, deleted items are removed (files, then folders deepest first), the new tree is placed top-down, and the stage folder is removed. Nothing is overwritten in place. A new or rewritten file of 4 MiB or more that sits whole in one segment is not copied: its segment file is cut to size and renamed into place.
4. **Rescan.** The card is the truth after a save, so the volume is rebuilt from a fresh scan, the overlay is emptied and the SCSI layer reports a media change once. Windows remounts the drive.

A save is refused as a whole, with the card untouched and the PC's changes kept, if the volume was reformatted, the image is half written, a name is not valid on FAT32, a file is 4 GiB or more, or a file the PC touched changed on the Switch after the scan. Large deletions ask first unless expert mode is on.

Windows' own housekeeping (`System Volume Information`, `$RECYCLE.BIN`, the dirty flag) is ignored.

While a save runs, Loopback answers the PC "not ready" so a long save cannot time Windows out.

## Threads

The UI thread draws at 30 fps (20 when idle, display pace during the launch fade) and owns the framebuffer and input. The main thread owns USB, the scan and saves. A worker thread stores the write-behind buffers. They share a few atomic flags and a mutex-protected log.
