#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include "../src/drivers/usb_storage_completion.h"
#define MAX_CONTROLLERS 2
enum class TaskState { READY, RUNNING, BLOCKED };
struct Task { TaskState state; uint64_t wake_at_tick; };
struct XHCIController {};
static XHCIController xhci_hc[MAX_CONTROLLERS];
struct USBDevice { XHCIController* hc; uint8_t slot_id; };
struct TRB { uint64_t parameter; uint32_t status, control; };
struct USB_Response { bool gotresponse; TRB* event; uint32_t ctrl, type; };
struct USBControlGuard {};
static Task task{TaskState::RUNNING, 0};
static Task* g_current_task = &task;
static bool g_scheduler_ready = true;
static uint64_t ticks;
static unsigned sleeps, deferred;
static bool background_completion, queue_full;
static std::deque<TRB> events;
static TRB delivered;
static uint64_t _usb_rdtsc() { return ++ticks; }
static uint64_t _usb_tsc_hz() { return 1000; }
static void boot_splash_tick() {}
static void print(char*) {}
static void to_hex(uint64_t n, char* s) { snprintf(s, 32, "%llx", (unsigned long long)n); }
static void to_str(uint64_t n, char* s) { snprintf(s, 32, "%llu", (unsigned long long)n); }
static void task_sleep_ms(uint64_t);
static USB_Response get_usb_response_raw(XHCIController*, int) {
    if (events.empty()) return {};
    delivered = events.front(); events.pop_front();
    return {true, &delivered, delivered.control, (delivered.control >> 10) & 63};
}
static bool usb_defer(XHCIController*, const USB_Response&);
#include "../.storage-test/usb_storage_wait_under_test.h"
static bool usb_defer(XHCIController* hc, const USB_Response& r) {
    if (usb_storage_dispatch_completion(hc, r)) return true;
    ++deferred; return !queue_full;
}
static uint32_t control(unsigned type, unsigned slot, unsigned dci) {
    return type << 10 | slot << 24 | dci << 16;
}
static void task_sleep_ms(uint64_t ms) {
    ++sleeps; ticks += ms;
    if (background_completion) {
        background_completion = false;
        task.state = TaskState::BLOCKED; task.wake_at_tick = 99999;
        TRB event{77, 1u << 24, control(32, 3, 5)};
        USB_Response r{true, &event, event.control, 32};
        assert(usb_storage_dispatch_completion(&xhci_hc[0], r));
        assert(task.state == TaskState::READY && task.wake_at_tick == 0);
        task.state = TaskState::RUNNING;
    }
}
int main() {
    USBDevice dev{&xhci_hc[0], 3};
    assert(usb_storage_begin_wait(&dev, 32, 77, 5));
    assert(!usb_storage_begin_wait(&dev, 32, 88, 5));
    background_completion = true;
    assert(usb_storage_wait(&dev, 32, 77, 5, 512) && sleeps == 1);
    assert(!usb_storage_completions[0][3].active && !usb_storage_waiters[0][3]);
    assert(usb_storage_begin_wait(&dev, 32, 88, 5));
    events.push_back({77, 1u << 24, control(32, 3, 5)}); // Late previous completion.
    events.push_back({88, 1u << 24, control(32, 4, 5)}); // Other slot.
    events.push_back({88, 1u << 24, control(32, 3, 7)}); // Other endpoint.
    events.push_back({88, (1u << 24) | 1u, control(32, 3, 5)}); // Short transfer.
    assert(!usb_storage_wait(&dev, 32, 88, 5, 512) && deferred == 3);
    assert(usb_storage_begin_wait(&dev, 33, 99, 0));
    TRB e{99, 1u << 24, control(33, 3, 0)};
    USB_Response r{true, &e, e.control, 33};
    assert(!usb_storage_dispatch_completion(&xhci_hc[1], r));
    assert(usb_storage_dispatch_completion(&xhci_hc[0], r));
    assert(usb_storage_wait(&dev, 33, 99, 0, 0));
    assert(usb_storage_begin_wait(&dev, 32, 100, 5));
    assert(!usb_storage_wait(&dev, 32, 100, 5, 512)); // Bounded missing completion.
    assert(!usb_storage_completions[0][3].active && !usb_storage_waiters[0][3]);
    assert(usb_storage_begin_wait(&dev, 32, 101, 5));
    queue_full = true;
    events.push_back({500, 1u << 24, control(32, 4, 5)});
    assert(!usb_storage_wait(&dev, 32, 101, 5, 512));
    assert(!usb_storage_completions[0][3].active);
    puts("Actual USB wait: background completion, wakeup, exact ownership, late/short events, timeout and queue failure passed");
}
