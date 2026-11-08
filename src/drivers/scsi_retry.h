#ifndef WYZAR_SCSI_RETRY_H
#define WYZAR_SCSI_RETRY_H
#include <stdint.h>

struct scsi_sense { uint8_t key, asc, ascq; };
static bool scsi_decode_sense(const uint8_t* data, uint32_t size, scsi_sense* out) {
    if (!data || !out || size < 8) return false;
    uint8_t format = data[0] & 0x7f;
    // Current (not deferred) fixed and descriptor sense formats.
    if (format == 0x70 && size >= 14 && data[7] >= 6) {
        *out = {(uint8_t)(data[2] & 15), data[12], data[13]};
        return true;
    }
    if (format == 0x72) {
        *out = {(uint8_t)(data[1] & 15), data[2], data[3]};
        return true;
    }
    return false;
}

static bool scsi_transient_sense(const scsi_sense& sense, bool startup) {
    // T10 ASC/ASCQ: https://www.t10.org/lists/asc-num.htm
    // Reset attention can be consumed. Media-change attention is accepted only
    // before mounting, never silently rebinding a live filesystem to new media.
    if (sense.key == 6) return sense.asc == 0x29 || (startup && sense.asc == 0x28);
    return sense.key == 2 && sense.asc == 4 && (sense.ascq == 1 || sense.ascq == 7);
}

struct scsi_retry_ops {
    void* context;
    int (*command)(void*, const uint8_t*, uint8_t, bool, void*, uint32_t);
    uint64_t (*now_ms)(void*);
    void (*pause_ms)(void*, uint32_t);
    void (*report)(void*, uint8_t opcode, const scsi_sense*); // null sense = invalid response
};

// Only safe read/probe commands may be retried. Transport failure requires BOT
// recovery and must not reuse outstanding DMA/rings; writes are never retried.
static bool scsi_read_with_retry(const scsi_retry_ops& ops, const uint8_t* cdb,
                                uint8_t length, bool in, void* data, uint32_t bytes,
                                uint32_t budget_ms, bool startup) {
    if (!cdb || !length || (cdb[0] != 0 && cdb[0] != 0x25 && cdb[0] != 0x28)) return false;
    uint64_t start = ops.now_ms(ops.context);
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        int status = ops.command(ops.context, cdb, length, in, data, bytes);
        if (status == 0) return true;
        if (status != 1) return false;
        uint8_t request[6] = {3, 0, 0, 0, 18, 0};
        uint8_t response[18];
        for (unsigned i = 0; i < sizeof(response); ++i) response[i] = 0;
        if (ops.command(ops.context, request, 6, true, response, sizeof(response)) != 0) return false;
        scsi_sense sense;
        bool valid = scsi_decode_sense(response, sizeof(response), &sense);
        if (ops.report) ops.report(ops.context, cdb[0], valid ? &sense : nullptr);
        uint64_t elapsed = ops.now_ms(ops.context) - start;
        if (!valid || !scsi_transient_sense(sense, startup) || elapsed >= budget_ms || attempt == 31) return false;
        if (sense.key == 2) {
            uint32_t delay = budget_ms - elapsed < 250 ? budget_ms - elapsed : 250;
            ops.pause_ms(ops.context, delay);
            if (ops.now_ms(ops.context) - start >= budget_ms) return false;
        }
    }
    return false;
}
#endif
