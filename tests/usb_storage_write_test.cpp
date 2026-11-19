#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#define STORAGE_HOST_TEST
#include "../src/drivers/block.h"
#include "../src/drivers/scsi_retry.h"
static constexpr uint32_t USB_STORAGE_DMA_BYTES = 65536;
struct usb_storage { bool sync_unsupported = false; };
static void print(char*) {}
struct command { uint64_t lba; uint32_t count; const void* data; };
static std::vector<command> commands;
static int fail_at = -1;
static int failure_status = -1;
static unsigned sense_commands, failure_reports;
static bool fail_flush;
static unsigned flush_commands;
static int flush_status = 1, sense_status = 0;
static scsi_sense response_sense{7,0x27,0};
static uint8_t response_format = 0x70;
static void usb_scsi_retry_report(void*, uint8_t, const scsi_sense* sense) {
    if(response_format==0x70||response_format==0x72)
        assert(sense && sense->key == response_sense.key && sense->asc == response_sense.asc && sense->ascq == response_sense.ascq);
    else assert(!sense);
    ++failure_reports;
}
static void usb_scsi_put32(uint8_t* p, uint32_t n) {
    p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n;
}
static int usb_storage_command(usb_storage*, const uint8_t* cdb, uint8_t len,
                               bool in, void* data, uint32_t bytes) {
    if(cdb[0]==3){
        assert(len==6&&in&&bytes==18);++sense_commands;
        if(sense_status)return sense_status;
        memset(data,0,bytes);auto* response=(uint8_t*)data;
        response[0]=response_format;
        if(response_format==0x72){response[1]=response_sense.key;response[2]=response_sense.asc;response[3]=response_sense.ascq;}
        else{response[2]=response_sense.key;response[7]=10;response[12]=response_sense.asc;response[13]=response_sense.ascq;}
        return 0;
    }
    if(cdb[0]==0x35){assert(len==10&&!in&&!data&&!bytes);++flush_commands;return fail_flush?flush_status:0;}
    assert(cdb[0] == 0x2a && len == 10 && !in);
    uint32_t count = (cdb[7] << 8) | cdb[8];
    uint64_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 |
                   (uint32_t)cdb[4] << 8 | cdb[5];
    assert(count && count <= 128 && bytes == count * 512);
    commands.push_back({lba, count, data});
    return (int)commands.size() == fail_at ? failure_status : 0;
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
    assert(sense_commands==0&&failure_reports==0);
    commands.clear();failure_status=1;fail_at=1;
    assert(!block_write_sectors(&disk,7,300,data.data())&&commands.size()==1);
    assert(sense_commands==1&&failure_reports==1);
    assert(usb_disk_flush(&disk));fail_flush=true;
    assert(!usb_disk_flush(&disk)&&sense_commands==2&&failure_reports==2);
    assert(!storage.sync_unsupported);
    for(uint8_t format:{0x70,0x72}){
        response_format=format;response_sense={5,0x20,0};
        assert(usb_disk_flush(&disk)&&storage.sync_unsupported&&disk.flush_unsupported);
        unsigned before=flush_commands,senses=sense_commands;
        assert(usb_disk_flush(&disk)&&flush_commands==before&&sense_commands==senses);
        disk.online=false;assert(!usb_disk_flush(&disk)&&flush_commands==before);disk.online=true;
        storage.sync_unsupported=false;
    }
    response_format=0x70;
    for(scsi_sense failure:{scsi_sense{3,0x0c,0},scsi_sense{2,0x3a,0},scsi_sense{6,0x28,0},
                           scsi_sense{5,0x24,0},scsi_sense{5,0x20,1}}){
        response_sense=failure;assert(!usb_disk_flush(&disk)&&!storage.sync_unsupported);
    }
    response_sense={5,0x20,0};response_format=0x71;
    assert(!usb_disk_flush(&disk)&&!storage.sync_unsupported);
    response_format=0x70;sense_status=-1;
    assert(!usb_disk_flush(&disk)&&!storage.sync_unsupported);sense_status=0;
    flush_status=-1;unsigned before=sense_commands;
    assert(!usb_disk_flush(&disk)&&sense_commands==before&&!storage.sync_unsupported);flush_status=1;
    // Unsupported WRITE(10) must still fail, even after flush compatibility.
    commands.clear();fail_at=1;storage.sync_unsupported=true;
    assert(!block_write_sectors(&disk,7,1,data.data())&&storage.sync_unsupported&&commands.size()==1);
    storage.sync_unsupported=false;
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
