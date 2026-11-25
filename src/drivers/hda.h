#ifndef WYZAR_HDA_H
#define WYZAR_HDA_H
#include "hda_topology.h"
#include "hda_command.h"
#include "hda_link.h"
#include "helpers.h"
#include "pagingstuff.h"
#include "timer.h"
#include "tty.h"
#include "thinkpad_audio.h"
#include "audio_hotkeys.h"
#include "fn_probe.h"
#include "audio_volume.h"
#include "hda_pcm_ring.h"

// Shared analog HDA diagnostics and one exclusive 48 kHz stereo PCM stream.
// Commands are serialized and polled, leaving shared PCI IRQ lines untouched.
// Spec: Intel High Definition Audio rev 1.0a, chapters 3, 4 and 7.
namespace hda {
static constexpr uint64_t mmio_base = 0xffff890000000000ULL;
static constexpr unsigned format = 0x11; // 48 kHz, signed 16-bit, stereo
struct Device {
    uint64_t ecam = 0;
    uint32_t id = 0;
    uint16_t segment = 0;
    uint8_t start = 0, bus = 0, dev = 0, fn = 0;
};
static Device devices[4]{};
static uint64_t bars[4]{};
static unsigned device_count = 0;
static Device active{};
static uint64_t mmio = 0, dma_phys = 0, samples_phys = 0;
static unsigned corb_size = 0, rirb_size = 0, corb_wp = 0, rirb_rp = 0;
static unsigned stream = 0, codec = 0, afg = 0;
static HdaWidget widgets[256]{};
static bool busy = false, attempted = false, ready = false, transport_failed = false;
static bool desktop_initialized = false;
// Publish only after PCI discovery and the first desktop frame. The audio
// task may have been scheduled while boot was still filling devices/bars.
static void desktop_ready() {
    __atomic_store_n(&desktop_initialized,true,__ATOMIC_RELEASE);
}
static unsigned available = 0;
static uint32_t codec_id = 0;
static bool immediate = false;
static AudioVolume& volume=g_audio_mixer.value;
static HdaRoute live_route{};

static uint8_t r8(unsigned off) { return *(volatile uint8_t*)(mmio + off); }
static uint16_t r16(unsigned off) { return *(volatile uint16_t*)(mmio + off); }
static uint32_t r32(unsigned off) { return *(volatile uint32_t*)(mmio + off); }
static void w8(unsigned off, uint8_t value) { *(volatile uint8_t*)(mmio + off) = value; }
static void w16(unsigned off, uint16_t value) { *(volatile uint16_t*)(mmio + off) = value; }
static void w32(unsigned off, uint32_t value) { *(volatile uint32_t*)(mmio + off) = value; }
static void fence() { __asm__ volatile("mfence" ::: "memory"); }
static void dma_flush(uint64_t address, unsigned bytes) {
    for (uint64_t p = address & ~63ULL; p < address + bytes; p += 64)
        __asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
    fence();
}
static void log_hex(const char* label, uint64_t value) {
    char text[32]; to_hex(value, text); print((char*)label); print(text);
}
// Tick deadlines plus an iteration bound protect against a stopped device/timer.
template<class Read> static bool wait(Read read, uint32_t mask, uint32_t value) {
    uint64_t started = g_tick_count;
    for (unsigned i = 0; i < 100; ++i) {
        if ((read() & mask) == value) return true;
        if (g_tick_count - started >= 100) break;
        sleep_ms(1);
    }
    return false;
}
static void discover(uint64_t ecam, uint16_t segment, uint8_t start,
                     uint8_t bus, uint8_t dev, uint8_t fn, uint32_t id, uint32_t cls) {
    if ((cls >> 16) != 0x0403 || device_count == 4) return;
    unsigned slot = device_count++;
    devices[slot] = {ecam, id, segment, start, bus, dev, fn};
    log_hex("Audio: HDA PCI vendor/device", id);
    log_hex("Audio: HDA segment:bus:device:function",
            ((uint64_t)segment << 24) | ((uint32_t)bus << 16) | (dev << 8) | fn);
    uint32_t bar = pci_cfg_read32(ecam, start, bus, dev, fn, 0x10);
    if (!bar || bar == 0xffffffff || (bar & 1) || ((bar >> 1) & 3) == 1 || ((bar >> 1) & 3) == 3) return;
    uint64_t physical = bar & ~15u;
    if ((bar & 6) == 4) physical |= (uint64_t)pci_cfg_read32(ecam, start, bus, dev, fn, 0x14) << 32;
    if (!physical || (physical & 0x3fff)) return;
    // Establish the upper-half page-table entry BEFORE user PML4s copy it.
    // Lazy hardware initialization may later run in any terminal's address space.
    { TtyIrqGuard guard; map_mmio_region(physical, mmio_base + slot * 0x10000, 0x4000); }
    bars[slot] = physical;
    log_hex("Audio: HDA PCI subsystem", pci_cfg_read32(ecam, start, bus, dev, fn, 0x2c));
}
static void bus_master(bool enable) {
    uint16_t command = pci_cfg_read16(active.ecam, active.start, active.bus, active.dev, active.fn, 4);
    command = enable ? command | 6u : command & ~4u;
    // Polling: suppress INTx without disturbing other devices sharing the line.
    pci_cfg_write16(active.ecam, active.start, active.bus, active.dev, active.fn, 4, command | (1u << 10));
}
static bool dma_address_ok(uint64_t phys, unsigned bytes) {
    return (r16(0) & 1) || (phys < 0x100000000ULL && bytes <= 0x100000000ULL - phys);
}
static unsigned ring_size(unsigned reg) {
    unsigned caps = r8(reg);
    unsigned code = (caps & 0x40) ? 2 : ((caps & 0x20) ? 1 : 0);
    if (!(caps & (0x10u << code))) return 0;
    w8(reg, (uint8_t)code);
    return code == 2 ? 256 : code == 1 ? 16 : 2;
}
static bool reset_controller() {
    ready = false;
    bool intel_link = hda_skl_kbl_controller(active.id);
    uint16_t wake = r16(0xc);
    uint32_t clock = 0;
    if (intel_link) {
        clock = pci_cfg_read32(active.ecam, active.start, active.bus, active.dev, active.fn, 0x48);
        pci_cfg_write32(active.ecam, active.start, active.bus, active.dev, active.fn, 0x48, clock & ~(1u << 6));
        log_hex("Audio: Intel reset CGCTL", clock);
    }
    // Keep codec wake armed during reset, as required by Intel's SKL/KBL path.
    // INTCTL stays zero throughout; this does not install/deliver an IRQ.
    w32(0x20, 0); w16(0xc, intel_link ? 0x7fff : 0);
    bool ok = [&] {
        w8(0x4c, 0); w8(0x5c, 0);
        w32(8, 0);
        if (!wait([] { return r32(8); }, 1, 0)) return false;
        sleep_ms(1); w32(8, 1);
        if (!wait([] { return r32(8); }, 1, 1)) return false;
        sleep_ms(intel_link ? 20 : 2);
        if ((r8(0x4c) | r8(0x5c)) & 2) return false;
        if (intel_link) {
            unsigned ml = hda_find_multilink(r32);
            log_hex("Audio: Intel multi-link capability offset", ml);
            if (ml) {
                log_hex("Audio: external link capabilities", r32(ml + 0x40));
                log_hex("Audio: external link control before", r32(ml + 0x44));
                log_hex("Audio: external link codec identifiers", r32(ml + 0x4c));
                bool linked = hda_start_external_link(ml, r32, w32,
                    [](auto read, uint32_t mask, uint32_t value) { return wait(read, mask, value); },
                    [](unsigned ms) { sleep_ms(ms); });
                log_hex("Audio: external link control after", r32(ml + 0x44));
                if (!linked) { print((char*)"Audio: external link power/clock setup failed"); return false; }
            }
        }
        return true;
    }();
    w16(0xc, wake);
    if (intel_link)
        pci_cfg_write32(active.ecam, active.start, active.bus, active.dev, active.fn, 0x48, clock);
    if (ok) transport_failed = false;
    return ok;
}
static bool init_rings() {
    if (!dma_phys) dma_phys = alloc_phys_page();
    if (!dma_address_ok(dma_phys, 4096)) return false;
    // One pinned page: CORB [0,1024), RIRB [1024,3072), BDL [3072,3104).
    w8(0x4c, 0); w8(0x5c, 0);
    if (!wait([] { return r8(0x4c) | r8(0x5c); }, 2, 0)) return false;
    // Flush the initialized page before DMA. Some firmware leaves snooping off;
    // do not let dirty zeroed cache lines overwrite incoming codec responses.
    memset((void*)(HHDM + dma_phys), 0, 4096);
    dma_flush(HHDM + dma_phys, 4096);
    corb_size = ring_size(0x4e); rirb_size = ring_size(0x5e);
    if (!corb_size || !rirb_size) return false;
    w32(0x40, (uint32_t)dma_phys); w32(0x44, dma_phys >> 32);
    w32(0x50, (uint32_t)(dma_phys + 1024)); w32(0x54, (dma_phys + 1024) >> 32);
    w16(0x48, 0); w16(0x4a, 0x8000);
    if (!wait([] { return r16(0x4a); }, 0x8000, 0x8000)) return false;
    w16(0x4a, 0);
    if (!wait([] { return r16(0x4a); }, 0x8000, 0)) return false;
    w16(0x58, 0x8000); // RIRB reset self-clears, unlike CORBRP reset.
    corb_wp = rirb_rp = 0;
    w16(0x5a, 1); w8(0x5d, 5); w8(0x4d, 1);
    // Enable response status generation so W1C acknowledges the response count.
    // Global INTCTL remains zero: no interrupt is delivered to the CPU.
    fence(); w8(0x5c, 3); w8(0x4c, 2);
    return wait([] { return r8(0x4c) & r8(0x5c); }, 2, 2);
}
static bool verb(unsigned nid, unsigned command, unsigned payload, uint32_t& result) {
    if (transport_failed) return false;
    uint32_t encoded = (codec << 28) | (nid << 20) | (command << 8) | payload;
    if (immediate) {
        bool ok = hda_immediate_command(encoded, result, r16, w16, r32, w32,
            [](auto read, uint32_t mask, uint32_t value) { return wait(read, mask, value); });
        if (ok) return true;
        log_hex("Audio: immediate command timeout/error", encoded);
        log_hex("Audio: immediate command status", r16(0x68));
        transport_failed = true; ready = false;
        return false;
    }
    auto* corb = (volatile uint32_t*)(HHDM + dma_phys);
    auto* rirb = (volatile uint64_t*)(HHDM + dma_phys + 1024);
    corb_wp = (corb_wp + 1) % corb_size;
    corb[corb_wp] = encoded;
    dma_flush((uint64_t)&corb[corb_wp], sizeof(uint32_t));
    w16(0x48, (uint16_t)corb_wp);
    uint64_t started = g_tick_count;
    for (unsigned poll = 0; poll < 100; ++poll) {
        if ((r8(0x5d) & 4) || (r8(0x4d) & 1)) break;
        unsigned wp = (r16(0x58) & 255) % rirb_size;
        while (rirb_rp != wp) {
            rirb_rp = (rirb_rp + 1) % rirb_size;
            dma_flush((uint64_t)&rirb[rirb_rp], sizeof(uint64_t));
            uint64_t response = rirb[rirb_rp];
            if ((response >> 36) & 1) continue; // unsolicited jack event
            if (((response >> 32) & 15) != codec) continue;
            result = (uint32_t)response;
            w8(0x5d, 1); // Ack response status even with interrupts disabled.
            return true;
        }
        if (g_tick_count - started >= 100) break;
        // Most verbs complete within two 48-kHz link frames. Brief bounded
        // polling avoids a scheduler tick for every widget parameter.
        for (unsigned spin = 0; spin < 200; ++spin) __asm__ volatile("pause");
        if (poll > 10) sleep_ms(1);
    }
    log_hex("Audio: codec command timeout/error", (codec << 28) | (nid << 20) | (command << 8) | payload);
    log_hex("Audio: CORB write/read pointers", ((uint32_t)r16(0x48) << 16) | r16(0x4a));
    log_hex("Audio: RIRB hardware/software pointers", ((uint32_t)r16(0x58) << 16) | rirb_rp);
    log_hex("Audio: CORB/RIRB control/status", ((uint32_t)r8(0x4c) << 24) | ((uint32_t)r8(0x4d) << 16) | (r8(0x5c) << 8) | r8(0x5d));
    log_hex("Audio: command DMA physical address", dma_phys);
    transport_failed = true; ready = false;
    return false; // Never reuse a late response for the next command.
}
static bool set(unsigned nid, unsigned command, unsigned payload) {
    uint32_t ignored; return verb(nid, command, payload, ignored);
}
static bool parameter(unsigned nid, unsigned param, uint32_t& result) { return verb(nid, 0xf00, param, result); }
static bool enumerate_group(unsigned group) {
    uint32_t children, formats, pcm, input_amp, output_amp;
    if (!set(group, 0x705, 0) || !parameter(group, 4, children) ||
        !parameter(group, 0xa, pcm) || !parameter(group, 0xb, formats) ||
        !parameter(group, 0xd, input_amp) || !parameter(group, 0x12, output_amp)) return false;
    sleep_ms(10);
    unsigned first = (children >> 16) & 255, count = children & 255;
    if (!first || first + count > 256) return false;
    memset(widgets, 0, sizeof(widgets));
    for (unsigned nid = first; nid < first + count; ++nid) {
        auto& w = widgets[nid];
        if (!parameter(nid, 9, w.caps)) return false;
        w.present = true;
        uint32_t rates = pcm, types = formats;
        if ((w.caps & (1u << 4)) &&
            (!parameter(nid, 0xa, rates) || !parameter(nid, 0xb, types))) return false;
        w.pcm = (w.caps & 1) && (types & 1) && (rates & (1u << 6)) && (rates & (1u << 17));
        w.pcm_rates = rates;
        w.input_amp = input_amp; w.output_amp = output_amp;
        if (w.caps & (1u << 3)) {
            if ((w.caps & 2) && !parameter(nid, 0xd, w.input_amp)) return false;
            if ((w.caps & 4) && !parameter(nid, 0x12, w.output_amp)) return false;
        }
        if (w.type() == 4) {
            if (!parameter(nid, 0xc, w.pin_caps) || !verb(nid, 0xf1c, 0, w.config)) return false;
            log_hex("Audio: pin NID", nid);
            log_hex("Audio: pin default configuration", w.config);
            log_hex("Audio: pin capabilities", w.pin_caps);
        }
        if (w.caps & (1u << 8)) {
            uint32_t length;
            if (!parameter(nid, 0xe, length)) return false;
            bool long_form = length & 0x80;
            unsigned entries = length & 127, per = long_form ? 2 : 4;
            bool valid = true;
            for (unsigned index = 0; index < entries; index += per) {
                uint32_t packed;
                if (!verb(nid, 0xf02, index, packed)) return false;
                for (unsigned j = 0; j < per && index + j < entries; ++j) {
                    unsigned entry = (packed >> (j * (long_form ? 16 : 8))) & (long_form ? 65535 : 255);
                    if (!w.inputs.append(entry, long_form)) valid = false;
                }
            }
            if (!valid) { w.inputs.count = 0; log_hex("Audio: unsupported connection list at NID", nid); }
        }
    }
    available = 0;
    for (unsigned nid = first; nid < first + count; ++nid) {
        unsigned kind = hda_pin_kind(widgets[nid]); HdaRoute route{};
        if (kind && hda_find_route(widgets, nid, route)) available |= 1u << kind;
    }
    return available != 0;
}
static bool probe_codec() {
    uint32_t groups;
    if (!parameter(0, 0, codec_id) || !parameter(0, 4, groups)) return false;
    if (!codec_id || codec_id == 0xffffffff) return false;
    log_hex("Audio: codec address", codec);
    log_hex("Audio: codec vendor/device", codec_id);
    unsigned first = (groups >> 16) & 255, count = groups & 255;
    if (!first || first + count > 256) return false;
    for (afg = first; afg < first + count; ++afg) {
        uint32_t type;
        if (!parameter(afg, 5, type)) return false;
        if ((type & 255) == 1 && enumerate_group(afg)) {
            ready = true;
            log_hex("Audio: analog output mask (speaker=2 headphone=4 line=8)", available);
            print((char*)"Audio: analog test output ready (not yet hardware verified)");
            return true;
        }
        if (transport_failed) return false;
    }
    return false;
}
static bool init_device(const Device& device, unsigned slot) {
    active = device; transport_failed = false; immediate = false;
    if (!bars[slot]) { print((char*)"Audio: unsupported/unassigned controller BAR"); return false; }
    // Move a PCI power-management capability to D0 without touching vendor DSP
    // registers. Broken/cyclic capability lists are bounded.
    unsigned cap = pci_cfg_read16(active.ecam, active.start, active.bus, active.dev, active.fn, 6) & 0x10
        ? pci_cfg_read32(active.ecam, active.start, active.bus, active.dev, active.fn, 0x34) & 0xfc : 0;
    for (unsigned i = 0; cap >= 0x40 && cap <= 0xfc && i < 48; ++i) {
        uint32_t header = pci_cfg_read32(active.ecam, active.start, active.bus, active.dev, active.fn, cap);
        if ((header & 255) == 1 && cap <= 0xf8) {
            uint16_t pm = pci_cfg_read16(active.ecam, active.start, active.bus, active.dev, active.fn, cap + 4);
            pci_cfg_write16(active.ecam, active.start, active.bus, active.dev, active.fn, cap + 4, pm & ~0x8003u);
            sleep_ms(10); break;
        }
        cap = (header >> 8) & 0xfc;
    }
    mmio = mmio_base + slot * 0x10000;
    bus_master(true);
    if (!reset_controller()) return false;
    uint16_t caps = r16(0), codecs = r16(0xe) & 0x7fff;
    log_hex("Audio: controller GCAP", caps); log_hex("Audio: codec mask", codecs);
    if (!((caps >> 12) & 15) || !codecs) return false;
    if (!init_rings()) {
        if (!reset_controller()) return false;
        immediate = true;
        print((char*)"Audio: command rings unavailable; trying immediate commands");
    }
    stream = 0x80 + ((caps >> 8) & 15) * 0x20; // first output, after input streams
    for (codec = 0; codec < 15; ++codec) {
        if (!(codecs & (1u << codec))) continue;
        if (probe_codec()) return true;
        if (transport_failed && !immediate) {
            // Reset flushes late responses and all codec configuration. Reprobe
            // this codec from the start; never mix ring and immediate commands.
            if (!reset_controller()) return false;
            immediate = true;
            print((char*)"Audio: retrying codec with immediate commands after link reset");
            if (probe_codec()) return true;
        }
        if (transport_failed) {
            log_hex("Audio: skipping unresponsive codec", codec);
            if (!reset_controller()) return false;
        }
    }
    return false;
}
static bool stop_stream() {
    w8(stream, r8(stream) & ~2u);
    return wait([] { return r8(stream); }, 2, 0);
}
static bool log_route_state(const HdaRoute& route, const char* phase) {
    print((char*)phase);
    auto read = [](unsigned nid, unsigned command, unsigned payload, const char* label) {
        uint32_t value;
        if (!verb(nid, command, payload, value)) return false;
        log_hex(label, value); return true;
    };
    for (unsigned d = 0; d < route.count; ++d) {
        unsigned nid = route.nodes[d]; const auto& w = widgets[nid];
        log_hex("Audio: route widget NID", nid);
        log_hex("Audio: route widget capabilities", w.caps);
        if (w.type() == 0) {
            if (!read(nid, 0xf06, 0, "Audio: converter stream/channel") ||
                !read(nid, 0xa00, 0, "Audio: converter format")) return false;
        }
        if (w.inputs.count) {
            log_hex("Audio: route connection count", w.inputs.count);
            for (unsigned i = 0; i < w.inputs.count; ++i)
                log_hex("Audio: route connection index/NID", (i << 8) | w.inputs.nodes[i]);
        }
        if (w.caps & (1u << 10))
            if (!read(nid, 0xf05, 0, "Audio: route power requested/actual")) return false;
        if (w.caps & 4) {
            log_hex("Audio: route output amp capabilities", w.output_amp);
            if (!read(nid, 0xb00, 0xa000, "Audio: route output amp left") ||
                !read(nid, 0xb00, 0x8000, "Audio: route output amp right")) return false;
        }
        if (w.caps & 2) {
            unsigned index = w.type() == 2 ? route.indices[d] : 0;
            log_hex("Audio: route input amp capabilities", w.input_amp);
            if (!read(nid, 0xb00, 0x2000 | index, "Audio: route input amp left") ||
                !read(nid, 0xb00, index, "Audio: route input amp right")) return false;
        }
        if ((w.type() == 3 || w.type() == 4) && w.inputs.count)
            if (!read(nid, 0xf01, 0, "Audio: route selected connection")) return false;
        if (w.type() == 4) {
            if (!read(nid, 0xf07, 0, "Audio: route pin control")) return false;
            if ((w.pin_caps & 4) && !read(nid, 0xf09, 0, "Audio: route pin sense")) return false;
            if ((w.pin_caps & (1u << 16)) && !read(nid, 0xf0c, 0, "Audio: route EAPD")) return false;
        }
    }
    uint32_t gpio;
    if (!parameter(afg, 0x11, gpio)) return false;
    log_hex("Audio: function group GPIO capabilities", gpio);
    if (gpio & 255) {
        if (!read(afg, 0xf15, 0, "Audio: GPIO data (read only)") ||
            !read(afg, 0xf16, 0, "Audio: GPIO enable mask (read only)") ||
            !read(afg, 0xf17, 0, "Audio: GPIO direction (read only)")) return false;
    }
    return true;
}
static bool configure_route(const HdaRoute& route, unsigned stream_format = format) {
    if (!set(afg, 0x705, 0)) return false;
    for (unsigned d = 0; d < route.count; ++d) {
        unsigned nid = route.nodes[d]; const auto& w = widgets[nid];
        if ((w.caps & (1u << 10)) && !set(nid, 0x705, 0)) return false;
    }
    sleep_ms(10);
    for (unsigned d = 0; d < route.count; ++d) {
        unsigned nid = route.nodes[d], selected = route.indices[d]; const auto& w = widgets[nid];
        if (d + 1 < route.count && w.type() != 2 && w.inputs.count && !set(nid, 0x701, selected)) return false;
        if ((w.caps & 4) && !set(nid, 0x300, 0xb000 |
            (w.type() == 0 ? volume.amp(w.output_amp) : hda_amp_gain(w.output_amp)))) return false;
        if (w.caps & 2) {
            if (w.type() == 2 && w.inputs.count > 16) return false;
            unsigned count = w.type() == 2 ? w.inputs.count : 1;
            for (unsigned i = 0; i < count; ++i) {
                bool mute = w.type() == 2 && i != selected;
                if (mute && !(w.input_amp & 0x80000000u)) return false;
                unsigned amp = 0x7000 | (i << 8) | hda_amp_gain(w.input_amp) | (mute ? 0x80 : 0);
                if (!set(nid, 0x300, amp)) return false;
            }
        }
    }
    unsigned dac = route.nodes[route.count - 1], pin = route.nodes[0];
    // Mute every other output pin: headphone tests must not also play speakers.
    for (unsigned nid = 1; nid < 256; ++nid)
        if (hda_pin_kind(widgets[nid]) && !set(nid, 0x707, 0)) return false;
    if (!set(dac, 0x200, stream_format) || !set(dac, 0x706, 0x10)) return false;
    unsigned control = 0x40 | (hda_pin_kind(widgets[pin]) == 2 && (widgets[pin].pin_caps & 8) ? 0x80 : 0);
    if (!set(pin, 0x707, volume.muted || !volume.percent ? 0 : control)) return false;
    if (widgets[pin].pin_caps & (1u << 16)) {
        uint32_t eapd;
        if (!verb(pin, 0xf0c, 0, eapd) || !set(pin, 0x70c, eapd | 2)) return false;
    }
    log_hex("Audio: selected pin", pin); log_hex("Audio: selected DAC", dac);
    return true;
}
static bool tone(unsigned kind, bool alternate = false) {
    log_hex("Audio: requested output kind", kind);
    if (!dma_phys || !dma_address_ok(dma_phys, 4096)) {
        print((char*)"Audio: playback descriptor address unsupported by controller"); return false;
    }
    HdaRoute route{};
    hda_select_output_route(widgets, kind, route, alternate);
    if (!route.count) { print((char*)"Audio: route search failed"); return false; }
    if (!log_route_state(route, "Audio: route state before configuration")) return false;
    if (!stop_stream()) { ready = false; bus_master(false); return false; }
    w8(stream, 1);
    // QEMU 6.2's virtual codec/controller clears SRST immediately (fixed in
    // newer QEMU). Keep the required assert acknowledgement on real hardware.
    bool qemu_self_clear = active.id == 0x26688086 && (codec_id >> 16) == 0x1af4 && !(r8(stream) & 1);
    if (!qemu_self_clear && !wait([] { return r8(stream); }, 1, 1)) {
        log_hex("Audio: stream reset assert failed", r32(stream)); return false;
    }
    if (qemu_self_clear) print((char*)"Audio: virtual codec stream reset self-cleared");
    w8(stream, 0);
    if (!wait([] { return r8(stream); }, 1, 0)) { log_hex("Audio: stream reset clear failed", r32(stream)); return false; }
    if (!configure_route(route)) return false;
    // Allow the output amplifier to settle before the short tone begins.
    sleep_ms(50);
    if (!log_route_state(route, "Audio: route state after configuration")) return false;
    constexpr unsigned frames = 4800, bytes = frames * 4;
    if (!samples_phys) samples_phys = alloc_phys_pages(5);
    if (!dma_address_ok(samples_phys, 5 * 4096)) return false;
    auto* samples = (int16_t*)(HHDM + samples_phys);
    for (unsigned i = 0; i < frames; ++i) {
        int phase = i % 100;
        int16_t value = (int16_t)((phase < 50 ? phase : 100 - phase) * 60 - 1500);
        samples[i * 2] = samples[i * 2 + 1] = value;
    }
    struct Descriptor { uint64_t address; uint32_t length, flags; };
    static_assert(sizeof(Descriptor) == 16);
    auto* bdl = (Descriptor*)(HHDM + dma_phys + 3072);
    bdl[0] = {samples_phys, bytes / 2, 0};
    bdl[1] = {samples_phys + bytes / 2, bytes / 2, 0};
    dma_flush((uint64_t)samples, bytes);
    dma_flush((uint64_t)bdl, 2 * sizeof(Descriptor));
    w32(stream + 0x18, (uint32_t)(dma_phys + 3072)); w32(stream + 0x1c, (dma_phys + 3072) >> 32);
    w32(stream + 8, bytes); w16(stream + 0xc, 1); w16(stream + 0x12, format);
    w8(stream + 2, 0x10); w8(stream + 3, 0x1c);
    fence(); w8(stream, 2);
    bool running = wait([] { return r8(stream); }, 2, 2), progressed = false, error = false;
    uint32_t previous = r32(stream + 4);
    for (unsigned i = 0; running && i < 80; ++i) {
        sleep_ms(10);
        uint32_t position = r32(stream + 4);
        progressed |= position != previous; previous = position;
        if (r8(stream + 3) & 0x18) { error = true; break; }
        if (g_current_task && g_current_task->interrupt_requested) break;
    }
    bool stopped = stop_stream();
    log_hex("Audio: final DMA position", previous); log_hex("Audio: stream status", r8(stream + 3));
    if (!stopped) { ready = false; bus_master(false); }
    // Disconnect the converter after stopping, even on a cancelled/failed test.
    set(route.nodes[route.count - 1], 0x706, 0);
    set(route.nodes[0], 0x707, 0);
    return running && progressed && !error && stopped && !transport_failed;
}
// SYS_AUDIO: 0=query, 1=test, 2=alternate DAC, 3=firmware status, 4=unmute,
// 5=toggle firmware mute (deferred physical mute key).
// -1=failed, -2=busy, -3=unsupported output, -4=invalid, -5=no alternate DAC.
static int64_t control_legacy(uint64_t action, uint64_t kind) {
    if (action > 5 || kind > 3 || ((!action || action >= 3) && kind)) return -4;
    { TtyIrqGuard guard; if (busy) return -2; busy = true; }
    if (!attempted) {
        attempted = true;
        for (unsigned i = 0; i < device_count; ++i) {
            if (init_device(devices[i], i)) break;
            if (mmio) { w8(0x4c, 0); w8(0x5c, 0); bus_master(false); }
            mmio = 0;
            // DMA pages stay pinned, including when hardware fails to stop.
            // Do not reuse an old controller's command buffers on the next one.
            dma_phys = 0;
        }
        if (!ready) print((char*)"Audio: no supported analog HDA output; inspect controller/codec diagnostics");
        else {
            uint32_t subsystem=pci_cfg_read32(active.ecam,active.start,active.bus,active.dev,active.fn,0x2c);
            if(thinkpad_audio_identity(active.id,subsystem,codec_id)) {
                if(thinkpad_audio::control(true,3)==0) thinkpad_audio::configure_linux_hotkeys();
            }
        }
    }
    int64_t result = -1;
    if (ready) {
        if (action >= 3) {
            uint32_t subsystem = pci_cfg_read32(active.ecam, active.start, active.bus, active.dev, active.fn, 0x2c);
            bool identity = thinkpad_audio_identity(active.id, subsystem, codec_id);
            unsigned received, dropped;
            { TtyIrqGuard guard; received = g_audio_hotkeys.received; dropped = g_audio_hotkeys.dropped; }
            log_hex("ThinkPad audio: PS/2 multimedia key presses received", received);
            log_hex("ThinkPad audio: PS/2 multimedia queue overflow", dropped);
            result = thinkpad_audio::control(identity, (unsigned)action - 3);
            if (result == -6) print((char*)"ThinkPad audio: firmware control unavailable on this platform");
        } else if (!action) result = available;
        else {
            if (!kind) kind = available & 2 ? 1 : available & 4 ? 2 : 3;
            HdaRoute alternate{};
            if (!(available & (1u << kind))) result = -3;
            else if (action == 2 && !hda_select_output_route(widgets, kind, alternate, true)) {
                result = -5;
                print((char*)"Audio: no alternate DAC route advertised");
            } else result = tone(kind, action == 2) ? 0 : -1;
            print((char*)(result == 0 ? "Audio: test DMA completed" : "Audio: test failed or route unavailable"));
        }
    }
    { TtyIrqGuard guard; busy = false; }
    return result;
}
#include "hda_pcm.h"
} // namespace hda
#endif
