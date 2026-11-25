#ifndef WYZAR_HDA_PCM_RING_H
#define WYZAR_HDA_PCM_RING_H
#include <stdint.h>
// One descriptor per page. Keep a page free so the DMA never reads a page
// while its previous contents are being reclaimed/refilled by the producer.
struct HdaPcmRing {
    static constexpr unsigned capacity = 262144, block = 4096;
    static constexpr unsigned prefill = capacity / 2;
    // Conservative bound at the fastest supported format (48 kHz stereo s16).
    static constexpr unsigned deadline_ms = 1200;
    uint64_t written = 0, consumed = 0, cleared = 0, tick = 0;
    unsigned position = 0;
    unsigned space() const {
        uint64_t queued = written > consumed ? written - consumed : 0;
        return queued >= capacity - block ? 0 : capacity - block - (unsigned)queued;
    }
    bool advance(unsigned pos, uint64_t now) {
        if (pos > capacity || now - tick >= deadline_ms) return false;
        if (pos == capacity) pos = 0; // Some controllers expose CBL at wrap.
        consumed += (pos + capacity - position) % capacity;
        position = pos; tick = now;
        return true;
    }
};
#endif
