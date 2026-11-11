#ifndef WYZAR_BLOCK_H
#define WYZAR_BLOCK_H
#include <stdint.h>
#ifndef STORAGE_HOST_TEST
#include "scheduler.h"
#endif

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
    bool (*write_sectors)(block_device*, uint64_t lba, uint32_t count, const void* data);
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

static bool block_write_sectors(block_device* d, uint64_t lba, uint32_t count, const void* buf) {
    if (!d || !d->online || !buf || !count || lba >= d->sectors || count > d->sectors - lba) return false;
    if (d->write_sectors) return d->write_sectors(d, lba, count, buf);
    if (!d->write) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!block_write(d, lba + i, (const uint8_t*)buf + (uint64_t)i * 512)) return false;
    return true;
}

// Reentrant single-core filesystem mutex. Short ownership updates mask IRQs;
// contending tasks sleep, and the owner can sleep for USB completion with IRQs on.
#ifndef STORAGE_HOST_TEST
static Task* storage_owner;
static unsigned storage_depth;
#endif
struct storage_guard {
#ifndef STORAGE_HOST_TEST
    uint64_t flags;
    storage_guard() {
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
        while (storage_owner && storage_owner != g_current_task) task_sleep_ms(1);
        storage_owner = g_current_task; ++storage_depth;
        if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
    }
    ~storage_guard() {
        __asm__ volatile("cli" ::: "memory");
        if (!--storage_depth) storage_owner = nullptr;
        if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
    }
#else
    storage_guard() {}
#endif
};
// Reference counts/queue ownership also run from scheduler cleanup paths;
// these must never wait for a filesystem mutex.
struct storage_irq_guard {
#ifndef STORAGE_HOST_TEST
    uint64_t flags;
    storage_irq_guard() { __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory"); }
    ~storage_irq_guard() { if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory"); }
#endif
};
#endif
