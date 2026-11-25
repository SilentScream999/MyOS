#ifndef WYZAR_HDA_COMMAND_H
#define WYZAR_HDA_COMMAND_H
#include <stdint.h>

// Optional HDA PIO command transport. The caller must stop CORB/RIRB and reset
// the link before switching transports so a late ring response cannot be reused.
template<class Read16, class Write16, class Read32, class Write32, class Wait>
static bool hda_immediate_command(uint32_t command, uint32_t& result,
                                  Read16 read16, Write16 write16,
                                  Read32 read32, Write32 write32, Wait wait) {
    if (!wait([&] { return read16(0x68); }, 1u, 0u)) return false;
    write16(0x68, 2); // W1C: discard the previous result before submitting.
    write32(0x60, command);
    write16(0x68, 1);
    if (!wait([&] { return read16(0x68); }, 3u, 2u)) return false;
    uint16_t status = read16(0x68);
    // ICVER advertises codec-address and unsolicited-response status fields.
    if ((status & 4) && ((status & 8) || ((status >> 4) & 15) != (command >> 28))) return false;
    result = read32(0x64);
    write16(0x68, 2);
    return true;
}
#endif
