#include "usb_msc.h"

#include <malloc.h>
#include <string.h>

#include "log.h"
#include "scsi.h"

// Endpoint addresses as seen by the host. If enumeration fails on hardware, the first thing
// to try is the libnx-style "direction only" form (0x80 / 0x00).
#define EP_IN_ADDR   0x81
#define EP_OUT_ADDR  0x01
#define SS_MAX_BURST 0x0F

#define CHUNK_BYTES     (256u * 1024u)
#define XFER_TIMEOUT_NS (5ULL * 1000000000ULL)
#define CBW_POST_BYTES  512u
#define CBW_SIG         0x43425355u  // "USBC"
#define CSW_SIG         0x53425355u  // "USBS"

#define VID 0x057E  // Nintendo, same as libnx usbcomms
#define PID 0x3100

typedef struct NX_PACKED {
    u32 sig, tag, len;
    u8  flags, lun, cb_len;
    u8  cb[16];
} Cbw;

typedef struct NX_PACKED {
    u32 sig, tag, residue;
    u8  status;
} Csw;

static Backend*        g_be;
static UsbDsInterface* g_if;
static UsbDsEndpoint*  g_in;
static UsbDsEndpoint*  g_out;
static u8*             g_cbw;   // page-aligned: usb:ds buffers must be
static u8*             g_csw;
static u8*             g_data;
static u8*             g_data2;  // second data buffer so a backend read overlaps the previous USB send
static u8*             g_ctrl;
static MscStats        g_stats;
static bool            g_ready;
static bool            g_cbw_posted;
static bool            g_halted;       // invalid CBW seen, waiting for reset recovery
static bool            g_cfg_logged;
static u32             g_cbw_urb;
static u32             g_rw_logged;
static bool            g_busy;         // commit in progress: answer NOT READY, never touch the backend

// ---------------------------------------------------------------------------------------------
// Descriptors

static const u8 g_bos[] = {
    5, USB_DT_BOS, 22, 0, 2,                                  // BOS header, 2 capabilities
    7, USB_DT_DEVICE_CAPABILITY, 2, 0x02, 0, 0, 0,            // USB 2.0 extension (LPM)
    10, USB_DT_DEVICE_CAPABILITY, 3, 0, 0x0E, 0, 0x01, 0x0A, 0x00, 0x01,  // SuperSpeed capability
};

static size_t buildConfig(u8* out, UsbDeviceSpeed speed) {
    u16 mps = speed == UsbDeviceSpeed_Super ? 1024 : speed == UsbDeviceSpeed_High ? 512 : 64;
    size_t n = 0;

    struct usb_interface_descriptor itf = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = USBDS_DEFAULT_InterfaceNumber,
        .bAlternateSetting = 0,
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_MASS_STORAGE,
        .bInterfaceSubClass = 0x06,  // SCSI transparent command set
        .bInterfaceProtocol = 0x50,  // Bulk-Only Transport
        .iInterface = 0,
    };
    memcpy(out + n, &itf, sizeof(itf)); n += sizeof(itf);

    u8 addrs[2] = { EP_IN_ADDR, EP_OUT_ADDR };
    for (int i = 0; i < 2; i++) {
        struct usb_endpoint_descriptor ep = {
            .bLength = USB_DT_ENDPOINT_SIZE,
            .bDescriptorType = USB_DT_ENDPOINT,
            .bEndpointAddress = addrs[i],
            .bmAttributes = 0x02,  // bulk
            .wMaxPacketSize = mps,
            .bInterval = 0,
        };
        memcpy(out + n, &ep, USB_DT_ENDPOINT_SIZE); n += USB_DT_ENDPOINT_SIZE;
        if (speed == UsbDeviceSpeed_Super) {
            struct usb_ss_endpoint_companion_descriptor ss = {
                .bLength = USB_DT_SS_ENDPOINT_COMPANION_SIZE,
                .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
                .bMaxBurst = SS_MAX_BURST,
                .bmAttributes = 0,
                .wBytesPerInterval = 0,
            };
            memcpy(out + n, &ss, USB_DT_SS_ENDPOINT_COMPANION_SIZE); n += USB_DT_SS_ENDPOINT_COMPANION_SIZE;
        }
    }
    return n;
}

