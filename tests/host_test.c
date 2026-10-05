// Host-side checks for scsi.c + the RAM backend. Not part of the NRO build.
// Usage: host_test <dump.img>   (writes the RAM disk as a raw image for fsck.fat)
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/backend.h"
#include "../source/scsi.h"

static ScsiPlan plan(Backend* be, const uint8_t* cdb) {
    ScsiPlan p;
    scsiPlan(be, cdb, 16, &p);
    return p;
}

int main(int argc, char** argv) {
    Backend* be = ramBackendCreate();
    assert(be && be->block_count == 32768 && be->block_size == 512);

    // INQUIRY
    uint8_t inq[16] = { 0x12, 0, 0, 0, 36 };
    ScsiPlan p = plan(be, inq);
    assert(p.ok && p.kind == SCSI_DATA_IN && p.resp_len == 36 && p.resp[1] == 0x80);
    // INQUIRY alloc length trims
    inq[4] = 5; p = plan(be, inq); assert(p.resp_len == 5);
    // INQUIRY EVPD -> fail, then REQUEST SENSE reports ILLEGAL REQUEST / invalid field
    uint8_t evpd[16] = { 0x12, 1, 0x80, 0, 36 };
    p = plan(be, evpd); assert(!p.ok);
    uint8_t rs[16] = { 0x03, 0, 0, 0, 18 };
    p = plan(be, rs); assert(p.ok && p.resp[2] == 5 && p.resp[12] == 0x24);
    // sense is cleared after being read
    p = plan(be, rs); assert(p.resp[2] == 0);
    // READ CAPACITY(10): last lba 32767, 512
    uint8_t rc10[16] = { 0x25 };
    p = plan(be, rc10);
    assert(p.resp_len == 8 && p.resp[2] == 0x7F && p.resp[3] == 0xFF && p.resp[6] == 0x02);
    // READ CAPACITY(16)
    uint8_t rc16[16] = { 0x9E, 0x10, 0,0,0,0,0,0,0,0, 0,0,0,32 };
    p = plan(be, rc16); assert(p.ok && p.resp_len == 32 && p.resp[6] == 0x7F && p.resp[7] == 0xFF && p.resp[10] == 0x02 && p.resp[11] == 0x00);
    // READ(10) lba 0, 1 block
    uint8_t r10[16] = { 0x28, 0, 0,0,0,0, 0, 0,1 };
    p = plan(be, r10); assert(p.ok && p.kind == SCSI_READ && p.blocks == 1 && p.lba == 0);
    // READ(10) out of range
    uint8_t oor[16] = { 0x28, 0, 0,0,0x80,0, 0, 0,1 };
    p = plan(be, oor); assert(!p.ok);
    p = plan(be, rs); assert(p.resp[2] == 5 && p.resp[12] == 0x21);
    // WRITE(16)
    uint8_t w16[16] = { 0x8A, 0, 0,0,0,0,0,0,0,10, 0,0,0,4 };
    p = plan(be, w16); assert(p.ok && p.kind == SCSI_WRITE && p.lba == 10 && p.blocks == 4);
    // read-only backend
    be->read_only = true;
    p = plan(be, w16); assert(!p.ok);
    uint8_t ms6[16] = { 0x1A, 0, 0x3F, 0, 192 };
    p = plan(be, ms6); assert(p.resp[2] == 0x80);
    be->read_only = false;
    // unknown opcode
    uint8_t bad[16] = { 0xEE };
    p = plan(be, bad); assert(!p.ok);
    scsiSetSense(0, 0, 0);

    // media change: reported once with UNIT ATTENTION, INQUIRY and REQUEST SENSE are exempt
    uint8_t tur[16] = { 0x00 };
    be->media_changed = true;
    p = plan(be, inq); assert(p.ok && be->media_changed);
    p = plan(be, tur); assert(!p.ok && !be->media_changed);
    p = plan(be, rs); assert(p.ok && p.resp[2] == 6 && p.resp[12] == 0x28 && p.resp[13] == 0x00);
    p = plan(be, tur); assert(p.ok);
    be->media_changed = true;
    p = plan(be, w16); assert(!p.ok && p.kind == SCSI_NODATA);  // a write hit by the unit attention is not executed
    p = plan(be, w16); assert(p.ok && p.kind == SCSI_WRITE);
    scsiSetSense(0, 0, 0);

    // dump image
    if (argc > 1) {
        FILE* f = fopen(argv[1], "wb");
        uint8_t* buf = malloc(512 * 4096);
        for (uint64_t lba = 0; lba < be->block_count; lba += 4096) {
            assert(be->read(be->ctx, lba, 4096, buf));
            fwrite(buf, 512, 4096, f);
        }
        fclose(f);
        free(buf);
    }
    ramBackendDestroy(be);
    puts("scsi/backend host tests passed");
    return 0;
}
