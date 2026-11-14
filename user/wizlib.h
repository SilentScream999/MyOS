#ifndef WYZAR_WIZLIB_H
#define WYZAR_WIZLIB_H

// ── wizlib.h ──────────────────────────────────────────────────────────────────
//
// Header-only helper for programs shipped as .wiz packages. It lets a running
// app read its own bundle (manifest, bundled data/ files) and keep per-app
// data outside the package:
//
//     wizlib_bundle b;
//     if (wizlib_open(&b) == 0) {
//         char text[256];
//         int64_t n = wizlib_read(&b, "data/greeting.txt", text, sizeof(text));
//         ... wizlib_data_save(&b, "highscore.txt", "42", 2);
//         wizlib_close(&b);
//     }
//
// The package is found through SYS_EXEPATH (the OS's /proc/self/exe), so it
// works wherever the .wiz lives: installed under /apps, on a USB stick, in a
// downloads folder. The package itself is read-only; anything the app writes
// belongs in its data directory, /home/.local/share/<id>/, which survives
// upgrades and removal unless the user asks `wiz remove --purge`.
//
// A plain .elf (not a package) makes wizlib_open() return WIZ_E_NOT_WIZ; apps
// should keep working without their bundle where that makes sense.
//
// Define WIZLIB_HOST_SYSCALLS before including to supply the wizlib_sys_*
// functions yourself (used by host tests).

#include "src/core/wiz.h"

typedef unsigned long wizlib_u64;
typedef long          wizlib_s64;

#define WIZLIB_PATH 256
#define WIZLIB_DATA_ROOT "/home/.local/share"

typedef struct { wizlib_u64 size, type; unsigned char reserved[16]; } wizlib_stat;  // mirrors file_info (32 bytes)
_Static_assert(sizeof(wizlib_stat) == 32, "wizlib_stat must match the kernel's file_info");

#ifndef WIZLIB_HOST_SYSCALLS
#define WIZLIB_SYSCALL(ret, number, ...)                                      \
    __asm__ volatile ("syscall" : "=a"(ret) : "a"((wizlib_u64)(number)), ##__VA_ARGS__ \
                      : "rcx", "r11", "memory")
static inline wizlib_u64 wizlib_sys_open(const char* path, wizlib_u64 flags) {
    wizlib_u64 r; WIZLIB_SYSCALL(r, 2, "D"(path), "S"(flags)); return r;
}
static inline void wizlib_sys_close(wizlib_u64 fd) {
    wizlib_u64 r; WIZLIB_SYSCALL(r, 3, "D"(fd)); (void)r;
}
static inline wizlib_u64 wizlib_sys_read(wizlib_u64 fd, void* buf, wizlib_u64 len) {
    wizlib_u64 r; WIZLIB_SYSCALL(r, 0, "D"(fd), "S"(buf), "d"(len)); return r;
}
static inline wizlib_u64 wizlib_sys_write(wizlib_u64 fd, const void* buf, wizlib_u64 len) {
    wizlib_u64 r; WIZLIB_SYSCALL(r, 1, "D"(fd), "S"(buf), "d"(len)); return r;
}
static inline wizlib_s64 wizlib_sys_lseek(wizlib_u64 fd, wizlib_s64 offset, wizlib_u64 whence) {
    wizlib_s64 r; WIZLIB_SYSCALL(r, 8, "D"(fd), "S"(offset), "d"(whence)); return r;
}
static inline wizlib_s64 wizlib_sys_stat(const char* path, wizlib_stat* info) {
    wizlib_s64 r; WIZLIB_SYSCALL(r, 4, "D"(path), "S"(info)); return r;
}
static inline wizlib_s64 wizlib_sys_mkdir(const char* path) {
    wizlib_s64 r; WIZLIB_SYSCALL(r, 83, "D"(path), "S"((wizlib_u64)0755)); return r;
}
static inline wizlib_s64 wizlib_sys_replace(const char* path, const void* data, wizlib_u64 len) {
    wizlib_s64 r; WIZLIB_SYSCALL(r, 138, "D"(path), "S"(data), "d"(len)); return r;
}
// Length of the caller's own executable path (written to buf), or -1.
static inline wizlib_s64 wizlib_sys_exepath(char* buf, wizlib_u64 size) {
    wizlib_s64 r; WIZLIB_SYSCALL(r, 210, "D"(buf), "S"(size)); return r;
}
#else
// Host mode: the including file defines these (static) after this header.
static wizlib_u64 wizlib_sys_open(const char* path, wizlib_u64 flags);
static void       wizlib_sys_close(wizlib_u64 fd);
static wizlib_u64 wizlib_sys_read(wizlib_u64 fd, void* buf, wizlib_u64 len);
static wizlib_u64 wizlib_sys_write(wizlib_u64 fd, const void* buf, wizlib_u64 len);
static wizlib_s64 wizlib_sys_lseek(wizlib_u64 fd, wizlib_s64 offset, wizlib_u64 whence);
static wizlib_s64 wizlib_sys_stat(const char* path, wizlib_stat* info);
static wizlib_s64 wizlib_sys_mkdir(const char* path);
static wizlib_s64 wizlib_sys_replace(const char* path, const void* data, wizlib_u64 len);
static wizlib_s64 wizlib_sys_exepath(char* buf, wizlib_u64 size);
#endif

