// Host test for user/wizlib.h with the syscalls faked over a sandbox directory.
//   wizlib_host ROOT PACKAGE_PATH PLAIN_ELF_PATH FUTURE_ABI_PACKAGE_PATH
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define WIZLIB_HOST_SYSCALLS
#include "user/wizlib.h"

static char root[512];
static char exe[256];
static const char* real(const char* p, char* out) { snprintf(out, 1024, "%s%s", root, p); return out; }
static wizlib_u64 wizlib_sys_open(const char* path, wizlib_u64 flags) {
    char r[1024]; int fd = open(real(path, r), flags == 0 ? O_RDONLY : (O_WRONLY | O_CREAT), 0644);
    return fd < 0 ? (wizlib_u64)-1 : (wizlib_u64)fd;
}
static void wizlib_sys_close(wizlib_u64 fd) { close((int)fd); }
static wizlib_u64 wizlib_sys_read(wizlib_u64 fd, void* b, wizlib_u64 n) { ssize_t r = read((int)fd, b, n); return r < 0 ? (wizlib_u64)-1 : (wizlib_u64)r; }
static wizlib_u64 wizlib_sys_write(wizlib_u64 fd, const void* b, wizlib_u64 n) { ssize_t r = write((int)fd, b, n); return r < 0 ? (wizlib_u64)-1 : (wizlib_u64)r; }
static wizlib_s64 wizlib_sys_lseek(wizlib_u64 fd, wizlib_s64 o, wizlib_u64 w) { return lseek((int)fd, o, w == 0 ? SEEK_SET : w == 1 ? SEEK_CUR : SEEK_END); }
static wizlib_s64 wizlib_sys_stat(const char* path, wizlib_stat* st) {
    char r[1024]; struct stat s; if (stat(real(path, r), &s) != 0) return -1;
    memset(st, 0, sizeof(*st)); st->size = (wizlib_u64)s.st_size; st->type = S_ISDIR(s.st_mode) ? 2 : 1; return 0;
}
static wizlib_s64 wizlib_sys_mkdir(const char* path) { char r[1024]; return mkdir(real(path, r), 0755) == 0 ? 0 : -1; }
static wizlib_s64 wizlib_sys_replace(const char* path, const void* d, wizlib_u64 n) {
    char r[1024]; FILE* f = fopen(real(path, r), "wb"); if (!f) return -1;
    int ok = fwrite(d, 1, n, f) == n; fclose(f); return ok ? 0 : -1;
}
static wizlib_s64 wizlib_sys_exepath(char* buf, wizlib_u64 size) {
    wizlib_u64 n = strlen(exe); if (!n || n + 1 > size) return -1; memcpy(buf, exe, n + 1); return (wizlib_s64)n;
}

int main(int argc, char** argv) {
    assert(argc == 5);
    snprintf(root, sizeof(root), "%s", argv[1]);
    wizlib_bundle b; char text[64];

    snprintf(exe, sizeof(exe), "%s", argv[2]);
    assert(wizlib_open(&b) == 0);
    assert(!strcmp(wizlib_id(&b), "demo-app") && !strcmp(wizlib_name(&b), "Demo App") && !strcmp(wizlib_version(&b), "4.2"));
    wizlib_s64 n = wizlib_read(&b, "data/greeting.txt", text, sizeof(text));
    assert(n == 14 && !memcmp(text, "Hello, bundle!", 14));
    assert(wizlib_size(&b, "data/big.bin") == 5000);
    assert(wizlib_read(&b, "data/big.bin", text, sizeof(text)) == WIZ_E_TOO_BIG);   // does not fit: reports, copies nothing
    static unsigned char big[5000];
    assert(wizlib_read(&b, "data/big.bin", big, sizeof(big)) == 5000);
    for (int i = 0; i < 5000; ++i) assert(big[i] == (unsigned char)(i * 7));
    assert(wizlib_read(&b, "data/missing.txt", text, sizeof(text)) == WIZ_E_NOENT);
    assert(wizlib_size(&b, "nothing") == WIZ_E_NOENT);

    // Per-app data directory: created on demand, outside the package.
    char dir[256]; assert(wizlib_data_dir(&b, dir, sizeof(dir)) == 0 && !strcmp(dir, "/home/.local/share/demo-app"));
    assert(wizlib_data_dir(&b, dir, sizeof(dir)) == 0);                              // idempotent
    assert(wizlib_data_load(&b, "score.txt", text, sizeof(text)) == WIZ_E_NOENT);
    assert(wizlib_data_save(&b, "score.txt", "123", 3) == 0);
    assert(wizlib_data_load(&b, "score.txt", text, sizeof(text)) == 3 && !memcmp(text, "123", 3));
    assert(wizlib_data_save(&b, "score.txt", "98765", 5) == 0);                      // replaces
    assert(wizlib_data_load(&b, "score.txt", text, sizeof(text)) == 5);
    assert(wizlib_data_load(&b, "score.txt", text, 5) == 5);                         // exact fit
    assert(wizlib_data_load(&b, "score.txt", text, 4) == WIZ_E_TOO_BIG);
    assert(wizlib_data_save(&b, "../escape.txt", "x", 1) == WIZ_E_UNSAFE);           // cannot leave the data dir
    assert(wizlib_data_save(&b, "/etc/passwd", "x", 1) == WIZ_E_UNSAFE);
    assert(wizlib_data_save(&b, "a/../../b", "x", 1) == WIZ_E_UNSAFE);
    char path[WIZLIB_PATH]; assert(wizlib_data_path(&b, "sub/file.txt", path, sizeof(path)) == 0);
    assert(!strcmp(path, "/home/.local/share/demo-app/sub/file.txt"));
    wizlib_close(&b);

    snprintf(exe, sizeof(exe), "%s", argv[3]);
    assert(wizlib_open(&b) == WIZ_E_NOT_WIZ);                                        // plain ELF
    snprintf(exe, sizeof(exe), "%s", argv[4]);
    assert(wizlib_open(&b) == WIZ_E_ABI);                                            // needs a newer OS
    snprintf(exe, sizeof(exe), "/no/such/file");
    assert(wizlib_open(&b) == WIZ_E_IO);
    exe[0] = 0; assert(wizlib_open(&b) == WIZ_E_IO);                                  // no exepath (e.g. boot module)
    printf("wizlib host test passed\n");
    return 0;
}