static Result setupUsb(void) {
    Result rc = usbDsInitialize();
    if (R_FAILED(rc)) { lg("usbDsInitialize: %08x", rc); return rc; }

    rc = usbDsClearDeviceData();
    if (R_FAILED(rc)) { lg("usbDsClearDeviceData: %08x", rc); return rc; }

    u8 iLang, iMan, iProd, iSer;
    static const u16 langs[1] = { 0x0409 };
    rc = usbDsAddUsbLanguageStringDescriptor(&iLang, langs, 1);
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iMan, "Loopback");
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iProd, "Switch SD Mount");
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iSer, "NXUSB0001");
    if (R_FAILED(rc)) { lg("string descriptors: %08x", rc); return rc; }

    static const struct { UsbDeviceSpeed speed; u16 bcd; u8 ep0; } speeds[] = {
        { UsbDeviceSpeed_Full,  0x0110, 64 },
        { UsbDeviceSpeed_High,  0x0200, 64 },
        { UsbDeviceSpeed_Super, 0x0300, 9 },
    };
    for (size_t i = 0; i < 3; i++) {
        struct usb_device_descriptor dd = {
            .bLength = USB_DT_DEVICE_SIZE,
            .bDescriptorType = USB_DT_DEVICE,
            .bcdUSB = speeds[i].bcd,
            .bDeviceClass = 0,
            .bDeviceSubClass = 0,
            .bDeviceProtocol = 0,
            .bMaxPacketSize0 = speeds[i].ep0,
            .idVendor = VID,
            .idProduct = PID,
            .bcdDevice = 0x0100,
            .iManufacturer = iMan,
            .iProduct = iProd,
            .iSerialNumber = iSer,
            .bNumConfigurations = 1,
        };
        rc = usbDsSetUsbDeviceDescriptor(speeds[i].speed, &dd);
        if (R_FAILED(rc)) { lg("device desc speed %d: %08x", speeds[i].speed, rc); return rc; }
    }

    rc = usbDsSetBinaryObjectStore(g_bos, sizeof(g_bos));
    if (R_FAILED(rc)) { lg("BOS: %08x", rc); return rc; }

    rc = usbDsRegisterInterface(&g_if);
    if (R_FAILED(rc)) { lg("RegisterInterface: %08x", rc); return rc; }

    u8 cfg[128];
    for (size_t i = 0; i < 3; i++) {
        size_t n = buildConfig(cfg, speeds[i].speed);
        rc = usbDsInterface_AppendConfigurationData(g_if, speeds[i].speed, cfg, n);
        if (R_FAILED(rc)) { lg("AppendConfig speed %d: %08x", speeds[i].speed, rc); return rc; }
    }

    rc = usbDsInterface_RegisterEndpoint(g_if, &g_in, EP_IN_ADDR);
    if (R_SUCCEEDED(rc)) rc = usbDsInterface_RegisterEndpoint(g_if, &g_out, EP_OUT_ADDR);
    if (R_FAILED(rc)) { lg("RegisterEndpoint: %08x", rc); return rc; }

    rc = usbDsInterface_EnableInterface(g_if);
    if (R_FAILED(rc)) { lg("EnableInterface: %08x", rc); return rc; }

    rc = usbDsEnable();
    if (R_FAILED(rc)) { lg("usbDsEnable: %08x", rc); return rc; }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Transfers. All blocking with a timeout; a timeout cancels the endpoint.

static Result xferWait(UsbDsEndpoint* ep, u32 urb, u32* got) {
    Result rc = eventWait(&ep->CompletionEvent, XFER_TIMEOUT_NS);
    if (R_FAILED(rc)) {
        usbDsEndpoint_Cancel(ep);
        eventClear(&ep->CompletionEvent);
        return rc;
    }
    UsbDsReportData rd;
    rc = usbDsEndpoint_GetReportData(ep, &rd);
    if (R_SUCCEEDED(rc)) {
        u32 req = 0, tr = 0;
        rc = usbDsParseReportData(&rd, urb, &req, &tr);
        if (got) *got = tr;
    }
    return rc;
}

static Result xfer(UsbDsEndpoint* ep, void* buf, u32 size, u32* got) {
    u32 urb;
    Result rc = usbDsEndpoint_PostBufferAsync(ep, buf, size, &urb);
    if (R_FAILED(rc)) return rc;
    return xferWait(ep, urb, got);
}

