#ifndef WYZAR_THINKPAD_HOTKEY_PROFILE_H
#define WYZAR_THINKPAD_HOTKEY_PROFILE_H
#include <stdint.h>
#include <stddef.h>

// E470 capture: LENOVO TP-R0D, revision 0x960. Fingerprint the actual
// ECOR/HAUM field declaration and complete SAUM method, not a name search.
// This is a firmware-specific equivalent of SAUM(2), not an AML interpreter.
// Other BIOS versions stay unsupported until their field layout is checked.
static uint64_t thinkpad_profile_hash(const uint8_t* bytes, size_t count) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
    return hash;
}
static bool thinkpad_hotkey_profile(const uint8_t* table, size_t size) {
    if (!table || size != 78213) return false;
    const uint8_t header[] = {'D','S','D','T',0x85,0x31,1,0,2};
    const uint8_t oem[] = {'L','E','N','O','V','O','T','P','-','R','0','D',' ',' ',0x60,9,0,0};
    for (unsigned i=0; i<sizeof(header); ++i) if(table[i]!=header[i]) return false;
    for (unsigned i=0; i<sizeof(oem); ++i) if(table[10+i]!=oem[i]) return false;
    uint8_t checksum=0;
    for(size_t i=0; i<size; ++i) checksum+=table[i];
    return !checksum &&
        thinkpad_profile_hash(table+21457,98)==0x5ee201e13f4c3aa5ULL &&
        thinkpad_profile_hash(table+47934,45)==0x59db4e04a249445bULL;
}
// The same _SB._INI Linux branch calls SAUM(2), then UCMS(0x1c).
// Validate the mailbox declaration (allow its physical address to relocate),
// SMI field layout and complete SMI/UCMS methods before returning MNVS.
static uint64_t thinkpad_linux_hotkey_mailbox(const uint8_t* table,size_t size) {
    if(!thinkpad_hotkey_profile(table,size)) return 0;
    const uint8_t region[]={0x5b,0x80,'M','N','V','S',0,0x0c};
    for(unsigned i=0;i<sizeof(region);++i) if(table[2266+i]!=region[i]) return 0;
    if(table[2278]!=0x0b || table[2279]!=0 || table[2280]!=0x10) return 0;
    if(thinkpad_profile_hash(table+76170,167)!=0xb7c36a35713dfdeaULL ||
       thinkpad_profile_hash(table+76841,21)!=0xfc2ee96fac66466eULL ||
       thinkpad_profile_hash(table+12178,50)!=0xcb2695d05f28c391ULL) return 0;
    uint64_t base=0;
    for(unsigned i=0;i<4;++i) base|=uint64_t(table[2274+i])<<(8*i);
    return base;
}
// ECOR starts at zero; HAUM occupies byte 3 bits 5..6 (Preserve).
// Firmware SAUM(2) sets 0b10: mute/up/down are software keys.
template<class Read, class Write>
static int thinkpad_ec_hotkey_update(Read read, Write write, uint8_t& before, uint8_t& after) {
    if(!read(3,before)) return -1;
    after=before;
    uint8_t next=(before & 0x9fu) | 0x40u;
    if(next==before) return 0;
    if(!write(3,next) || !read(3,after)) return -1;
    return after==next ? 1 : -2;
}
#endif
