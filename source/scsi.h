// SCSI command decoding for a direct-access block device. Pure C, no libnx,
// so it can be unit-tested on the PC. The USB transport executes the plan.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "backend.h"

typedef enum {
    SCSI_NODATA,  // no data phase
    SCSI_DATA_IN, // device -> host from plan.resp
    SCSI_READ,    // device -> host from backend
    SCSI_WRITE,   // host -> device into backend
} ScsiKind;

typedef struct {
    ScsiKind kind;
    bool     ok;       // false => CHECK CONDITION (sense is set)
    uint8_t  resp[64];
    uint32_t resp_len;
    uint64_t lba;
    uint32_t blocks;
} ScsiPlan;

void scsiPlan(const Backend* be, const uint8_t* cdb, uint8_t cdb_len, ScsiPlan* out);

// Sense for a failure the transport itself hit (e.g. backend I/O error).
void scsiSetSense(uint8_t key, uint8_t asc, uint8_t ascq);

#define SENSE_MEDIUM_ERROR 0x03
