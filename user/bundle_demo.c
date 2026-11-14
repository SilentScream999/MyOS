// bundle_demo: a tiny app that shows the .wiz bundle API (user/wizlib.h).
//
// Build:   make bundle_demo.wiz
// Try it:  run /share/samples/bundle_demo.wiz        (portable: runs from anywhere)
//          wiz install /share/samples/bundle_demo.wiz (optional: adds it to Start)
//
// Each run reads its own name/version and a bundled text file, then bumps a run
// counter stored in its data directory (/home/.local/share/bundle-demo/).

#include "user/wizlib.h"

static inline void __attribute__((noreturn)) sys_exit(wizlib_u64 code) {
    __asm__ volatile ("syscall" : : "a"(60), "D"(code) : "rcx", "r11", "memory");
    for (;;) {}
}
static unsigned text_length(const char* s) { unsigned n = 0; while (s[n]) ++n; return n; }
static void print(const char* s) { wizlib_sys_write(1, s, text_length(s)); }
static void print_number(wizlib_u64 n) {
    char digits[24], out[24]; int used = 0, k = 0;
    do { digits[used++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (used) out[k++] = digits[--used];
    out[k] = 0; print(out);
}

void _start() {
    wizlib_bundle bundle;
    int status = wizlib_open(&bundle);
    if (status) {
        print("[bundle_demo] Not running from a .wiz package: ");
        print(wiz_error_text(status)); print("\n");
        sys_exit(1);
    }
    print("\n[bundle_demo] "); print(wizlib_name(&bundle)); print(" "); print(wizlib_version(&bundle));
    print(" (id "); print(wizlib_id(&bundle)); print(")\n");
    print("[bundle_demo] running from "); print(bundle.path); print("\n");

    char greeting[256];
    wizlib_s64 n = wizlib_read(&bundle, "data/greeting.txt", greeting, sizeof(greeting) - 1);
    if (n >= 0) { greeting[n] = 0; print(greeting); }
    else print("[bundle_demo] (no bundled greeting)\n");

    char saved[24]; wizlib_u64 runs = 0;
    n = wizlib_data_load(&bundle, "runs.txt", saved, sizeof(saved) - 1);
    for (wizlib_s64 i = 0; i < n; ++i) if (saved[i] >= '0' && saved[i] <= '9') runs = runs * 10 + (wizlib_u64)(saved[i] - '0');
    ++runs;
    char out[24]; int used = 0, k = 0; char digits[24]; wizlib_u64 v = runs;
    do { digits[used++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (used) out[k++] = digits[--used];
    if (wizlib_data_save(&bundle, "runs.txt", out, (wizlib_u64)k) == 0) {
        print("[bundle_demo] This is run number "); print_number(runs); print(" (saved in /home/.local/share/");
        print(wizlib_id(&bundle)); print("/)\n");
    } else print("[bundle_demo] Could not save to the data directory.\n");
    wizlib_close(&bundle);
    sys_exit(0);
}
