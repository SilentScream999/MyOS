#ifndef WYZAR_THINKPAD_EC_PROTOCOL_H
#define WYZAR_THINKPAD_EC_PROTOCOL_H
#include <stdint.h>

// ACPI EC byte protocol; callers serialize the complete read/modify/write with
// the firmware Global Lock. Never drain unexpected OBF data or issue QR_EC.
template<class In, class Out, class Wait>
struct ThinkpadEcProtocol {
    uint16_t data, command;
    In in; Out out; Wait wait;
    bool idle() { return wait([&] { return in(command); }, 3u, 0u); }
    bool input_ready() { return wait([&] { return in(command); }, 2u, 0u); }
    bool read(uint8_t address, uint8_t& value) {
        if (!idle()) return false;
        out(command, 0x80);
        if (!input_ready()) return false;
        out(data, address);
        if (!wait([&] { return in(command); }, 3u, 1u)) return false;
        value = in(data);
        return true;
    }
    bool write(uint8_t address, uint8_t value) {
        if (!idle()) return false;
        out(command, 0x81);
        if (!input_ready()) return false;
        out(data, address);
        if (!input_ready()) return false;
        out(data, value);
        return input_ready();
    }
};
// Only bit 6 is the documented Lenovo hardware mute gate. Volume and every
// unrelated bit are preserved. mode: 0=read, 1=unmute, 2=toggle mute.
template<class Read, class Write>
static int thinkpad_ec_audio_update(unsigned mode, Read read, Write write,
                                   uint8_t& before, uint8_t& after) {
    if (mode > 2 || !read(0x30, before)) return -1;
    after = before;
    if (!mode) return 0;
    uint8_t next = mode == 1 ? before & ~0x40u : before ^ 0x40u;
    if (next == before) return 0;
    if (!write(0x30, next) || !read(0x30, after)) return -1;
    return after == next ? 1 : -2;
}
// ACPI Global Lock state transition. Pending=bit 0, Owned=bit 1.
static inline uint32_t thinkpad_global_lock_claim(uint32_t old) {
    return (old & ~3u) | 2u | ((old & 2u) ? 1u : 0u);
}
static inline bool thinkpad_audio_identity(uint32_t controller, uint32_t subsystem, uint32_t codec) {
    return controller == 0x9d718086 && subsystem == 0x505b17aa && codec == 0x14f15111;
}
#endif
