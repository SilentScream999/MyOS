#ifndef rtc_h
#define rtc_h

// ── rtc.h ─────────────────────────────────────────────────────────────────────
//
// Wall-clock time from the CMOS real-time clock (RTC), for the taskbar clock.
//
// Split in two on purpose (see docs/AI_HANDOFF.md, decision 3):
//   1. PURE logic (no kernel dependencies, testable on a PC): register decoding
//      (BCD/binary, 12h/24h), date arithmetic, UTC-offset handling, text formatting.
//   2. Thin HARDWARE glue (ports 0x70/0x71), compiled out with -DRTC_NO_HW.
//
// WHICH TIME IS IN THE RTC?
//   Windows keeps LOCAL time in the RTC. Linux and QEMU (default) keep UTC.
//   The default below (offset 0) shows the RTC value as-is, which matches the
//   Windows clock on Windows-installed test machines. If the clock is off by whole
//   hours, set CLOCK_UTC_OFFSET_MIN (e.g. -420 for UTC-7) on the build command:
//        make clean && make iso CLOCK_UTC_OFFSET_MIN=-420      (see Makefile)
//   For QEMU, either pass -rtc base=localtime (the Makefile's `run` does) or use an offset.
//
// DESIGN NOTES
//   * The compositor thread calls rtc_poll() ~4x per second and repaints the tray only when the
//     displayed text changed. Drawing never touches I/O ports; it only reads g_clock.
//   * The RTC is read "until two consecutive reads agree", with a bounded wait on the
//     update-in-progress flag, so a rollover mid-read can never give a bogus time
//     and a missing RTC can never hang the compositor.
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include <stdbool.h>

// Minutes to ADD to the RTC value to get the time to display. 0 = RTC already holds local time.
#ifndef CLOCK_UTC_OFFSET_MIN
#define CLOCK_UTC_OFFSET_MIN 0
#endif

// 1 = show seconds in the tray ("3:07:09 PM"), 0 = "3:07 PM".
#ifndef CLOCK_SHOW_SECONDS
#define CLOCK_SHOW_SECONDS 0
#endif

// ── PURE LOGIC ────────────────────────────────────────────────────────────────

struct RtcTime  { uint16_t year; uint8_t month, day, hour, minute, second; };       // hour is 0..23
struct RtcRaw   { uint8_t sec, min, hour, day, mon, year, century, regb; };         // raw CMOS bytes

static inline bool rtc_bcd_ok(uint8_t v)  { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }
static inline uint8_t rtc_bcd(uint8_t v)  { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }

// Decode raw CMOS bytes into a validated RtcTime. Returns false if anything is out of range
// (dead battery garbage, floating bus, ...).
static inline bool rtc_decode(const RtcRaw& r, RtcTime* out) {
    const bool binary = (r.regb & 0x04) != 0;   // status B bit 2: 1 = binary, 0 = BCD
    const bool h24    = (r.regb & 0x02) != 0;   // status B bit 1: 1 = 24-hour, 0 = 12-hour
    const bool pm     = !h24 && (r.hour & 0x80);
    const uint8_t hraw = (uint8_t)(r.hour & 0x7F);  // in 12h mode bit 7 is the PM flag, not part of the value

    uint8_t s = r.sec, m = r.min, h = hraw, d = r.day, mo = r.mon, y = r.year, c = r.century;
    if (!binary) {
        if (!rtc_bcd_ok(s) || !rtc_bcd_ok(m) || !rtc_bcd_ok(h) ||
            !rtc_bcd_ok(d) || !rtc_bcd_ok(mo) || !rtc_bcd_ok(y)) return false;
        s = rtc_bcd(s); m = rtc_bcd(m); h = rtc_bcd(h);
        d = rtc_bcd(d); mo = rtc_bcd(mo); y = rtc_bcd(y);
        if (rtc_bcd_ok(c)) c = rtc_bcd(c); else c = 0;
    }

    if (!h24) {                         // 12h clock: 1..12, 12 AM = 0h, 12 PM = 12h
        if (h < 1 || h > 12) return false;
        if (h == 12) h = 0;
        if (pm) h = (uint8_t)(h + 12);
    }

    // The century register (usually CMOS 0x32) is not guaranteed to exist; trust it only if plausible.
    uint16_t year = (c >= 19 && c <= 21) ? (uint16_t)(c * 100 + y) : (uint16_t)(2000 + y);

    if (s > 59 || m > 59 || h > 23 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;

    out->year = year; out->month = mo; out->day = d;
    out->hour = h; out->minute = m; out->second = s;
    return true;
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian). Howard Hinnant's algorithm.
static inline int64_t rtc_days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static inline void rtc_civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

// Add a (possibly negative) number of minutes, rolling over day / month / year correctly.
static inline void rtc_add_minutes(RtcTime* t, int32_t minutes) {
    if (minutes == 0) return;
    int64_t days = rtc_days_from_civil(t->year, t->month, t->day);
    int64_t secs = days * 86400 + (int64_t)t->hour * 3600 + (int64_t)t->minute * 60 + t->second
                 + (int64_t)minutes * 60;
    int64_t nd = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; nd -= 1; }            // floor division for pre-midnight results
    int64_t y; unsigned mo, d;
    rtc_civil_from_days(nd, &y, &mo, &d);
    t->year = (uint16_t)y; t->month = (uint8_t)mo; t->day = (uint8_t)d;
    t->hour = (uint8_t)(rem / 3600); t->minute = (uint8_t)((rem % 3600) / 60); t->second = (uint8_t)(rem % 60);
}

