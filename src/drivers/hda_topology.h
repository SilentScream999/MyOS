#ifndef WYZAR_HDA_TOPOLOGY_H
#define WYZAR_HDA_TOPOLOGY_H
#include <stdint.h>

// HDA connection lists encode ranges; selector indices address the expanded list.
// Keep this parser and route search independent of PCI/MMIO for host tests.
struct HdaConnections {
    uint8_t nodes[32]{};
    unsigned count = 0;
    unsigned previous = 0;
    bool previous_range = false;
    bool append(unsigned entry, bool long_form) {
        unsigned range_bit = long_form ? 0x8000 : 0x80;
        unsigned nid = entry & (range_bit - 1);
        bool range = (entry & range_bit) != 0;
        if (!nid || nid > 255 || (range && (!count || previous_range || nid <= previous))) return false;
        unsigned first = range ? previous + 1 : nid;
        if (nid - first + 1 > 32 - count) return false;
        for (unsigned n = first; n <= nid; ++n) nodes[count++] = (uint8_t)n;
        previous = nid;
        previous_range = range;
        return true;
    }
};
struct HdaWidget {
    uint32_t caps = 0, pin_caps = 0, config = 0;
    uint32_t input_amp = 0, output_amp = 0;
    HdaConnections inputs{};
    bool present = false, pcm = false;
    uint32_t pcm_rates = 0;
    unsigned type() const { return (caps >> 20) & 15; }
};
struct HdaRoute {
    uint8_t nodes[16]{}, indices[16]{};
    unsigned count = 0;
};
static inline bool hda_find_route(const HdaWidget* widgets, unsigned nid, HdaRoute& route, unsigned excluded_dac = 0) {
    if (!nid || nid > 255 || route.count == 16 || !widgets[nid].present) return false;
    for (unsigned i = 0; i < route.count; ++i) if (route.nodes[i] == nid) return false;
    const auto& w = widgets[nid];
    unsigned depth = route.count++;
    route.nodes[depth] = (uint8_t)nid;
    if (w.type() == 0 && nid != excluded_dac && !(w.caps & (1u << 9)) && w.pcm) return true;
    // Only traverse analog output pins, selectors and mixers. Never route inputs
    // or vendor widgets merely because they happen to lead to a converter.
    if (w.type() == 2 || w.type() == 3 || (w.type() == 4 && !(w.caps & (1u << 9)))) {
        for (unsigned i = 0; i < w.inputs.count; ++i) {
            route.indices[depth] = (uint8_t)i;
            if (hda_find_route(widgets, w.inputs.nodes[i], route, excluded_dac)) return true;
        }
    }
    route.count = depth;
    return false;
}
// 1=speakers, 2=headphones, 3=line out. Skip disconnected/digital/input pins.
static inline unsigned hda_pin_kind(const HdaWidget& w) {
    if (!w.present || w.type() != 4 || !(w.pin_caps & (1u << 4)) ||
        (w.caps & (1u << 9)) || ((w.config >> 30) & 3) == 1) return 0;
    switch ((w.config >> 20) & 15) {
        case 1: return 1;
        case 2: return 2;
        case 0: return 3;
        default: return 0;
    }
}
static inline bool hda_select_output_route(const HdaWidget* widgets, unsigned kind,
                                         HdaRoute& route, bool alternate = false) {
    route = {};
    for (unsigned nid = 1; nid < 256; ++nid) {
        if (hda_pin_kind(widgets[nid]) != kind || !hda_find_route(widgets, nid, route)) continue;
        if (!alternate) return true;
        unsigned dac = route.nodes[route.count - 1];
        route = {};
        // Keep the same physical pin; only vary a reachable, supported DAC.
        return hda_find_route(widgets, nid, route, dac);
    }
    return false;
}
// A gain step is an index, not a signed dB value. Zero-dB offset must be
// clamped to the codec's implemented range; never turn every amplifier to max.
static inline unsigned hda_amp_gain(uint32_t caps) {
    unsigned offset = caps & 127, steps = (caps >> 8) & 127;
    return offset < steps ? offset : steps;
}
#endif