typedef struct {
    wizlib_u64   fd;
    wiz_package  pkg;
    wiz_manifest manifest;
    char         path[WIZLIB_PATH];
} wizlib_bundle;

static int wizlib_read_at(void* ctx, wiz_u64 offset, void* buf, wiz_u64 len) {
    wizlib_u64 fd = ((wizlib_bundle*)ctx)->fd;
    char* p = (char*)buf;
    if (!len) return 1;
    if (wizlib_sys_lseek(fd, (wizlib_s64)offset, 0) < 0) return 0;
    while (len) {
        wizlib_u64 want = len > 4096 ? 4096 : len;
        wizlib_u64 got = wizlib_sys_read(fd, p, want);
        if (got == 0 || got > want) return 0;
        p += got; len -= got;
    }
    return 1;
}

// Open the running program's own package and read its manifest.
// Returns 0, or a WIZ_E_* code (WIZ_E_NOT_WIZ if this is a plain ELF).
static int wizlib_open(wizlib_bundle* b) {
    wizlib_s64 n = wizlib_sys_exepath(b->path, sizeof(b->path));
    if (n <= 0) return WIZ_E_IO;
    b->fd = wizlib_sys_open(b->path, 0);
    if (b->fd == (wizlib_u64)-1) return WIZ_E_IO;
    wizlib_s64 size = wizlib_sys_lseek(b->fd, 0, 2);
    int status = size < 0 ? WIZ_E_IO : wiz_open(&b->pkg, wizlib_read_at, b, (wiz_u64)size);
    if (status == 0) status = wiz_read_manifest(&b->pkg, &b->manifest);
    if (status == 0) status = wiz_manifest_check_package(&b->manifest);
    if (status) wizlib_sys_close(b->fd);
    return status;
}
static void wizlib_close(wizlib_bundle* b) { wizlib_sys_close(b->fd); }

static const char* wizlib_id(const wizlib_bundle* b)      { return b->manifest.id; }
static const char* wizlib_name(const wizlib_bundle* b)    { return b->manifest.name; }
static const char* wizlib_version(const wizlib_bundle* b) { return b->manifest.version; }