static bool sendIn(u32 len) {
    Result rc = xfer(g_in, g_data, len, NULL);
    if (R_FAILED(rc)) lg("IN xfer %u: %08x", len, rc);
    return R_SUCCEEDED(rc);
}

static bool sendZeros(u32 total, u32* moved) {
    memset(g_data, 0, total < CHUNK_BYTES ? total : CHUNK_BYTES);
    u32 done = 0;
    while (done < total) {
        u32 n = total - done < CHUNK_BYTES ? total - done : CHUNK_BYTES;
        if (!sendIn(n)) { *moved += done; return false; }
        done += n;
    }
    *moved += done;
    return true;
}

static bool drainOut(u32 total) {
    u32 done = 0;
    while (done < total) {
        u32 n = total - done < CHUNK_BYTES ? total - done : CHUNK_BYTES;
        u32 got = 0;
        Result rc = xfer(g_out, g_data, n, &got);
        if (R_FAILED(rc)) { lg("OUT drain: %08x", rc); return false; }
        done += got ? got : n;
    }
    return true;
}

static bool sendCsw(u32 tag, u32 residue, u8 status) {
    Csw csw = { CSW_SIG, tag, residue, status };
    memcpy(g_csw, &csw, sizeof(csw));
    Result rc = xfer(g_in, g_csw, sizeof(csw), NULL);
    if (R_FAILED(rc)) lg("CSW: %08x", rc);
    return R_SUCCEEDED(rc);
}

// ---------------------------------------------------------------------------------------------
// SCSI command execution

// Sequential read-ahead. Windows sends 64 KiB READ commands, so one command has no second chunk to
// overlap. While a command's data is on the wire we read the next range from the backend into g_ra,
// and the next sequential command is then served from RAM.
static u8*  g_ra;
static u64  g_ra_lba;
static u32  g_ra_blocks;       // 0 = empty
static u64  g_last_end;        // lba just past the previous READ command
static bool g_last_end_valid;

static bool raCovers(u64 lba, u32 blocks) {
    return g_ra_blocks && lba >= g_ra_lba && lba + blocks <= g_ra_lba + g_ra_blocks;
}

static void raInvalidate(void) {
    g_ra_blocks = 0;
    g_last_end_valid = false;
}

// Reads up to `blocks` blocks starting at lba into g_ra, unless g_ra already holds data at lba.
static void raFill(u64 lba, u32 blocks) {
    u32 bs = g_be->block_size;
    if (g_ra_blocks && lba >= g_ra_lba && lba < g_ra_lba + g_ra_blocks) return;
    if (lba >= g_be->block_count) { g_ra_blocks = 0; return; }
    if (blocks > CHUNK_BYTES / bs) blocks = CHUNK_BYTES / bs;
    if (lba + blocks > g_be->block_count) blocks = (u32)(g_be->block_count - lba);
    u64 t0 = armGetSystemTick();
    bool ok = g_be->read(g_be->ctx, lba, blocks, g_ra);
    g_stats.read_ns += armTicksToNs(armGetSystemTick() - t0);
    g_ra_lba = lba;
    g_ra_blocks = ok ? blocks : 0;
}

// Fills dst with n bytes from the backend, or zeros after the first failure. Returns true if the
// chunk holds real data.
static bool readChunk(u64 lba, u32 n, u8* dst, bool* zero_rest, bool* cmd_failed) {
    if (!*zero_rest && raCovers(lba, n / g_be->block_size)) {
        memcpy(dst, g_ra + (lba - g_ra_lba) * g_be->block_size, n);
        g_stats.ra_hits++;
        return true;
    }
    if (!*zero_rest) {
        u64 t0 = armGetSystemTick();
        bool ok = g_be->read(g_be->ctx, lba, n / g_be->block_size, dst);
        g_stats.read_ns += armTicksToNs(armGetSystemTick() - t0);
        if (!ok) {
            lg("backend read failed lba=%llu", (unsigned long long)lba);
            scsiSetSense(SENSE_MEDIUM_ERROR, 0x11, 0x00);
            *cmd_failed = true;
            *zero_rest = true;
        }
    }
    if (*zero_rest) memset(dst, 0, n);
    return !*zero_rest;
}

