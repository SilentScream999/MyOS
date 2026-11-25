#include <cassert>
#include <cstdint>
#include <vector>
#include "../src/drivers/hda_link.h"

struct Link {
    uint32_t caps = 5, control = 0x04000000;
    bool stuck = false;
    std::vector<uint32_t> writes;
    unsigned delay = 0;
    bool start() {
        return hda_start_external_link(0x200,
            [&](unsigned off) { assert(off == 0x240 || off == 0x244); return off == 0x240 ? caps : control; },
            [&](unsigned off, uint32_t value) {
                assert(off == 0x244);
                // Changing SCF is legal only while powered down and acked down.
                if ((value & 15) != (control & 15)) assert(!(control & ((1u << 16) | (1u << 23))));
                writes.push_back(value);
                control = value;
                if (!stuck) control = (control & ~(1u << 23)) | ((control & (1u << 16)) ? 1u << 23 : 0);
            },
            [&](auto read, uint32_t mask, uint32_t value) { return (read() & mask) == value; },
            [&](unsigned ms) { delay += ms; });
    }
};
int main() {
    assert(hda_skl_kbl_controller(0x9d718086));
    assert(hda_skl_kbl_controller(0xa1708086));
    assert(!hda_skl_kbl_controller(0x26688086)); // no vendor writes to QEMU/ICH
    assert(!hda_skl_kbl_controller(0x9d711022));
    uint32_t registers[4096]{};
    auto read = [&](unsigned off) { assert(!(off & 3) && off < sizeof(registers)); return registers[off / 4]; };
    registers[0x14 / 4] = 0x100;
    registers[0x100 / 4] = 0x00010200;
    registers[0x200 / 4] = 0x00020000;
    assert(hda_find_multilink(read) == 0x200);
    registers[0x100 / 4] = 0x00010100;
    assert(!hda_find_multilink(read)); // cycle
    registers[0x14 / 4] = 0x101;
    assert(!hda_find_multilink(read));
    registers[0x14 / 4] = 0x4000;
    assert(!hda_find_multilink(read)); // outside mapped BAR
    registers[0x14 / 4] = 0;
    assert(!hda_find_multilink(read));
    Link cold;
    assert(cold.start() && (cold.control & 15) == 2);
    assert((cold.control & ((1u << 16) | (1u << 23))) == ((1u << 16) | (1u << 23)));
    assert(cold.control & 0x04000000);
    assert(cold.writes.size() == 3 && cold.delay == 21);
    Link warm; warm.control |= (1u << 16) | (1u << 23);
    assert(warm.start() && !(warm.writes[0] & (1u << 16)));
    Link current; current.control |= 2 | (1u << 16) | (1u << 23);
    assert(current.start() && current.writes.size() == 1 && current.delay == 20);
    Link limited; limited.caps = 2;
    assert(limited.start() && (limited.control & 15) == 1);
    Link six; six.caps = 1;
    assert(six.start() && !(six.control & 15));
    Link invalid; invalid.caps = 0;
    assert(!invalid.start() && invalid.writes.empty());
    Link unsynced; unsynced.control |= 1u << 23;
    assert(!unsynced.start() && unsynced.writes.empty());
    Link timeout; timeout.control |= (1u << 16) | (1u << 23); timeout.stuck = true;
    assert(!timeout.start() && timeout.writes.size() == 1); // no SCF write after failed CPA ack
    Link extended; extended.caps |= 1u << 28;
    assert(!extended.start() && extended.writes.empty());
}
