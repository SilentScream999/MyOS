#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#define helpers_h
#define heap_h
#define STORAGE_HOST_TEST
static void* kmalloc(size_t n) { void* p = malloc(n); if (p) memset(p, 0xa5, n); return p; }
static void kfree(void* p) { free(p); }
static void print(char*) {}
char g_klog_buffer[65536];
uint64_t g_klog_pos;
#include "../src/fs/ramfs.h"
int main() {
    ramfs_init();
    file f{}, reader{};
    assert(vfs_open("/save.txt", 0x242, &f) == 0);
    assert(vfs_write(&f, "long-value", 10) == 10);
    assert(vfs_open("/save.txt", 0, &reader) == 0);
    assert(vfs_ftruncate(&f, 2) == 0 && reader.vn->size == 2);
    assert(vfs_ftruncate(&f, 2000) == 0);
    char data[2000]; memset(data, 0xff, sizeof(data));
    assert(vfs_read(&reader, data, sizeof(data)) == sizeof(data));
    assert(!memcmp(data, "lo", 2));
    for (unsigned i = 2; i < sizeof(data); ++i) assert(!data[i]);
    assert(vfs_ftruncate(&reader, 0) == -1);
    assert(vfs_fsync(&f) == 0);
    vfs_close(&f); vfs_close(&reader);
    assert(vfs_open("/save.txt", 0x241, &f) == 0);
    assert(vfs_write(&f, "ok", 2) == 2); vfs_close(&f);
    assert(vfs_open("/save.txt", 0x200, &f) == -1);
    assert(vfs_open("/save.txt", 0, &f) == 0);
    assert(vfs_read(&f, data, sizeof(data)) == 2 && !memcmp(data, "ok", 2));
    vfs_close(&f);
    assert(vfs_replace("/save.txt", "replacement", 11) == 0);
    assert(vfs_open("/save.txt", 0, &f) == 0);
    assert(vfs_replace("/save.txt", "blocked", 7) == -1);
    assert(vfs_read(&f, data, sizeof(data)) == 11 && !memcmp(data, "replacement", 11));
    vfs_close(&f);
    assert(vfs_rename("/save.txt", "/renamed.txt") == 0);
    assert(!vfs_namei("/save.txt") && vfs_namei("/renamed.txt"));
    assert(vfs_replace("/save.txt", nullptr, 0) == 0);
    assert(vfs_rename("/renamed.txt", "/save.txt") == -1);
    vnode* cached = vfs_namei("/renamed.txt");
    assert(vfs_exchange("/renamed.txt", "/missing") == -1);
    assert(vfs_open("/renamed.txt", 0, &f) == 0);
    assert(vfs_exchange("/renamed.txt", "/save.txt") == -1);
    vfs_close(&f);
    assert(vfs_exchange("/renamed.txt", "/save.txt") == 0);
    assert(vfs_namei("/renamed.txt") == cached && cached->size == 0);
    assert(vfs_open("/save.txt", 0, &f) == 0);
    assert(vfs_read(&f, data, sizeof(data)) == 11 && !memcmp(data, "replacement", 11));
    vfs_close(&f);
    assert(vfs_mkdir("/folder") == 0);
    assert(vfs_replace("/folder/file", "other", 5) == 0);
    assert(vfs_exchange("/save.txt", "/folder/file") == -1);
    assert(vfs_exchange("/save.txt", "/folder") == -1);
    ramfs_mount_klog();
    assert(vfs_open("/bootlog.txt", 0x201, &f) == -1);
    assert(vfs_replace("/bootlog.txt", "bad", 3) == -1);
    assert(vfs_exchange("/bootlog.txt", "/save.txt") == -1);
}
