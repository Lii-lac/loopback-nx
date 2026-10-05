#include "backend.h"

#include <stdlib.h>
#include <string.h>

// Fixed geometry: 16 MiB, 512 B sectors, 4 sectors/cluster, FAT16.
// 32768 sectors: 1 reserved + 2*32 FAT + 32 root dir => data starts at 97, 8167 clusters.
#define SECTOR_SIZE   512u
#define TOTAL_SECTORS 32768u
#define SPC           4u
#define RESERVED      1u
#define FAT_SECTORS   32u
#define ROOT_ENTRIES  512u
#define ROOT_SECTORS  (ROOT_ENTRIES * 32u / SECTOR_SIZE)
#define FAT0_LBA      RESERVED
#define ROOT_LBA      (RESERVED + 2u * FAT_SECTORS)
#define DATA_LBA      (ROOT_LBA + ROOT_SECTORS)

static const char README[] =
    "Loopback RAM disk\r\n"
    "\r\n"
    "If you can read this, USB mass storage over usb:ds works.\r\n"
    "This drive lives in the Switch's RAM and vanishes when the app exits.\r\n"
    "Copy a file in and back out to test writes.\r\n";

static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void dirEntry(uint8_t* e, const char name83[11], uint8_t attr, uint16_t cluster, uint32_t size) {
    memset(e, 0, 32);
    memcpy(e, name83, 11);
    e[11] = attr;
    uint16_t date = ((2026 - 1980) << 9) | (10 << 5) | 3;
    put16(e + 24, date);              // last write date
    put16(e + 22, 12 << 11);          // last write time 12:00
    put16(e + 26, cluster);
    put32(e + 28, size);
}

static void format(uint8_t* d) {
    uint8_t* bs = d;
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
    memcpy(bs + 3, "NXUSBMNT", 8);
    put16(bs + 11, SECTOR_SIZE);
    bs[13] = SPC;
    put16(bs + 14, RESERVED);
    bs[16] = 2;
    put16(bs + 17, ROOT_ENTRIES);
    put16(bs + 19, TOTAL_SECTORS);
    bs[21] = 0xF8;
    put16(bs + 22, FAT_SECTORS);
    put16(bs + 24, 63);
    put16(bs + 26, 255);
    bs[36] = 0x80;
    bs[38] = 0x29;
    put32(bs + 39, 0x4E585553);
    memcpy(bs + 43, "NXUSB RAM  ", 11);
    memcpy(bs + 54, "FAT16   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;

    for (int f = 0; f < 2; f++) {
        uint8_t* fat = d + (FAT0_LBA + f * FAT_SECTORS) * SECTOR_SIZE;
        put16(fat + 0, 0xFFF8);
        put16(fat + 2, 0xFFFF);
        put16(fat + 4, 0xFFFF);  // cluster 2 = README, end of chain
    }

    uint8_t* root = d + ROOT_LBA * SECTOR_SIZE;
    dirEntry(root, "NXUSB RAM  ", 0x08, 0, 0);  // volume label
    dirEntry(root + 32, "README  TXT", 0x20, 2, sizeof(README) - 1);
    memcpy(d + DATA_LBA * SECTOR_SIZE, README, sizeof(README) - 1);
}

static bool ramRead(void* ctx, uint64_t lba, uint32_t count, void* out) {
    memcpy(out, (uint8_t*)ctx + lba * SECTOR_SIZE, (size_t)count * SECTOR_SIZE);
    return true;
}

static bool ramWrite(void* ctx, uint64_t lba, uint32_t count, const void* in) {
    memcpy((uint8_t*)ctx + lba * SECTOR_SIZE, in, (size_t)count * SECTOR_SIZE);
    return true;
}

static bool ramFlush(void* ctx) { (void)ctx; return true; }

Backend* ramBackendCreate(void) {
    Backend* be = calloc(1, sizeof(*be));
    uint8_t* disk = calloc(TOTAL_SECTORS, SECTOR_SIZE);
    if (!be || !disk) { free(be); free(disk); return NULL; }
    format(disk);
    be->name = "RAM disk (16 MiB)";
    be->block_size = SECTOR_SIZE;
    be->block_count = TOTAL_SECTORS;
    be->read_only = false;
    be->ctx = disk;
    be->read = ramRead;
    be->write = ramWrite;
    be->flush = ramFlush;
    return be;
}

void ramBackendDestroy(Backend* be) {
    if (!be) return;
    free(be->ctx);
    free(be);
}
