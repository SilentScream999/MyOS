#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "src/drivers/xhci_port_state.h"

int main() {
    assert(!xhci_port_probe_needed(0));
    assert(xhci_port_probe_needed(1));
    assert(xhci_port_probe_needed(1u << 24)); // Cold attach before CCS.
    assert(xhci_port_reset_bit(1u << 24) == (1u << 31));
    assert(xhci_port_reset_bit(1u | (4u << 10) | (6u << 5)) == (1u << 31));
    assert(xhci_port_reset_bit(1u | (4u << 10) | (10u << 5)) == (1u << 31));
    assert(xhci_port_reset_bit(1u | (4u << 10)) == (1u << 4));
    assert(xhci_port_reset_bit(1u | (3u << 10)) == (1u << 4));
    constexpr uint32_t enabled = 3 | (1u << 9) | (4u << 10);
    XHCIPortWatch port{};
    xhci_port_watch_init(port, enabled);
    // A stream of reset/link/config notifications must never count as hotplug.
    for (uint64_t t = 0; t < 10000; ++t)
        assert(!xhci_port_watch_changed(port, enabled | (127u << 17) | ((t % 4) << 5), t, 150));
    // A bounce returns to the baseline without forcing a controller reset.
    assert(!xhci_port_watch_changed(port, 0, 10000, 150));
    assert(!xhci_port_watch_changed(port, enabled, 10149, 150));
    assert(!xhci_port_watch_changed(port, 0, 11000, 150));
    assert(!xhci_port_watch_changed(port, 0, 11149, 150));
    assert(xhci_port_watch_changed(port, 0, 11150, 150));
    // Software polling still detects a real insert even without any PSC bits.
    xhci_port_watch_init(port, 0);
    assert(!xhci_port_watch_changed(port, enabled, 20000, 150));
    assert(xhci_port_watch_changed(port, enabled, 20150, 150));
    // B850 regression: delayed PED completion after enumeration's reset timeout
    // must not reset the whole controller and repeat the same sequence forever.
    xhci_port_watch_init(port, 1);
    for (uint64_t t = 0; t < 2000; ++t)
        assert(!xhci_port_watch_changed(port, enabled, t, 150));
    // Losing enable alone likewise is not a topology change.
    xhci_port_watch_init(port, enabled);
    assert(!xhci_port_watch_changed(port, 1, 30000, 150));
    assert(!xhci_port_watch_changed(port, 1, 30150, 150));
    // Independent port timers: chatter must not postpone a different removal.
    XHCIPortWatch other{};
    xhci_port_watch_init(other, enabled);
    xhci_port_watch_init(port, enabled);
    for (uint64_t t = 0; t <= 150; ++t) {
        assert(!xhci_port_watch_changed(port, (t & 1) ? enabled : 0, t, 150));
        assert(xhci_port_watch_changed(other, 0, t, 150) == (t == 150));
    }
    // Acknowledge all change flags without starting reset, disabling PED,
    // latching a link command, warm-resetting, or clearing RW power/wake bits.
    uint32_t sc = enabled | (127u << 17) | (7u << 25) | (3u << 14) | (3u << 5);
    uint32_t ack = xhci_port_ack(sc | (1u << 31) | (1u << 16) | (1u << 4));
    assert((ack & (127u << 17)) == (127u << 17));
    assert(!(ack & ((1u << 1) | (1u << 4) | (1u << 16) | (1u << 31))));
    assert((ack & ((7u << 25) | (1u << 9))) == ((7u << 25) | (1u << 9)));
    assert(usb_ep0_packet_size(4, 9) == 512);
    assert(usb_ep0_packet_size(5, 9) == 512);
    assert(!usb_ep0_packet_size(4, 64));
    assert(usb_ep0_packet_size(3, 64) == 64);
    assert(!usb_ep0_packet_size(3, 9));
    assert(usb_ep0_packet_size(2, 8) == 8);
    assert(!usb_ep0_packet_size(2, 64));
    for (uint8_t size : {8, 16, 32, 64}) assert(usb_ep0_packet_size(1, size) == size);
    assert(!usb_ep0_packet_size(1, 0));
    // A connected -> disconnected -> connected sequence between samplings has
    // unchanged CCS but latched CSC. Acknowledging it must not lose replacement.
    XHCIPortChangeLatch replacement{};
    xhci_port_watch_init(port, enabled);
    xhci_port_note_change(replacement, port.baseline, enabled | (1u << 17), 1000);
    assert(!xhci_port_watch_changed(port, enabled, 1000, 150));
    assert(!xhci_port_replaced(replacement, enabled, 1149, 150));
    assert(xhci_port_replaced(replacement, enabled, 1150, 150));
    // Another actual connection transition restarts debounce; reset/link changes
    // do not create or extend a replacement request.
    xhci_port_note_change(replacement, 1, enabled | (1u << 17), 1200);
    assert(!xhci_port_replaced(replacement, enabled, 1250, 150));
    xhci_port_note_change(replacement, 1, enabled | (1u << 21), 1300);
    assert(xhci_port_replaced(replacement, enabled, 1350, 150));
    assert(!xhci_port_replaced(replacement, 0, 1350, 150));
    replacement.replacement = false;
    xhci_port_note_change(replacement, 0, enabled | (1u << 17), 1400);
    assert(!xhci_port_replaced(replacement, enabled, 1600, 150));
    assert(xhci_interrupt_interval(1, 255) == 10);
    assert(xhci_interrupt_interval(2, 10) == 6);
    assert(xhci_interrupt_interval(1, 1) == 3);
    assert(xhci_interrupt_interval(3, 1) == 0);
    assert(xhci_interrupt_interval(3, 4) == 3);
    assert(xhci_interrupt_interval(4, 16) == 15);
    puts("xHCI port notifications, debounce, safe acknowledgments and EP0 sizes passed");
}
