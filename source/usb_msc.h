// USB Mass Storage (Bulk-Only Transport) over libnx usb:ds.
#pragma once
#include <switch.h>
#include "backend.h"

typedef struct {
    UsbState       state;
    UsbDeviceSpeed speed;
    u64            bytes_read;     // device -> host
    u64            bytes_written;  // host -> device
    u32            commands;
    u32            errors;
    u8             last_opcode;
    u64            read_ns;        // time spent in backend reads (reads only)
    u64            wait_ns;        // time spent waiting for USB IN transfers to finish
    u64            out_ns;         // time spent waiting for USB OUT (host -> device) data
    u64            write_ns;       // time spent in backend writes
    u32            write_cmds;     // SCSI WRITE commands served
    u32            max_write_bytes;// largest single WRITE command
    u32            read_cmds;      // SCSI READ commands served
    u32            max_read_bytes; // largest single READ command
    u32            ra_hits;        // chunks served from the read-ahead buffer
    u64            last_write_tick;// armGetSystemTick() of the last data written to the backend
    u32            sync_cmds;      // SYNCHRONIZE CACHE commands that succeeded
    u32            eject_cmds;     // START STOP UNIT with eject (safe removal) that succeeded
    bool           host_locked;    // the host has locked the medium (PREVENT MEDIUM REMOVAL): it has mounted the volume
    u32            reads_at_config;// read_cmds when the host last configured the device, to count reads since then
} MscStats;

Result mscInit(Backend* be);
void   mscExit(void);

// Writes from the PC are acknowledged as soon as they are in RAM and reach the backend a moment later on a second thread.
// mscFlush() returns once every acknowledged write has been handed to the backend; call it before touching the backend
// yourself (a commit, an eject). It returns false if a deferred write failed.
bool   mscFlush(void);

// Writes the PC's current run of consecutive write sectors to the log (for studying how the PC writes).
void   mscLogWriteRun(void);

// Service the bus for up to timeout_ns. Handles at most one SCSI command per call.
void mscPoll(u64 timeout_ns);

// While busy (a commit is copying to the card, or waiting for a decision), every command except INQUIRY and
// REQUEST SENSE is answered NOT READY / becoming ready without touching the backend. Call mscPoll() from
// the busy code so the host keeps getting answers instead of timing the drive out.
void mscSetBusy(bool busy);

const MscStats* mscStats(void);