// Double-buffered: chunk k goes out over USB while chunk k+1 is read from the backend.
static bool doRead(const ScsiPlan* p, u32* moved, bool* cmd_failed) {
    u32 bs = g_be->block_size;
    u64 lba = p->lba;
    u32 left = p->blocks * bs;
    bool zero_rest = false;
    g_stats.read_cmds++;
    if (left > g_stats.max_read_bytes) g_stats.max_read_bytes = left;
    bool sequential = g_last_end_valid && p->lba == g_last_end;
    u8* bufs[2] = { g_data, g_data2 };
    int cur = 0;
    u32 n = left < CHUNK_BYTES ? left : CHUNK_BYTES;
    bool real = left ? readChunk(lba, n, bufs[0], &zero_rest, cmd_failed) : false;
    while (left) {
        u32 urb;
        Result rc = usbDsEndpoint_PostBufferAsync(g_in, bufs[cur], n, &urb);
        if (R_FAILED(rc)) { lg("IN post %u: %08x", n, rc); return false; }

        u64 next_lba = lba + n / bs;
        u32 next_left = left - n;
        u32 next_n = next_left < CHUNK_BYTES ? next_left : CHUNK_BYTES;
        bool next_real = false;
        if (next_left) next_real = readChunk(next_lba, next_n, bufs[cur ^ 1], &zero_rest, cmd_failed);
        else if (sequential && !zero_rest) raFill(next_lba, n / bs);  // read ahead for the next command

        u64 t0 = armGetSystemTick();
        rc = xferWait(g_in, urb, NULL);
        g_stats.wait_ns += armTicksToNs(armGetSystemTick() - t0);
        if (R_FAILED(rc)) { lg("IN xfer %u: %08x", n, rc); return false; }

        *moved += n;
        if (real) g_stats.bytes_read += n;
        lba = next_lba; left = next_left; n = next_n; real = next_real; cur ^= 1;
    }
    g_last_end = p->lba + p->blocks;
    g_last_end_valid = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Write-behind. The PC sends 64 KiB WRITE commands, and each must be answered before it sends the next, so receiving and
// storing in turn leaves the bus idle during every card write (and the card idle during every transfer). Instead each chunk is
// copied into a 2 MiB staging buffer and acknowledged at once; a second thread stores full (or stale) buffers, so USB and the
// card work at the same time and the card sees big sequential writes. Anything that reads the backend waits for it first.

#define WB_BYTES (2u * 1024u * 1024u)

static u8*      g_wb[2];
static int      g_wb_cur;                 // buffer being filled by the USB thread
static u64      g_wb_lba;                 // where it starts
static u32      g_wb_bytes;               // how much it holds
static Thread   g_wb_thread;
static bool     g_wb_started;
static Mutex    g_wb_mtx;
static CondVar  g_wb_cv;
static int      g_wb_inflight = -1;       // buffer the worker is storing, or -1
static u64      g_wb_if_lba;
static u32      g_wb_if_bytes;
static bool     g_wb_quit;
static volatile bool g_wb_err;            // a stored write failed after it was acknowledged

static void wbWorker(void* arg) {
    (void)arg;
    mutexLock(&g_wb_mtx);
    for (;;) {
        while (g_wb_inflight < 0 && !g_wb_quit) condvarWait(&g_wb_cv, &g_wb_mtx);
        if (g_wb_inflight < 0) break;  // quitting with nothing left to store
        u8* buf = g_wb[g_wb_inflight];
        u64 lba = g_wb_if_lba;
        u32 bytes = g_wb_if_bytes;
        mutexUnlock(&g_wb_mtx);
        u64 t0 = armGetSystemTick();
        bool ok = g_be->write(g_be->ctx, lba, bytes / g_be->block_size, buf);
        g_stats.write_ns += armTicksToNs(armGetSystemTick() - t0);
        if (!ok) { lg("backend write failed lba=%llu (deferred)", (unsigned long long)lba); g_wb_err = true; }
        mutexLock(&g_wb_mtx);
        g_wb_inflight = -1;
        condvarWakeAll(&g_wb_cv);
    }
    mutexUnlock(&g_wb_mtx);
}

// Hands the buffer being filled to the worker (waiting for the worker's previous job first).
static void wbSeal(void) {
    if (!g_wb_started || !g_wb_bytes) return;
    mutexLock(&g_wb_mtx);
    while (g_wb_inflight >= 0) condvarWait(&g_wb_cv, &g_wb_mtx);
    g_wb_inflight = g_wb_cur;
    g_wb_if_lba = g_wb_lba;
    g_wb_if_bytes = g_wb_bytes;
    condvarWakeAll(&g_wb_cv);
    mutexUnlock(&g_wb_mtx);
    g_wb_cur ^= 1;
    g_wb_bytes = 0;
}

bool mscFlush(void) {
    if (!g_wb_started) return true;
    wbSeal();
    mutexLock(&g_wb_mtx);
    while (g_wb_inflight >= 0) condvarWait(&g_wb_cv, &g_wb_mtx);
    mutexUnlock(&g_wb_mtx);
    return !g_wb_err;
}

static void wbStart(void) {
    g_wb[0] = memalign(0x1000, WB_BYTES);
    g_wb[1] = memalign(0x1000, WB_BYTES);
    g_wb_cur = 0; g_wb_bytes = 0; g_wb_inflight = -1; g_wb_quit = false; g_wb_err = false;
    mutexInit(&g_wb_mtx);
    condvarInit(&g_wb_cv);
    g_wb_started = g_wb[0] && g_wb[1] && R_SUCCEEDED(threadCreate(&g_wb_thread, wbWorker, NULL, NULL, 0x10000, 0x2C, -2)) &&
                   R_SUCCEEDED(threadStart(&g_wb_thread));
    if (!g_wb_started) lg("write-behind unavailable, writing in step with USB");
}

static void wbStop(void) {
    if (g_wb_started) {
        mscFlush();
        mutexLock(&g_wb_mtx);
        g_wb_quit = true;
        condvarWakeAll(&g_wb_cv);
        mutexUnlock(&g_wb_mtx);
        threadWaitForExit(&g_wb_thread);
        threadClose(&g_wb_thread);
        g_wb_started = false;
    }
    free(g_wb[0]); free(g_wb[1]);
    g_wb[0] = g_wb[1] = NULL;
}

// Queues n bytes (already in g_data) for lba. Returns false if the backend refused earlier data.
static bool wbQueue(u64 lba, u32 n) {
    u32 bs = g_be->block_size;
    if (g_wb_bytes && (lba != g_wb_lba + g_wb_bytes / bs || g_wb_bytes + n > WB_BYTES)) wbSeal();
    if (!g_wb_bytes) g_wb_lba = lba;
    memcpy(g_wb[g_wb_cur] + g_wb_bytes, g_data, n);
    g_wb_bytes += n;
    if (g_wb_bytes >= WB_BYTES) wbSeal();
    return true;
}

// How the PC lays out its writes: runs of consecutive sectors, and where it jumps to between them.
static u64  g_wr_start, g_wr_end;
static bool g_wr_valid;
static u32  g_wr_logged;

void mscLogWriteRun(void) {
    if (!g_wr_valid || g_wr_logged >= 700) return;
    g_wr_logged++;
    lg("wrun lba=%llu sectors=%llu", (unsigned long long)g_wr_start, (unsigned long long)(g_wr_end - g_wr_start));
}

static void noteWrite(u64 lba, u32 blocks) {
    if (g_wr_valid && lba == g_wr_end) { g_wr_end += blocks; return; }
    mscLogWriteRun();
    g_wr_start = lba; g_wr_end = lba + blocks; g_wr_valid = true;
}

static bool doWrite(const ScsiPlan* p, u32* moved, bool* cmd_failed) {
    noteWrite(p->lba, p->blocks);
    u32 bs = g_be->block_size;
    u64 lba = p->lba;
    u32 left = p->blocks * bs;
    bool discard = false;
    raInvalidate();  // writes may change what the read-ahead buffer holds
    g_stats.write_cmds++;
    if (left > g_stats.max_write_bytes) g_stats.max_write_bytes = left;
    while (left) {
        u32 n = left < CHUNK_BYTES ? left : CHUNK_BYTES;
        u32 got = 0;
        u64 t0 = armGetSystemTick();
        Result rc = xfer(g_out, g_data, n, &got);
        g_stats.out_ns += armTicksToNs(armGetSystemTick() - t0);
        if (R_FAILED(rc) || got != n) {
            lg("OUT xfer want %u got %u rc %08x", n, got, rc);
            return false;
        }
        *moved += n;
        bool wrote = true;
        if (!discard) {
            if (g_wb_err) { wrote = false; g_wb_err = false; }  // an earlier chunk could not be stored: fail this command instead of piling on
            else if (g_wb_started) wrote = wbQueue(lba, n);
            else {
                u64 t1 = armGetSystemTick();
                wrote = g_be->write(g_be->ctx, lba, n / bs, g_data);
                g_stats.write_ns += armTicksToNs(armGetSystemTick() - t1);
            }
        }
        if (!wrote) {
            lg("backend write failed lba=%llu", (unsigned long long)lba);
            scsiSetSense(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
            *cmd_failed = true;
            discard = true;
        }
        if (!discard) {
            g_stats.bytes_written += n;
            g_stats.last_write_tick = armGetSystemTick();
        }
        lba += n / bs;
        left -= n;
    }
    return true;
}

static void processCbw(u32 got) {
    Cbw cbw;
    memcpy(&cbw, g_cbw, sizeof(cbw));
    if (got != sizeof(Cbw) || cbw.sig != CBW_SIG || cbw.lun != 0 || cbw.cb_len == 0 || cbw.cb_len > 16) {
        lg("bad CBW: len=%u sig=%08x lun=%u cb_len=%u", got, cbw.sig, cbw.lun, cbw.cb_len);
        usbDsEndpoint_Stall(g_in);
        usbDsEndpoint_Stall(g_out);
        g_halted = true;
        g_stats.errors++;
        return;
    }

    bool wb_fail = false;
    if (cbw.cb[0] != 0x2A && cbw.cb[0] != 0x0A && cbw.cb[0] != 0x8A) {  // anything but a WRITE sees all earlier writes
        wb_fail = !mscFlush() && cbw.cb[0] != 0x12 && cbw.cb[0] != 0x03;
        if (wb_fail) g_wb_err = false;  // reported once, on this command
    }

    ScsiPlan p;
    bool busy_reject = g_busy && cbw.cb[0] != 0x12 && cbw.cb[0] != 0x03;  // INQUIRY and REQUEST SENSE are always answered
    if (busy_reject || wb_fail) {
        memset(&p, 0, sizeof(p));
        p.kind = SCSI_NODATA;
        p.ok = false;
        if (busy_reject) scsiSetSense(0x02, 0x04, 0x01);  // NOT READY, logical unit is in process of becoming ready
        else scsiSetSense(SENSE_MEDIUM_ERROR, 0x0C, 0x00);  // a write acknowledged earlier could not be stored
    } else {
        if (g_be->media_changed) raInvalidate();  // the read-ahead buffer holds the old volume
        scsiPlan(g_be, cbw.cb, cbw.cb_len, &p);
    }

    bool host_in = cbw.flags & 0x80;
    u32 expected = cbw.len;
    u32 moved = 0;
    bool cmd_failed = !p.ok;
    bool bus_err = false;

    bool is_rw = p.kind == SCSI_READ || p.kind == SCSI_WRITE;
    // Host and device must agree on direction and length; if not, fail the command safely.
    if (p.ok && is_rw && (p.blocks * g_be->block_size != expected ||
                          host_in != (p.kind == SCSI_READ))) {
        lg("rw mismatch op=%02x exp=%u blocks=%u", cbw.cb[0], expected, p.blocks);
        scsiSetSense(0x05, 0x24, 0x00);
        cmd_failed = true;
        p.ok = false;
    }
    if (p.ok && p.kind == SCSI_DATA_IN && (!host_in || expected == 0)) {
        cmd_failed = true;
        p.ok = false;
    }

    if (expected > 0 && host_in) {
        if (p.ok && p.kind == SCSI_DATA_IN) {
            u32 n = p.resp_len < expected ? p.resp_len : expected;
            if (n) {
                memcpy(g_data, p.resp, n);
                bus_err = !sendIn(n);
                moved = n;
            } else {
                bus_err = !sendZeros(expected, &moved);
            }
        } else if (p.ok && p.kind == SCSI_READ) {
            bus_err = !doRead(&p, &moved, &cmd_failed);
        } else {
            cmd_failed = true;
            bus_err = !sendZeros(expected, &moved);
        }
    } else if (expected > 0) {
        if (p.ok && p.kind == SCSI_WRITE) {
            bus_err = !doWrite(&p, &moved, &cmd_failed);
        } else {
            cmd_failed = true;
            bus_err = !drainOut(expected);
            moved = expected;
        }
    } else if (p.ok && p.kind != SCSI_NODATA) {
        scsiSetSense(0x05, 0x24, 0x00);
        cmd_failed = true;
    }

    u8 status = bus_err ? 2 : cmd_failed ? 1 : 0;
    sendCsw(cbw.tag, moved < expected ? expected - moved : 0, status);

    g_stats.commands++;
    g_stats.last_opcode = cbw.cb[0];
    if (!cmd_failed && !bus_err && cbw.cb[0] == 0x1E) g_stats.host_locked = (cbw.cb[4] & 1) != 0;  // the PC locks the drive once it has mounted it
    if (!cmd_failed && !bus_err) {
        if (cbw.cb[0] == 0x35) g_stats.sync_cmds++;                                              // SYNCHRONIZE CACHE
        else if (cbw.cb[0] == 0x1B && (cbw.cb[4] & 0x02) && !(cbw.cb[4] & 0x01)) g_stats.eject_cmds++;  // eject
    }
    if ((cmd_failed && !busy_reject) || bus_err) g_stats.errors++;
    // Polling commands (TEST UNIT READY, PREVENT/ALLOW REMOVAL, READ CAPACITY, MODE SENSE, REQUEST SENSE)
    // would flood the 10-line screen log and push commit messages off it; log them only on failure.
    bool is_poll = cbw.cb[0] == 0x00 || cbw.cb[0] == 0x1E || cbw.cb[0] == 0x25 ||
                   cbw.cb[0] == 0x1A || cbw.cb[0] == 0x03;
    if (busy_reject && !bus_err) return;  // expected while committing; would only flood the log
    if (cmd_failed || bus_err || (is_rw ? g_rw_logged++ < 16 : !is_poll))
        lg("op %02x len %u -> %s", cbw.cb[0], expected, bus_err ? "PHASE" : cmd_failed ? "FAIL" : "ok");
}

// ---------------------------------------------------------------------------------------------
// Control requests (class-specific; the standard ones are answered by the usb sysmodule)

static void resetRecovery(void) {
    usbDsEndpoint_Cancel(g_in);
    usbDsEndpoint_Cancel(g_out);
    eventClear(&g_in->CompletionEvent);
    eventClear(&g_out->CompletionEvent);
    g_cbw_posted = false;
    g_halted = false;
}

static void ctrlIn(const void* data, u32 len) {
    if (len) memcpy(g_ctrl, data, len);
    u32 urb;
    Result rc = usbDsInterface_CtrlInPostBufferAsync(g_if, g_ctrl, len, &urb);
    if (R_SUCCEEDED(rc)) rc = eventWait(&g_if->CtrlInCompletionEvent, XFER_TIMEOUT_NS);
    if (R_SUCCEEDED(rc)) {
        UsbDsReportData rd;
        rc = usbDsInterface_GetCtrlInReportData(g_if, &rd);
    }
    if (R_FAILED(rc)) lg("ctrl IN: %08x", rc);
}

static void handleSetup(void) {
    struct usb_control_setup s;
    Result rc = usbDsInterface_GetSetupPacket(g_if, &s, sizeof(s));
    if (R_FAILED(rc)) { lg("GetSetupPacket: %08x", rc); return; }
    lg("setup type=%02x req=%02x val=%04x idx=%04x len=%u", s.bmRequestType, s.bRequest, s.wValue, s.wIndex, s.wLength);

    bool class_req = (s.bmRequestType & 0x60) == 0x20;
    if (class_req && s.bRequest == 0xFE && (s.bmRequestType & 0x80)) {  // Get Max LUN
        u8 lun = 0;
        ctrlIn(&lun, 1);
    } else if (class_req && s.bRequest == 0xFF) {                         // Bulk-Only Mass Storage Reset
        resetRecovery();
        ctrlIn(NULL, 0);
    } else {
        usbDsInterface_StallCtrl(g_if);
    }
}

// ---------------------------------------------------------------------------------------------
// Public API

Result mscInit(Backend* be) {
    g_be = be;
    // a fresh start every time the card is mounted, so it can be stopped and started again
    memset(&g_stats, 0, sizeof(g_stats));
    g_cbw_posted = false; g_halted = false; g_cfg_logged = false; g_busy = false; g_rw_logged = 0;
    g_wr_valid = false; g_wr_logged = 0;
    g_cbw = memalign(0x1000, 0x1000);
    g_csw = memalign(0x1000, 0x1000);
    g_ctrl = memalign(0x1000, 0x1000);
    g_data = memalign(0x1000, CHUNK_BYTES);
    g_data2 = memalign(0x1000, CHUNK_BYTES);
    g_ra = memalign(0x1000, CHUNK_BYTES);
    raInvalidate();
    wbStart();
    if (!g_cbw || !g_csw || !g_ctrl || !g_data || !g_data2 || !g_ra) { lg("out of memory"); return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); }

    Result rc = setupUsb();
    if (R_FAILED(rc)) { usbDsExit(); return rc; }
    g_ready = true;
    lg("usb:ds ready, waiting for host");
    return 0;
}

void mscExit(void) {
    wbStop();
    if (g_ready) {
        usbDsEndpoint_Cancel(g_in);
        usbDsEndpoint_Cancel(g_out);
        usbDsInterface_DisableInterface(g_if);
        usbDsDisable();
        usbDsExit();
        g_ready = false;
    }
    free(g_cbw); free(g_csw); free(g_ctrl); free(g_data); free(g_data2); free(g_ra);
    g_cbw = g_csw = g_ctrl = g_data = g_data2 = g_ra = NULL;
}

void mscPoll(u64 timeout_ns) {
    if (!g_ready) { svcSleepThread(timeout_ns); return; }

    UsbState st;
    if (R_SUCCEEDED(usbDsGetState(&st))) g_stats.state = st;

    if (g_stats.state != UsbState_Configured) {
        if (g_cbw_posted) {
            usbDsEndpoint_Cancel(g_out);
            eventClear(&g_out->CompletionEvent);
            g_cbw_posted = false;
        }
        g_halted = false;
        g_cfg_logged = false;
        g_stats.host_locked = false;
        g_stats.speed = UsbDeviceSpeed_None;
        wbSeal();
        eventWait(usbDsGetStateChangeEvent(), timeout_ns);
        return;
    }

    if (!g_cfg_logged) {
        UsbDeviceSpeed spd = UsbDeviceSpeed_None;
        Result rc = usbDsGetSpeed(&spd);
        g_stats.speed = R_SUCCEEDED(rc) ? spd : UsbDeviceSpeed_None;
        lg("configured by host, speed=%d (rc %08x)", (int)spd, rc);
        g_stats.reads_at_config = g_stats.read_cmds;
        g_cfg_logged = true;
    }

    if (!g_halted && !g_cbw_posted) {
        Result rc = usbDsEndpoint_PostBufferAsync(g_out, g_cbw, CBW_POST_BYTES, &g_cbw_urb);
        if (R_FAILED(rc)) { lg("post CBW: %08x", rc); svcSleepThread(timeout_ns); return; }
        g_cbw_posted = true;
    }

    int idx = -1;
    Result rc;
    if (g_halted) {
        rc = waitMulti(&idx, timeout_ns, waiterForEvent(&g_if->SetupEvent),
                       waiterForEvent(usbDsGetStateChangeEvent()));
        if (R_SUCCEEDED(rc) && idx == 0) handleSetup();
        return;
    }

    rc = waitMulti(&idx, timeout_ns, waiterForEvent(&g_out->CompletionEvent),
                   waiterForEvent(&g_if->SetupEvent), waiterForEvent(usbDsGetStateChangeEvent()));
    if (R_FAILED(rc)) { wbSeal(); return; }  // the PC is quiet: start storing what is waiting

    if (idx == 0) {
        g_cbw_posted = false;
        UsbDsReportData rd;
        u32 req = 0, got = 0;
        rc = usbDsEndpoint_GetReportData(g_out, &rd);
        if (R_SUCCEEDED(rc)) rc = usbDsParseReportData(&rd, g_cbw_urb, &req, &got);
        if (R_SUCCEEDED(rc)) processCbw(got);
        else lg("CBW report: %08x", rc);
    } else if (idx == 1) {
        handleSetup();
    }
}

void mscSetBusy(bool busy) { g_busy = busy; }

const MscStats* mscStats(void) { return &g_stats; }
