#include <cassert>
#include <cstdio>
#include "src/drivers/usb_hub_poll_policy.h"

int main() {
    USBHubPollSchedule s = {};
    s.audit_after = 250;
    bool audited;
    assert(usb_hub_poll_plan(s, 8, 249, audited) == 0 && !audited);
    unsigned requests = 0;
    for (unsigned port = 1; port <= 8; ++port) {
        const uint64_t now = port * 250;
        const uint32_t mask = usb_hub_poll_plan(s, 8, now, audited);
        assert(audited && mask == ((1u << port) | (port == 8 ? 1u : 0u)));
        requests += __builtin_popcount(mask);
        usb_hub_poll_finish(s, now, 1, audited, false);
        assert(usb_hub_poll_plan(s, 8, now + 249, audited) == 0);
    }
    assert(requests == 9); // Previous full scans: 72 requests in two seconds.

    s.pending = (1u << 2) | (1u << 5);
    assert(usb_hub_poll_plan(s, 8, 2010, audited) == s.pending && !audited);
    s.pending &= ~(1u << 2); // Handling one notification preserves its sibling.
    usb_hub_poll_finish(s, 2020, 1, false, false);
    assert(s.pending_after == 2070);
    assert(usb_hub_poll_plan(s, 8, 2069, audited) == 0);
    assert(usb_hub_poll_plan(s, 8, 2070, audited) == (1u << 5));
    usb_hub_poll_finish(s, 2080, 1, false, true);
    assert(s.pending_after == 2330);
    assert(usb_hub_poll_plan(s, 8, 2329, audited) == 0);
    assert(usb_hub_poll_plan(s, 8, 2330, audited) == ((1u << 5) | (1u << 1)) && audited);
    // Slow requests cannot leave an overdue audit and cause a busy catch-up loop.
    s.pending = 0;
    usb_hub_poll_finish(s, 9000, 1, true, false);
    assert(s.audit_after == 9250 && s.pending_after == 0);
    assert(usb_hub_poll_plan(s, 8, 9249, audited) == 0);
    assert(usb_hub_poll_plan(s, 0, 10000, audited) == 0);
    assert(usb_hub_poll_plan(s, 17, 10000, audited) == 0);
    std::puts("Hub poll policy: idle budget, immediate notifications, debounce and retry deadlines passed");
}