static inline char* rtc_put2(char* p, unsigned v) { *p++ = (char)('0' + (v / 10) % 10); *p++ = (char)('0' + v % 10); return p; }

// "3:07 PM"  (or "3:07:09 PM" with seconds). Returns the length. `buf` needs >= 12 bytes.
static inline int rtc_format_time(const RtcTime& t, char* buf, bool with_seconds) {
    char* p = buf;
    unsigned h12 = t.hour % 12; if (h12 == 0) h12 = 12;
    if (h12 >= 10) *p++ = '1';
    *p++ = (char)('0' + h12 % 10);
    *p++ = ':'; p = rtc_put2(p, t.minute);
    if (with_seconds) { *p++ = ':'; p = rtc_put2(p, t.second); }
    *p++ = ' '; *p++ = (t.hour >= 12) ? 'P' : 'A'; *p++ = 'M';
    *p = 0;
    return (int)(p - buf);
}

// "2026-11-06". Returns the length (10). `buf` needs >= 11 bytes.
static inline int rtc_format_date(const RtcTime& t, char* buf) {
    char* p = buf;
    p = rtc_put2(p, t.year / 100); p = rtc_put2(p, t.year % 100);
    *p++ = '-'; p = rtc_put2(p, t.month);
    *p++ = '-'; p = rtc_put2(p, t.day);
    *p = 0;
    return (int)(p - buf);
}

// ── HARDWARE GLUE (needs outb/inb from irq.h) ─────────────────────────────────
#ifndef RTC_NO_HW

#define CMOS_INDEX  0x70u
#define CMOS_DATA   0x71u
#define CMOS_REG_SEC     0x00
#define CMOS_REG_MIN     0x02
#define CMOS_REG_HOUR    0x04
#define CMOS_REG_DAY     0x07
#define CMOS_REG_MON     0x08
#define CMOS_REG_YEAR    0x09
#define CMOS_REG_STATA   0x0A
#define CMOS_REG_STATB   0x0B
#define CMOS_REG_CENTURY 0x32   // conventional location; validated in rtc_decode

// One index/data access. Interrupts are masked for the pair so a task switch cannot land between the
// outb and the inb (the index latch is shared). The NMI-disable bit (0x80) is deliberately NOT set.
static inline uint8_t cmos_read(uint8_t reg) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    outb(CMOS_INDEX, reg);
    uint8_t v = inb(CMOS_DATA);
    if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
    return v;
}

// Read all time registers once update-in-progress is clear. Bounded: returns false (never hangs)
// if the flag never clears or the chip is absent (a floating bus reads 0xFF).
static inline bool rtc_read_raw_once(RtcRaw* r) {
    for (int guard = 0; ; guard++) {
        uint8_t a = cmos_read(CMOS_REG_STATA);
        if (a == 0xFF) return false;              // no RTC / floating bus
        if (!(a & 0x80)) break;                   // UIP clear: safe to read
        if (guard > 4000) return false;
    }
    r->sec  = cmos_read(CMOS_REG_SEC);   r->min  = cmos_read(CMOS_REG_MIN);
    r->hour = cmos_read(CMOS_REG_HOUR);  r->day  = cmos_read(CMOS_REG_DAY);
    r->mon  = cmos_read(CMOS_REG_MON);   r->year = cmos_read(CMOS_REG_YEAR);
    r->century = cmos_read(CMOS_REG_CENTURY);
    r->regb = cmos_read(CMOS_REG_STATB);
    return true;
}

static inline bool rtc_raw_equal(const RtcRaw& a, const RtcRaw& b) {
    return a.sec == b.sec && a.min == b.min && a.hour == b.hour && a.day == b.day &&
           a.mon == b.mon && a.year == b.year && a.century == b.century && a.regb == b.regb;
}

// Two consecutive identical reads = a consistent snapshot (no rollover in between).
static inline bool rtc_read_time(RtcTime* out) {
    RtcRaw a, b;
    if (!rtc_read_raw_once(&a)) return false;
    for (int tries = 0; tries < 5; tries++) {
        if (!rtc_read_raw_once(&b)) return false;
        if (rtc_raw_equal(a, b)) {
            if (!rtc_decode(b, out)) return false;
            rtc_add_minutes(out, CLOCK_UTC_OFFSET_MIN);
            return true;
        }
        a = b;
    }
    return false;
}

// ── Cached clock state (single writer: the compositor thread) ────────────────
static RtcTime g_clock = { 2000, 1, 1, 0, 0, 0 };
static bool    g_clock_valid = false;

// Re-read the RTC. Returns true when what the tray shows has CHANGED (so the caller repaints it),
// including a valid<->invalid transition. A failed read keeps the last good time on screen.
static inline bool rtc_poll() {
    RtcTime t;
    if (!rtc_read_time(&t)) return false;
    bool changed = !g_clock_valid || t.minute != g_clock.minute || t.hour != g_clock.hour ||
                   t.day != g_clock.day || t.month != g_clock.month || t.year != g_clock.year;
#if CLOCK_SHOW_SECONDS
    changed = changed || t.second != g_clock.second;
#endif
    g_clock = t;
    g_clock_valid = true;
    return changed;
}

#endif // RTC_NO_HW
#endif // rtc_h
