#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/drivers/scsi_retry.h"

struct reply { int status; uint8_t key = 0, asc = 0, ascq = 0; };
struct fake_disk {
    std::vector<reply> replies;
    unsigned commands = 0, senses = 0, reports = 0, pauses = 0;
    uint64_t now = 0;
    reply last{};
    uint8_t format = 0x70;
    bool fail_sense = false;
};
static int command(void* p, const uint8_t* cdb, uint8_t, bool, void* data, uint32_t bytes) {
    auto& disk = *(fake_disk*)p;
    ++disk.now;
    if (cdb[0] == 3) {
        ++disk.senses;
        if (disk.fail_sense) return -1;
        assert(bytes == 18);
        auto* sense = (uint8_t*)data;
        memset(sense, 0, bytes); sense[0] = disk.format;
        if (disk.format == 0x72) {
            sense[1] = disk.last.key; sense[2] = disk.last.asc; sense[3] = disk.last.ascq;
        } else {
            sense[2] = disk.last.key; sense[7] = 10;
            sense[12] = disk.last.asc; sense[13] = disk.last.ascq;
        }
        return 0;
    }
    assert(cdb[0] == 0 || cdb[0] == 0x25 || cdb[0] == 0x28);
    disk.last = disk.replies[disk.commands < disk.replies.size() ? disk.commands : disk.replies.size()-1];
    ++disk.commands;
    if (!disk.last.status && data) memset(data, 0x42, bytes);
    return disk.last.status;
}
static uint64_t now(void* p) { return ((fake_disk*)p)->now; }
static void pause(void* p, uint32_t ms) { auto& d = *(fake_disk*)p; ++d.pauses; d.now += ms; }
static void report(void* p, uint8_t, const scsi_sense*) { ++((fake_disk*)p)->reports; }
static bool run(fake_disk& d, uint8_t opcode = 0, bool startup = true, uint32_t budget = 5000) {
    scsi_retry_ops ops = {&d, command, now, pause, report};
    uint8_t cdb[10] = {}; cdb[0] = opcode;
    uint8_t data[512] = {};
    bool ok = scsi_read_with_retry(ops, cdb, 10, opcode != 0, opcode ? data : nullptr,
                                   opcode ? sizeof(data) : 0, budget, startup);
    if (ok && opcode) for (uint8_t byte : data) assert(byte == 0x42);
    return ok;
}
int main() {
    fake_disk healthy{{{0}}};
    assert(run(healthy) && healthy.commands == 1 && healthy.pauses == 0 && healthy.senses == 0);
    fake_disk cold;
    cold.replies.assign(8, {1,2,4,1}); cold.replies.push_back({0});
    assert(run(cold) && cold.commands == 9 && cold.now > 2000 && cold.now < 5000);
    fake_disk reset{{{1,6,0x29,0},{0}}};
    assert(run(reset, 0x25) && reset.commands == 2 && reset.pauses == 0);
    fake_disk read_retry{{{1,2,4,7},{0}}};
    assert(run(read_retry, 0x28, false, 1000) && read_retry.commands == 2);
    fake_disk unavailable{{{1,2,4,1}}};
    assert(!run(unavailable) && unavailable.now >= 5000 && unavailable.now < 5250 && unavailable.commands <= 20);
    fake_disk persistent_attention{{{1,6,0x29,0}}};
    assert(!run(persistent_attention) && persistent_attention.commands == 32);
    for (reply fatal : {reply{1,3,0x11,0}, reply{1,5,0x20,0}, reply{1,2,0x3a,0}, reply{1,2,4,3}}) {
        fake_disk bad{{fatal}};
        assert(!run(bad) && bad.commands == 1 && bad.senses == 1 && bad.pauses == 0);
    }
    fake_disk transport{{{-1}}};
    assert(!run(transport) && transport.commands == 1 && transport.senses == 0);
    fake_disk changed{{{1,6,0x28,0},{0}}};
    assert(!run(changed, 0x28, false) && changed.commands == 1);
    fake_disk startup_changed{{{1,6,0x28,0},{0}}};
    assert(run(startup_changed) && startup_changed.commands == 2);
    fake_disk descriptor{{{1,6,0x29,0},{0}}}; descriptor.format = 0x72;
    assert(run(descriptor));
    for (uint8_t format : {0,0x71,0x73}) {
        fake_disk invalid{{{1,6,0x29,0},{0}}}; invalid.format = format;
        assert(!run(invalid) && invalid.commands == 1);
    }
    fake_disk sense_failed{{{1,6,0x29,0},{0}}}; sense_failed.fail_sense = true;
    assert(!run(sense_failed) && sense_failed.commands == 1);
    fake_disk write{{{0}}};
    assert(!run(write, 0x2a) && !write.commands);
    puts("SCSI retry tests passed: cold startup, reset attention, transient reads, fatal/transport/media-change failures, malformed/deferred sense, deadlines and no write replay");
}
