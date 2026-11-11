#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#define STORAGE_HOST_TEST
#include "../src/drivers/block.h"
static constexpr uint32_t USB_STORAGE_DMA_BYTES = 65536;
struct usb_storage {};
struct command { uint64_t lba; uint32_t count; const void* data; };
static std::vector<command> commands;
static int fail_at = -1;
static void usb_scsi_put32(uint8_t* p, uint32_t n) {
    p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n;
}
static int usb_storage_command(usb_storage*, const uint8_t* cdb, uint8_t len,
                               bool in, void* data, uint32_t bytes) {
    assert(cdb[0] == 0x2a && len == 10 && !in);
    uint32_t count = (cdb[7] << 8) | cdb[8];
    uint64_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 |
                   (uint32_t)cdb[4] << 8 | cdb[5];
    assert(count && count <= 128 && bytes == count * 512);
    commands.push_back({lba, count, data});
    return (int)commands.size() == fail_at ? -1 : 0;
}
#include "../.storage-test/usb_storage_write_under_test.h"
static unsigned fallback_calls;
static bool fallback_write(block_device*, uint64_t, const void*) { ++fallback_calls; return true; }
int main() {
    usb_storage storage;
    block_device disk{}; disk.context = &storage; disk.online = true;
    disk.sectors = 0x100000100ULL; disk.write = usb_disk_write; disk.write_sectors = usb_disk_write_sectors;
    std::vector<uint8_t> data(300 * 512);
    assert(block_write_sectors(&disk, 7, 300, data.data()));
    assert(commands.size() == 3);
    assert(commands[0].lba == 7 && commands[0].count == 128 && commands[0].data == data.data());
    assert(commands[1].lba == 135 && commands[1].count == 128 && commands[1].data == data.data() + 65536);
    assert(commands[2].lba == 263 && commands[2].count == 44 && commands[2].data == data.data() + 131072);
    commands.clear(); fail_at = 2;
    assert(!block_write_sectors(&disk, 7, 300, data.data()) && commands.size() == 2);
    commands.clear(); fail_at = -1;
    assert(!usb_disk_write_sectors(&disk, 0xffffffffULL, 2, data.data()));
    assert(!usb_disk_write_sectors(&disk, 0x100000000ULL, 1, data.data()));
    assert(!block_write_sectors(&disk, disk.sectors - 1, 2, data.data()));
    assert(!block_write_sectors(&disk, 0, 0, data.data()));
    disk.online = false;
    assert(!block_write_sectors(&disk, 0, 1, data.data()) && commands.empty());
    disk.online = true;
    assert(usb_disk_write(&disk, 0xffffffffULL, data.data()));
    assert(commands.size() == 1 && commands[0].count == 1);
    disk.write_sectors = nullptr; disk.write = fallback_write;
    assert(block_write_sectors(&disk, 3, 4, data.data()) && fallback_calls == 4);
    puts("Actual USB writes: 64 KiB batches, offsets, bounds, no retry and fallback passed");
}
