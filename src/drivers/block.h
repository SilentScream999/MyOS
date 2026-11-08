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
    // Optional acceleration. A successful call fills all count sectors.
    bool (*read_sectors)(block_device*, uint64_t lba, uint32_t count, void* data);
};

static bool block_read(block_device* d, uint64_t lba, void* buf) {
    return d && d->online && lba < d->sectors && d->read(d, lba, buf);
}
static bool block_read_sectors(block_device* d, uint64_t lba, uint32_t count, void* buf) {
    if (!d || !d->online || !buf || !count || lba >= d->sectors || count > d->sectors - lba) return false;
    if (d->read_sectors) return d->read_sectors(d, lba, count, buf);
    if (!d->read) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!block_read(d, lba + i, (uint8_t*)buf + (uint64_t)i * 512)) return false;
    return true;
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
