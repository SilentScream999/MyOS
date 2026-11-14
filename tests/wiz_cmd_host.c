// Host harness for user/wiz_cmd.h: runs the real `wiz` shell command against a
// sandbox directory by faking the syscall wrappers init.c provides.
//   wiz_cmd_host ROOT CWD "install foo.wiz" "list" ...
// Each quoted argument is the text typed after "wiz ". Output goes to stdout.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <unistd.h>
#include "src/fs/fileinfo.h"

static char root[512];
static char current_dir[256];
static void print(const char* s) { fputs(s, stdout); }
static void print_dec(uint64_t v) { printf("%llu", (unsigned long long)v); }
static const char* real(const char* path, char* out) { snprintf(out, 1024, "%s%s", root, path); return out; }

static uint64_t sys_open(const char* path, uint64_t flags) {
    char r[1024]; int fd = open(real(path, r), flags == 0 ? O_RDONLY : (O_WRONLY | O_CREAT), 0644);
    return fd < 0 ? (uint64_t)-1 : (uint64_t)fd;
}
static void sys_close(uint64_t fd) { close((int)fd); }
static uint64_t sys_read(uint64_t fd, char* buf, uint64_t len) { ssize_t n = read((int)fd, buf, len); return n < 0 ? (uint64_t)-1 : (uint64_t)n; }
static uint64_t sys_write(uint64_t fd, const char* buf, uint64_t len) { ssize_t n = write((int)fd, buf, len); return n < 0 ? (uint64_t)-1 : (uint64_t)n; }
static int64_t sys_lseek(uint64_t fd, int64_t off, uint64_t whence) { return lseek((int)fd, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END); }
static int64_t sys_unlink(const char* path) { char r[1024]; return unlink(real(path, r)) == 0 ? 0 : -1; }
static int64_t sys_rename(const char* a, const char* b) {      // never overwrites, like the real syscall
    char ra[1024], rb[1024]; real(a, ra); real(b, rb);
    struct stat st; if (stat(rb, &st) == 0) return -1;
    return rename(ra, rb) == 0 ? 0 : -1;
}
static int64_t sys_exchange(const char* a, const char* b) {
    if (getenv("WIZ_TEST_EXCHANGE_FAIL")) return -1;
    char ra[1024], rb[1024]; real(a, ra); real(b, rb);
    return syscall(SYS_renameat2, AT_FDCWD, ra, AT_FDCWD, rb, RENAME_EXCHANGE) == 0 ? 0 : -1;
}
static int64_t sys_replace(const char* path, const void* data, uint64_t len) {
    char r[1024]; FILE* f = fopen(real(path, r), "wb"); if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len; fclose(f); return ok ? 0 : -1;
}
static int64_t sys_mkdir(const char* path, uint64_t mode) { (void)mode; char r[1024]; return mkdir(real(path, r), 0755) == 0 ? 0 : -1; }
static int64_t sys_rmdir(const char* path) { char r[1024]; return rmdir(real(path, r)) == 0 ? 0 : -1; }
static int64_t sys_stat(const char* path, file_info* info) {
    char r[1024]; struct stat st; if (stat(real(path, r), &st) != 0) return -1;
    memset(info, 0, sizeof(*info)); info->size = (uint64_t)st.st_size; info->type = S_ISDIR(st.st_mode) ? 2 : 1; return 0;
}
static int64_t sys_readdir(const char* path, uint64_t index, char* name, uint64_t* size, uint64_t* type) {
    char r[1024]; struct dirent** list; int n = scandir(real(path, r), &list, 0, alphasort);   // includes "." and ".."
    if (n < 0) return -1;
    int64_t result = 0;
    if (index < (uint64_t)n) {
        strcpy(name, list[index]->d_name);
        char full[2048]; snprintf(full, sizeof(full), "%s/%s", r, name); struct stat st;
        *size = 0; *type = 1; if (stat(full, &st) == 0) { *size = (uint64_t)st.st_size; *type = S_ISDIR(st.st_mode) ? 2 : 1; }
        result = 1;
    }
    for (int i = 0; i < n; ++i) free(list[i]);
    free(list); return result;
}

#include "user/wiz_cmd.h"

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s ROOT CWD CMD...\n", argv[0]); return 2; }
    snprintf(root, sizeof(root), "%s", argv[1]); snprintf(current_dir, sizeof(current_dir), "%s", argv[2]);
    for (int i = 3; i < argc; ++i) {
        char line[256]; snprintf(line, sizeof(line), "%s", argv[i]);
        printf("$ wiz %s\n", argv[i]); fflush(stdout);
        wiz_command(line);
    }
    return 0;
}
