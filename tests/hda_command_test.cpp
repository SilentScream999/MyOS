#include <cassert>
#include <cstdint>
#include "../src/drivers/hda_command.h"

struct Hardware {
    uint16_t status = 2; // stale valid response from an earlier command
    uint32_t command = 0, response = 0x10ec0257;
    unsigned writes = 0, acknowledgements = 0;
    bool complete = true, unsolicited = false, wrong_codec = false, stuck_busy = false;
    bool run(uint32_t& output) {
        return hda_immediate_command(0x200f0000, output,
            [&](unsigned reg) { assert(reg == 0x68); return status; },
            [&](unsigned reg, uint16_t value) {
                assert(reg == 0x68);
                if (value == 2) { status &= ~2; ++acknowledgements; }
                else {
                    assert(value == 1 && !(status & 3) && writes == 1);
                    status = 1;
                    if (complete) status = 2 | 4 | (unsolicited ? 8 : 0) | (wrong_codec ? 0x10 : 0x20);
                }
            },
            [&](unsigned reg) { assert(reg == 0x64 && (status & 3) == 2); return response; },
            [&](unsigned reg, uint32_t value) {
                assert(reg == 0x60 && !(status & 3)); command = value; ++writes;
            },
            [&](auto read, uint32_t mask, uint32_t expected) {
                for (unsigned i = 0; i < 3; ++i) if ((read() & mask) == expected && !stuck_busy) return true;
                return false;
            });
    }
};
int main() {
    uint32_t result = 0xdeadbeef;
    Hardware normal;
    assert(normal.run(result) && result == 0x10ec0257 && normal.command == 0x200f0000);
    assert(normal.acknowledgements == 2 && !(normal.status & 3));
    Hardware absent; absent.complete = false; result = 0xdeadbeef;
    assert(!absent.run(result) && result == 0xdeadbeef && absent.writes == 1);
    Hardware busy; busy.status = 1; busy.stuck_busy = true;
    assert(!busy.run(result) && busy.writes == 0 && result == 0xdeadbeef);
    Hardware unsolicited; unsolicited.unsolicited = true;
    assert(!unsolicited.run(result) && result == 0xdeadbeef);
    Hardware wrong; wrong.wrong_codec = true;
    assert(!wrong.run(result) && result == 0xdeadbeef);
}
