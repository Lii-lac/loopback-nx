// Block device abstraction. USB/SCSI code only ever talks to this.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct Backend {
    const char* name;
    uint32_t    block_size;
    uint64_t    block_count;
    bool        read_only;
    bool        media_changed;  // set by the backend when the volume was replaced; the SCSI layer reports it once
    void*       ctx;
    bool (*read)(void* ctx, uint64_t lba, uint32_t count, void* out);
    bool (*write)(void* ctx, uint64_t lba, uint32_t count, const void* in);
    bool (*flush)(void* ctx);
} Backend;

// 16 MiB FAT16 RAM disk with a README.TXT on it, for checking the cable.
Backend* ramBackendCreate(void);
void     ramBackendDestroy(Backend* be);
