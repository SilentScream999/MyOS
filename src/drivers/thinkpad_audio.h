#ifndef WYZAR_THINKPAD_AUDIO_H
#define WYZAR_THINKPAD_AUDIO_H
#include "thinkpad_ec_protocol.h"
#include "thinkpad_hotkey_profile.h"
#include "thinkpad_smi.h"
#include "acpi_mode.h"
#include "acpi.h"
#include "timer.h"

// Narrow E470 backend: standard ECDT resources and FACS Global Lock, with the
// hardware mute byte documented by Linux thinkpad-acpi, plus fingerprinted
// E470 SAUM(2)/UCMS(0x1c) initialization. No general AML interpretation,
// EC query/event consumption, NVRAM writes, or changes to SCI/GPE routing.
namespace thinkpad_audio {
struct Ecdt {
    ACPISDTHeader header;
    GAS control, data;
    uint32_t uid;
    uint8_t gpe;
    char path[1];
} __attribute__((packed));
static_assert(offsetof(Ecdt, path) == 65);
static bool initialized = false, supported = false, busy = false, failed = false;
static uint16_t data_port = 0, control_port = 0, pm1a = 0, pm1b = 0;
static volatile uint32_t* global_lock = nullptr;
static bool hotkey_attempted = false;
static bool hotkey_mode_ready = false, linux_hotkey_attempted = false;
static bool acpi_mode_attempted = false;
static void log_hex(const char* label, uint64_t value) {
    char buffer[32]; to_hex(value, buffer); print((char*)label); print(buffer);
}
static bool valid_table(const ACPISDTHeader* h, unsigned minimum) {
    if (!h || h->length < minimum || h->length > 4096) return false;
    uint8_t checksum = 0;
    for (unsigned i = 0; i < h->length; ++i) checksum += ((const uint8_t*)h)[i];
    return !checksum;
}
static uint64_t integer_at(const void* ptr, unsigned bytes) {
    uint64_t result = 0;
    for (unsigned i = 0; i < bytes; ++i) result |= (uint64_t)((const uint8_t*)ptr)[i] << (8 * i);
    return result;
}
static bool io_byte(const GAS& gas) {
    return gas.address_space == 1 && gas.bit_width == 8 && !gas.bit_offset &&
        gas.access_size <= 1 && gas.address && gas.address <= 65535 &&
        gas.address != 0x60 && gas.address != 0x64;
}
static bool map_firmware(uint64_t physical, uint64_t length, bool nvs_only=false) {
    if(!physical || !length || physical+length<physical || !memmap_req.response) return false;
    bool backing=false;
    for(uint64_t i=0;i<memmap_req.response->entry_count;++i){
        auto* e=memmap_req.response->entries[i];
        if((e->type==LIMINE_MEMMAP_ACPI_NVS || (!nvs_only && e->type==LIMINE_MEMMAP_ACPI_RECLAIMABLE)) &&
           physical>=e->base && physical-e->base<=e->length && length<=e->length-(physical-e->base)) backing=true;
    }
    if(!backing) return false;
    TtyIrqGuard guard;
    for(uint64_t p=physical&~4095ULL;p<physical+length;p+=4096) map_page(HHDM+p,p,WRITABLE|NX);
    return true;
}
static const ACPISDTHeader* hotkey_dsdt() {
    auto* fadt=find_fadt();
    if(!valid_table(fadt ? &fadt->h : nullptr,116)) return nullptr;
    uint64_t physical=fadt->Dsdt;
    if(fadt->h.length>=148){uint64_t extended=integer_at((uint8_t*)fadt+140,8);if(extended)physical=extended;}
    if(!map_firmware(physical,36)) return nullptr;
    auto* dsdt=(const ACPISDTHeader*)(HHDM+physical);
    if(dsdt->length!=78213 || !map_firmware(physical,dsdt->length)) return nullptr;
    return dsdt;
}
static bool hotkey_firmware() {
    auto* dsdt=hotkey_dsdt();
    return dsdt && thinkpad_hotkey_profile((const uint8_t*)dsdt,dsdt->length);
}
static void configure_acpi_mode() {
    if(acpi_mode_attempted || !supported || failed) return;
    acpi_mode_attempted=true;
    // This staged backend polls and does not yet implement a general SCI/GPE
    // dispatcher. Only the captured E470's unused, already masked PIC IRQ9 is
    // accepted. Never steal a registered/shared SCI or unmask an interrupt
    // with no handler. Pending ACPI events are left intact for diagnostics.
    auto* fadt=find_fadt();
    if(!hotkey_firmware() || !fadt || fadt->SCI_Interrupt!=9 ||
       g_irq_handlers[9] || !(inb(PIC2_DATA)&2)) {
        print((char*)"ThinkPad hotkeys: ACPI entry skipped; firmware/SCI ownership unsupported");return;
    }
    log_hex("ThinkPad hotkeys: PM1 control before ACPI entry",inw_acpi(pm1a));
    log_hex("ThinkPad hotkeys: FADT ACPI enable command",fadt->AcpiEnable);
    uint64_t start=g_tick_count;
    int rc=acpi_enter_mode({fadt->SMI_CommandPort,pm1a,pm1b,fadt->AcpiEnable},
        [](uint16_t port){return inw_acpi(port);},
        [](uint16_t port,uint8_t value){outb(port,value);},
        [&](){if(g_tick_count-start>=300)return false;sleep_ms(1);return g_tick_count-start<300;});
    // Firmware mode transition must not leave the unhandled SCI unmasked.
    {TtyIrqGuard guard;outb(PIC2_DATA,inb(PIC2_DATA)|2u);}
    log_hex("ThinkPad hotkeys: PM1 control after ACPI entry",inw_acpi(pm1a));
    log_hex("ThinkPad hotkeys: SCI PIC mask (kept masked)",inb(PIC2_DATA)&2);
    print((char*)(rc>=0 ? "ThinkPad hotkeys: ACPI mode active; SCI delivery remains masked" :
        rc==-3 ? "ThinkPad hotkeys: invalid ACPI mode resources; entry skipped" :
        "ThinkPad hotkeys: ACPI mode entry timed out"));
}
static void configure_linux_hotkeys() {
    {TtyIrqGuard guard;
     if(busy || linux_hotkey_attempted || !supported || failed || !hotkey_mode_ready) return;
     busy=true;linux_hotkey_attempted=true;}
    auto* dsdt=hotkey_dsdt();
    uint64_t base=dsdt ? thinkpad_linux_hotkey_mailbox((const uint8_t*)dsdt,dsdt->length) : 0;
    if(!base || !map_firmware(base,4096,true)) {
        print((char*)"ThinkPad hotkeys: validated Linux-init mailbox unavailable; UCMS skipped");
    }else{
        // The captured AnyAcc fields include unaligned dwords. Access bytes
        // explicitly, avoiding C++ alignment assumptions. SMI is synchronous.
        auto* mailbox=(volatile uint8_t*)(HHDM+base+0xfc0);
        auto read=[&](unsigned offset,unsigned width){
            uint32_t value=0;for(unsigned i=0;i<width;++i)value|=uint32_t(mailbox[offset+i])<<(8*i);return value;
        };
        auto write=[&](unsigned offset,unsigned width,uint32_t value){
            for(unsigned i=0;i<width;++i)mailbox[offset+i]=uint8_t(value>>(8*i));
        };
        auto trigger=[](){asm volatile("mfence" ::: "memory");outb(0xb2,0xf5);asm volatile("mfence" ::: "memory");};
        uint64_t start=g_tick_count;
        auto pause=[&](){if(g_tick_count-start>=50)return false;sleep_ms(1);return g_tick_count-start<50;};
        uint32_t status=0,result=0;
        int rc=thinkpad_linux_hotkey_init(read,write,trigger,pause,status,result);
        log_hex("ThinkPad hotkeys: Linux-init mailbox physical address",base+0xfc0);
        log_hex("ThinkPad hotkeys: UCMS(0x1c) firmware status",status);
        log_hex("ThinkPad hotkeys: UCMS(0x1c) result",result);
        print((char*)(rc==0 ? "ThinkPad hotkeys: firmware Linux-init sequence completed; key delivery needs verification" :
            rc==-2 ? "ThinkPad hotkeys: firmware mailbox busy; UCMS skipped" :
            rc==-1 ? "ThinkPad hotkeys: firmware Linux-init timeout; no further calls this boot" :
            "ThinkPad hotkeys: firmware Linux-init returned an error"));
    }
    {TtyIrqGuard guard;busy=false;}
}
static void initialize(bool identity) {
    if (initialized) return;
    initialized = true;
    if (!identity) return;
    auto* fadt = find_fadt();
    auto* ec = (Ecdt*)find_acpi_table("ECDT");
    if (!valid_table(fadt ? &fadt->h : nullptr, 116) || strncmp(fadt->h.oem_id, "LENOVO", 6)) {
        print((char*)"ThinkPad audio: Lenovo FADT unavailable/invalid"); return;
    }
    if (!valid_table(ec ? &ec->header : nullptr, 66) || strncmp(ec->header.oem_id, "LENOVO", 6) ||
        !io_byte(ec->control) || !io_byte(ec->data) || ec->control.address == ec->data.address) {
        print((char*)"ThinkPad audio: valid ECDT I/O resources unavailable; EC access disabled"); return;
    }
    bool terminated = false;
    for (unsigned i = 65; i < ec->header.length; ++i) if (!((const char*)ec)[i]) { terminated = true; break; }
    if (!terminated) return;
    uint64_t facs = fadt->FirmwareCtrl;
    if (fadt->h.length >= 140) {
        uint64_t extended = integer_at((uint8_t*)fadt + 132, 8);
        if (extended) facs = extended;
    }
    // Validate the lock's backing as ACPI memory before mapping or accessing it.
    bool backing = false;
    if (facs && !(facs & 63) && memmap_req.response) {
        for (uint64_t i = 0; i < memmap_req.response->entry_count; ++i) {
            auto* e = memmap_req.response->entries[i];
            if ((e->type == LIMINE_MEMMAP_ACPI_NVS || e->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE) &&
                facs >= e->base && facs - e->base <= e->length && 64 <= e->length - (facs - e->base)) backing = true;
        }
    }
    if (!backing) { print((char*)"ThinkPad audio: FACS Global Lock backing unavailable; EC access disabled"); return; }
    { TtyIrqGuard guard; map_page(HHDM + (facs & ~4095ULL), facs & ~4095ULL, WRITABLE | NX); }
    auto* f = (uint8_t*)(HHDM + facs);
    if (strncmp((char*)f, "FACS", 4) || integer_at(f + 4, 4) < 64 ||
        fadt->PM1ControlLength < 2 || !fadt->PM1aControlBlock ||
        fadt->PM1aControlBlock > 65534 || fadt->PM1bControlBlock > 65534) {
        print((char*)"ThinkPad audio: FACS/Global Lock release registers invalid"); return;
    }
    global_lock = (volatile uint32_t*)(f + 16);
    data_port = (uint16_t)ec->data.address; control_port = (uint16_t)ec->control.address;
    pm1a = (uint16_t)fadt->PM1aControlBlock; pm1b = (uint16_t)fadt->PM1bControlBlock;
    supported = true;
    log_hex("ThinkPad audio: ECDT data/control ports", ((uint32_t)data_port << 16) | control_port);
    log_hex("ThinkPad audio: ECDT GPE (not enabled by this backend)", ec->gpe);
    log_hex("ThinkPad audio: FADT SCI interrupt", fadt->SCI_Interrupt);
    log_hex("ThinkPad audio: firmware SCI_EN", inw_acpi(pm1a) & 1u);
    print((char*)"ThinkPad audio: ECDT namespace path"); print(ec->path);
    print((char*)"ThinkPad audio: E470 hardware mute backend ready (EC 0x30 bit 6)");
}
template<class Read> static bool wait(Read read, unsigned mask, unsigned value) {
    uint64_t start = g_tick_count;
    for (unsigned i = 0; i < 50 && g_tick_count - start < 50; ++i) {
        if ((read() & mask) == value) return true;
        sleep_ms(1);
    }
    return false;
}
static bool lock() {
    for (unsigned i = 0; i < 50; ++i) {
        uint32_t old = __atomic_load_n(global_lock, __ATOMIC_ACQUIRE);
        uint32_t next = thinkpad_global_lock_claim(old);
        if (__atomic_compare_exchange_n(global_lock, &old, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && !(next & 1)) return true;
        sleep_ms(1);
    }
    __atomic_fetch_and(global_lock, ~1u, __ATOMIC_RELEASE); // Withdraw our pending request.
    return false;
}
static void unlock() {
    uint32_t old = __atomic_fetch_and(global_lock, ~3u, __ATOMIC_RELEASE);
    if (old & 1) {
        outw_acpi(pm1a, inw_acpi(pm1a) | 4u); // GBL_RLS notifies waiting firmware.
        if (pm1b) outw_acpi(pm1b, inw_acpi(pm1b) | 4u);
    }
}
static int64_t control(bool identity, unsigned mode) {
    // Internal mode 3 configures software keys without changing hardware mute.
    if (mode > 3) return -4;
    { TtyIrqGuard guard; if (busy) return -2; busy = true; }
    initialize(identity);
    int64_t result = -6;
    if (supported && !failed) {
        // ACPI ownership precedes the firmware's _INI-equivalent mode setup.
        if(mode) configure_acpi_mode();
        if (!lock()) { result = -1; print((char*)"ThinkPad audio: firmware Global Lock timeout"); }
        else {
            auto input = [](uint16_t port) { return inb(port); };
            auto output = [](uint16_t port, uint8_t value) { outb(port, value); };
            auto waiting = [](auto read, unsigned mask, unsigned value) { return wait(read, mask, value); };
            ThinkpadEcProtocol<decltype(input), decltype(output), decltype(waiting)> protocol{data_port, control_port, input, output, waiting};
            bool hotkey_ok=true;
            if(mode && !hotkey_attempted){
                hotkey_attempted=true;
                if(hotkey_firmware()){
                    uint8_t old_mode=0,new_mode=0;
                    hotkey_ok=thinkpad_ec_hotkey_update(
                        [&](uint8_t address,uint8_t& value){return protocol.read(address,value);},
                        [&](uint8_t address,uint8_t value){return protocol.write(address,value) && (sleep_ms(1),true);},old_mode,new_mode)>=0;
                    hotkey_mode_ready=hotkey_ok;
                    log_hex("ThinkPad hotkeys: EC 0x03 before SAUM(2)",old_mode);
                    log_hex("ThinkPad hotkeys: EC 0x03 readback",new_mode);
                    print((char*)(hotkey_ok ? "ThinkPad hotkeys: software-key mode verified (SAUM 2)" : "ThinkPad hotkeys: mode transaction/readback failed"));
                }else print((char*)"ThinkPad hotkeys: unrecognized firmware; software key setup skipped");
            }
            uint8_t before = 0, after = 0;
            int changed = !hotkey_ok ? -1 : mode==3 ? 0 : thinkpad_ec_audio_update(mode,
                [&](uint8_t address, uint8_t& value) { return protocol.read(address, value); },
                [&](uint8_t address, uint8_t value) { return protocol.write(address, value) && (sleep_ms(1), true); },
                before, after);
            unlock();
            if (changed < 0) {
                // A partial EC command must never be followed by another one.
                failed = true; result = -1;
                print((char*)"ThinkPad audio: EC transaction/readback failed; backend disabled until reboot");
            } else if(mode==3) result=0;
            else {
                log_hex("ThinkPad audio: EC audio byte before", before);
                log_hex("ThinkPad audio: EC audio byte after", after);
                print((char*)((after & 0x40) ? "ThinkPad audio: EC mute bit is SET" : "ThinkPad audio: EC mute bit is CLEAR"));
                result = mode ? 0 : after;
            }
        }
    }
    { TtyIrqGuard guard; busy = false; }
    return result;
}
} // namespace thinkpad_audio
#endif