// Read bundled file `entry` (e.g. "data/level1.txt") into buf. Returns the
// file's size, a WIZ_E_* code on failure, or WIZ_E_TOO_BIG if it does not fit
// in `capacity` (nothing is copied then, so the caller can size a buffer).
static wizlib_s64 wizlib_read(wizlib_bundle* b, const char* entry, void* buf, wizlib_u64 capacity) {
    wiz_entry e;
    int status = wiz_find(&b->pkg, entry, &e);
    if (status < 0) return status;
    if (e.size > capacity) return WIZ_E_TOO_BIG;
    status = wiz_read_entry(&b->pkg, &e, 0, buf, e.size);
    return status ? status : (wizlib_s64)e.size;
}
// Size of a bundled file without reading it, or a WIZ_E_* code.
static wizlib_s64 wizlib_size(wizlib_bundle* b, const char* entry) {
    wiz_entry e; int status = wiz_find(&b->pkg, entry, &e);
    return status < 0 ? status : (wizlib_s64)e.size;
}

// ── Per-app data directory ────────────────────────────────────────────────────
static int wizlib_join(char* out, unsigned capacity, const char* a, const char* b, const char* c) {
    unsigned n = 0; const char* parts[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) for (const char* p = parts[i]; *p; ++p) { if (n + 1 >= capacity) return 0; out[n++] = *p; }
    out[n] = 0; return 1;
}
static int wizlib_is_dir(const char* path) {
    wizlib_stat st; return wizlib_sys_stat(path, &st) == 0 && st.type == 2;
}
// Create (if needed) /home/.local/share/<id>/ and write its path to out.
static int wizlib_data_dir(const wizlib_bundle* b, char* out, unsigned capacity) {
    wizlib_sys_mkdir("/home"); wizlib_sys_mkdir("/home/.local"); wizlib_sys_mkdir(WIZLIB_DATA_ROOT);
    if (!wizlib_join(out, capacity, WIZLIB_DATA_ROOT "/", b->manifest.id, "")) return WIZ_E_TOO_BIG;
    wizlib_sys_mkdir(out);
    return wizlib_is_dir(out) ? 0 : WIZ_E_IO;
}
static int wizlib_data_path(const wizlib_bundle* b, const char* file, char* out, unsigned capacity) {
    char dir[WIZLIB_PATH];
    if (!wiz_safe_name(file, wiz_str_len(file))) return WIZ_E_UNSAFE;      // no "..", no absolute paths
    int status = wizlib_data_dir(b, dir, sizeof(dir));
    if (status) return status;
    return wizlib_join(out, capacity, dir, "/", file) ? 0 : WIZ_E_TOO_BIG;
}
// Read a file from the app's data directory. Returns its size or a WIZ_E_* code.
static wizlib_s64 wizlib_data_load(const wizlib_bundle* b, const char* file, void* buf, wizlib_u64 capacity) {
    char path[WIZLIB_PATH]; int status = wizlib_data_path(b, file, path, sizeof(path));
    if (status) return status;
    wizlib_u64 fd = wizlib_sys_open(path, 0);
    if (fd == (wizlib_u64)-1) return WIZ_E_NOENT;
    wizlib_u64 used = 0;
    for (;;) {
        if (used >= capacity) { wizlib_u64 probe; char c; probe = wizlib_sys_read(fd, &c, 1);
                                wizlib_sys_close(fd); return probe == 0 ? (wizlib_s64)used : WIZ_E_TOO_BIG; }
        wizlib_u64 got = wizlib_sys_read(fd, (char*)buf + used, capacity - used);
        if (got == 0) break;
        if (got > capacity - used) { wizlib_sys_close(fd); return WIZ_E_IO; }
        used += got;
    }
    wizlib_sys_close(fd); return (wizlib_s64)used;
}
// Atomically replace a file in the app's data directory.
static int wizlib_data_save(const wizlib_bundle* b, const char* file, const void* data, wizlib_u64 len) {
    char path[WIZLIB_PATH]; int status = wizlib_data_path(b, file, path, sizeof(path));
    if (status) return status;
    return wizlib_sys_replace(path, data, len) == 0 ? 0 : WIZ_E_IO;
}

#endif
