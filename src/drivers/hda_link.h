#ifndef WYZAR_HDA_LINK_H
#define WYZAR_HDA_LINK_H
#include <stdint.h>

static inline bool hda_skl_kbl_controller(uint32_t id) {
    if ((id & 65535) != 0x8086) return false;
    switch (id >> 16) {
        case 0x9d70: case 0x9d71: case 0xa170: case 0xa171: return true;
        default: return false;
    }
}
// Extended HDA capability headers carry an absolute next offset in bits 15:0.
// Bound every read to the mapped BAR and reject cycles/alignment errors.
template<class Read> static unsigned hda_find_multilink(Read read) {
    unsigned offset = read(0x14) & 65535;
    unsigned seen[48]{}, count = 0;
    while (offset) {
        if (offset < 0x100 || offset > 0x3f80 || (offset & 3) || count == 48) return 0;
        for (unsigned i = 0; i < count; ++i) if (seen[i] == offset) return 0;
        seen[count++] = offset;
        uint32_t header = read(offset);
        if (((header >> 16) & 0xfff) == 2) return offset;
        offset = header & 65535;
    }
    return 0;
}
// Legacy HDA link 0 serves external codecs on the supported SKL/KBL controllers.
// Configure only advertised clock rates and never change SPA while CPA differs.
template<class Read, class Write, class Wait, class Delay>
static bool hda_start_external_link(unsigned base, Read read, Write write, Wait wait, Delay delay) {
    unsigned link = base + 0x40;
    uint32_t caps = read(link), control = read(link + 4);
    if (caps & (1u << 28)) return false; // SoundWire/other extended-link layout
    constexpr uint32_t spa = 1u << 16, cpa = 1u << 23;
    uint32_t expected = control & spa ? cpa : 0;
    if (!wait([&] { return read(link + 4); }, cpa, expected)) return false;
    control = read(link + 4);
    if (!(control & 15)) {
        unsigned frequency = 0;
        const unsigned frequencies[] = {2, 3, 1, 4, 5};
        for (unsigned candidate : frequencies)
            if (caps & (1u << candidate)) { frequency = candidate; break; }
        if (!frequency && !(caps & 1)) return false;
        if (frequency) {
            write(link + 4, control & ~spa);
            if (!wait([&] { return read(link + 4); }, cpa, 0u)) return false;
            delay(1);
            control = (read(link + 4) & ~15u) | frequency;
            write(link + 4, control);
        }
    }
    write(link + 4, read(link + 4) | spa);
    if (!wait([&] { return read(link + 4); }, cpa, cpa)) return false;
    delay(20);
    return true;
}
#endif
