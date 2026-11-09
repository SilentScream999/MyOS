#ifndef WYZAR_XHCI_PORT_STATE_H
#define WYZAR_XHCI_PORT_STATE_H
#include <stdint.h>

// USB 3 encodes EP0's packet size as an exponent (9 means 512 bytes).
// USB 2 uses the byte count directly.
static inline uint16_t usb_ep0_packet_size(uint8_t speed, uint8_t encoded) {
    if (speed >= 4) return encoded == 9 ? 512 : 0;
    if (speed == 3) return encoded == 64 ? 64 : 0;
    if (speed == 2) return encoded == 8 ? 8 : 0;
    if (speed == 1 && (encoded == 8 || encoded == 16 || encoded == 32 || encoded == 64)) return encoded;
    return 0;
}

// PORTSC has write-one-to-clear and write-one-to-start fields. Never echo
// PED, PR, WPR or LWS while acknowledging a status notification.
static inline uint32_t xhci_port_ack(uint32_t sc) {
    constexpr uint32_t preserved = (15u << 5) | (1u << 9) |
                                  (3u << 14) | (7u << 25);
    return (sc & preserved) | (sc & (127u << 17));
}

struct XHCIPortWatch {
    uint8_t baseline, candidate;
    uint64_t since;
};

struct XHCIPortChangeLatch {
    bool replacement;
    uint64_t last_change;
};
static inline void xhci_port_note_change(XHCIPortChangeLatch& latch, uint8_t baseline,
                                        uint32_t sc, uint64_t now) {
    // CSC stays latched even if disconnect/reconnect both occur before polling.
    // Preserve that evidence before acknowledging PORTSC or the PSC interrupt.
    if ((sc & (1u << 17)) && baseline) {
        latch.replacement = true;
        latch.last_change = now;
    }
}
static inline bool xhci_port_replaced(const XHCIPortChangeLatch& latch, uint32_t sc,
                                      uint64_t now, uint64_t settle) {
    return latch.replacement && (sc & 1u) && now - latch.last_change >= settle;
}

// Only CCS identifies a topology change. PED can settle after our reset timeout
// or change during link recovery while the same device remains plugged in.
// Using PED as a restart trigger can make the reset cause its own next reset.
// Track each port separately so a noisy port cannot prevent another settling.
static inline uint8_t xhci_port_presence(uint32_t sc) {
    return sc & 1u;
}

static inline uint8_t xhci_interrupt_interval(uint8_t speed, uint8_t b_interval) {
    if (speed >= 3) {
        if (!b_interval) b_interval = 1;
        if (b_interval > 16) b_interval = 16;
        return b_interval - 1;
    }
    // FS/LS descriptors count frames; xHCI uses log2(microframes), rounding down.
    uint16_t microframes = (uint16_t)(b_interval ? b_interval : 1) * 8u;
    uint8_t exponent = 0;
    while (microframes >>= 1) ++exponent;
    return exponent;
}
static inline void xhci_port_watch_init(XHCIPortWatch& watch, uint32_t sc) {
    watch.baseline = watch.candidate = xhci_port_presence(sc);
    watch.since = 0;
}
static inline bool xhci_port_watch_changed(XHCIPortWatch& watch, uint32_t sc,
                                          uint64_t now, uint64_t settle_ticks) {
    uint8_t state = xhci_port_presence(sc);
    if (state == watch.baseline) {
        watch.candidate = state;
        watch.since = now;
        return false;
    }
    if (state != watch.candidate) {
        watch.candidate = state;
        watch.since = now;
        return false;
    }
    return now - watch.since >= settle_ticks;
}
#endif
