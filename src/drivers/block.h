#ifndef WYZAR_BLOCK_H
#define WYZAR_BLOCK_H
#include <stdint.h>

// Transport-neutral, synchronous 512-byte block device. Drivers must bound waits.
struct block_device {
    void* context;
    uint64_t sectors;
    bool online;
    bool (*read)(block_device*, uint64_t lba, void* sector);
    bool (*write)(block_device*, uint64_t lba, const void* sector);
    bool (*flush)(block_device*);
};

static bool block_read(block_device* d, uint64_t lba, void* buf) {
    return d && d->online && lba < d->sectors && d->read(d, lba, buf);
}
static bool block_write(block_device* d, uint64_t lba, const void* buf) {
    return d && d->online && lba < d->sectors && d->write(d, lba, buf);
}

// This kernel is single-core. Serialize VFS transactions with USB polling and
// other filesystem users. Do not sleep with this guard held.
struct storage_guard {
#ifndef STORAGE_HOST_TEST
    uint64_t flags;
    storage_guard() { __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory"); }
    ~storage_guard() { if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory"); }
#else
    storage_guard() {}
#endif
};
#endif
