#include <cassert>
#include <cstdio>
#include <string>
#include "src/core/structures.h"
#include "src/drivers/xhci_port_state.h"
static volatile uint32_t portsc;
static uint64_t now;
static uint32_t issued, completed;
static bool finish_reset;
static std::string messages;
static volatile uint32_t* usb_root_portsc(XHCIController*, uint32_t) { return &portsc; }
static uint64_t usb_timeout_deadline(XHCIController*, uint64_t ms) { issued = portsc; return now + ms; }
static uint64_t _usb_rdtsc() { return ++now; }
static void tsc_delay_ms(uint64_t ms) {
    now += ms;
    if (finish_reset && now >= 5) portsc = completed;
}
static void print(char* s) { messages += s; }
static void to_str(uint64_t n, char* s) { std::sprintf(s, "%llu", (unsigned long long)n); }
static void to_hex(uint64_t n, char* s) { to_str(n, s); }
#include "../.storage-test/usb_root_reset_under_test.h"
static void setup(uint32_t initial, uint32_t final, bool finish = true) {
    portsc = initial; completed = final; finish_reset = finish;
    now = 0; issued = 0; messages.clear();
}
int main() {
    XHCIController hc{};
    setup(1u << 24, (4u << 10) | 3u);
    assert(usb_root_reset(&hc, 0));
    assert((issued & (1u << 31)) && !(issued & (1u << 4)) && now >= 15);
    setup((4u << 10) | (6u << 5) | 1u, (4u << 10) | 3u);
    assert(usb_root_reset(&hc, 0) && (issued & (1u << 31)));
    setup((3u << 10) | 1u, (3u << 10) | 3u);
    assert(usb_root_reset(&hc, 0) && (issued & (1u << 4)) && !(issued & (1u << 31)));
    setup((1u << 24) | (1u << 31), (4u << 10) | 3u);
    assert(usb_root_reset(&hc, 0) && (issued & (1u << 24)));
    setup(1u << 24, 0, false);
    assert(!usb_root_reset(&hc, 0) && now >= 500 && messages.find("not ready") != std::string::npos);
    setup(0, 0);
    assert(!usb_root_reset(&hc, 0) && issued == 0);
    std::puts("Actual root reset: cold attach before CCS, warm recovery, USB 2 reset, pending reset and bounded failure passed");
}
