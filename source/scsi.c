#include "scsi.h"

#include <string.h>

enum {
    OP_TEST_UNIT_READY    = 0x00,
    OP_REQUEST_SENSE      = 0x03,
    OP_INQUIRY            = 0x12,
    OP_MODE_SENSE_6       = 0x1A,
    OP_START_STOP_UNIT    = 0x1B,
    OP_PREVENT_ALLOW      = 0x1E,
    OP_READ_FORMAT_CAPS   = 0x23,
    OP_READ_CAPACITY_10   = 0x25,
    OP_READ_10            = 0x28,
    OP_WRITE_10           = 0x2A,
    OP_SYNC_CACHE_10      = 0x35,
    OP_MODE_SENSE_10      = 0x5A,
    OP_READ_16            = 0x88,
    OP_WRITE_16           = 0x8A,
    OP_SERVICE_ACTION_IN  = 0x9E,
    OP_READ_12            = 0xA8,
    OP_WRITE_12           = 0xAA,
};

static struct { uint8_t key, asc, ascq; } g_sense;

void scsiSetSense(uint8_t key, uint8_t asc, uint8_t ascq) {
    g_sense.key = key; g_sense.asc = asc; g_sense.ascq = ascq;
}

static void fail(ScsiPlan* p, uint8_t key, uint8_t asc, uint8_t ascq) {
    scsiSetSense(key, asc, ascq);
    p->kind = SCSI_NODATA;
    p->ok = false;
}

static void be16(uint8_t* p, uint32_t v) { p[0] = v >> 8; p[1] = v; }
static void be32(uint8_t* p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v); }
static void be64(uint8_t* p, uint64_t v) { be32(p, v >> 32); be32(p + 4, (uint32_t)v); }
static uint32_t rd16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
static uint32_t rd32(const uint8_t* p) { return ((uint32_t)rd16(p) << 16) | rd16(p + 2); }
static uint64_t rd64(const uint8_t* p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }

static void dataIn(ScsiPlan* p, uint32_t len, uint32_t alloc_len) {
    p->kind = SCSI_DATA_IN;
    p->resp_len = len < alloc_len ? len : alloc_len;
}

static void rw(const Backend* be, ScsiPlan* p, bool write, uint64_t lba, uint32_t blocks) {
    if (lba >= be->block_count || blocks > be->block_count - lba) {
        fail(p, 0x05, 0x21, 0x00);  // ILLEGAL REQUEST, LBA out of range
        return;
    }
    if (write && be->read_only) {
        fail(p, 0x07, 0x27, 0x00);  // DATA PROTECT, write protected
        p->kind = SCSI_WRITE;       // transport still has to drain the host's data
        return;
    }
    p->kind = write ? SCSI_WRITE : SCSI_READ;
    p->lba = lba;
    p->blocks = blocks;
}

void scsiPlan(const Backend* be, const uint8_t* cdb, uint8_t cdb_len, ScsiPlan* p) {
    memset(p, 0, sizeof(*p));
    p->kind = SCSI_NODATA;
    p->ok = true;
    (void)cdb_len;

    // The backend replaced its volume (after a commit). Tell the host once, so it re-reads capacity
    // and remounts. INQUIRY and REQUEST SENSE are exempt, as SCSI requires.
    if (be->media_changed && cdb[0] != OP_INQUIRY && cdb[0] != OP_REQUEST_SENSE) {
        ((Backend*)be)->media_changed = false;
        fail(p, 0x06, 0x28, 0x00);  // UNIT ATTENTION, not ready to ready change, medium may have changed
        return;
    }

    switch (cdb[0]) {
    case OP_TEST_UNIT_READY:
    case OP_START_STOP_UNIT:
    case OP_PREVENT_ALLOW:
        break;

    case OP_SYNC_CACHE_10:
        if (be->flush && !be->flush(be->ctx)) fail(p, SENSE_MEDIUM_ERROR, 0x03, 0x00);
        break;

    case OP_REQUEST_SENSE:
        p->resp[0] = 0x70;
        p->resp[2] = g_sense.key;
        p->resp[7] = 10;
        p->resp[12] = g_sense.asc;
        p->resp[13] = g_sense.ascq;
        scsiSetSense(0, 0, 0);
        dataIn(p, 18, cdb[4]);
        break;

    case OP_INQUIRY:
        if (cdb[1] & 1) {  // EVPD pages not supported
            fail(p, 0x05, 0x24, 0x00);
            break;
        }
        p->resp[0] = 0x00;  // direct access block device
        p->resp[1] = 0x80;  // removable: Windows then accepts a partition-table-less volume
        p->resp[2] = 0x06;  // SPC-4
        p->resp[3] = 0x02;
        p->resp[4] = 31;
        memcpy(p->resp + 8,  "NXUSB   ", 8);
        memcpy(p->resp + 16, "Switch SD Mount ", 16);
        memcpy(p->resp + 32, "0.1 ", 4);
        dataIn(p, 36, rd16(cdb + 3));
        break;

    case OP_MODE_SENSE_6:
        p->resp[0] = 3;
        p->resp[2] = be->read_only ? 0x80 : 0x00;
        dataIn(p, 4, cdb[4]);
        break;

    case OP_MODE_SENSE_10:
        be16(p->resp, 6);
        p->resp[3] = be->read_only ? 0x80 : 0x00;
        dataIn(p, 8, rd16(cdb + 7));
        break;

    case OP_READ_FORMAT_CAPS:
        p->resp[3] = 8;
        be32(p->resp + 4, be->block_count > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)be->block_count);
        p->resp[8] = 0x02;  // formatted media
        p->resp[9] = be->block_size >> 16;
        p->resp[10] = be->block_size >> 8;
        p->resp[11] = be->block_size;
        dataIn(p, 12, rd16(cdb + 7));
        break;

    case OP_READ_CAPACITY_10: {
        uint64_t last = be->block_count - 1;
        be32(p->resp, last > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)last);
        be32(p->resp + 4, be->block_size);
        dataIn(p, 8, 8);
        break;
    }

    case OP_SERVICE_ACTION_IN:
        if ((cdb[1] & 0x1F) != 0x10) { fail(p, 0x05, 0x20, 0x00); break; }  // READ CAPACITY(16) only
        be64(p->resp, be->block_count - 1);
        be32(p->resp + 8, be->block_size);
        dataIn(p, 32, rd32(cdb + 10));
        break;

    case OP_READ_10:  rw(be, p, false, rd32(cdb + 2), rd16(cdb + 7)); break;
    case OP_WRITE_10: rw(be, p, true,  rd32(cdb + 2), rd16(cdb + 7)); break;
    case OP_READ_12:  rw(be, p, false, rd32(cdb + 2), rd32(cdb + 6)); break;
    case OP_WRITE_12: rw(be, p, true,  rd32(cdb + 2), rd32(cdb + 6)); break;
    case OP_READ_16:  rw(be, p, false, rd64(cdb + 2), rd32(cdb + 10)); break;
    case OP_WRITE_16: rw(be, p, true,  rd64(cdb + 2), rd32(cdb + 10)); break;

    default:
        fail(p, 0x05, 0x20, 0x00);  // ILLEGAL REQUEST, invalid command opcode
        break;
    }
}
