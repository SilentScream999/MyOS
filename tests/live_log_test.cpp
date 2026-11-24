#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#define helpers_h
#define heap_h
static void* kmalloc(size_t n) { return calloc(1, n); }
static void kfree(void* p) { free(p); }
static void print(char*) {}
#include "../src/core/klog.h"
char g_klog_buffer[KLOG_BUFFER_SIZE];
uint64_t g_klog_pos;
#include "../src/fs/ramfs.h"

static uint64_t listed_size(const char* dir, const char* wanted) {
    char name[256]; uint64_t size; VnodeType type;
    for (uint64_t i = 0; vfs_readdir(dir, i, name, &size, &type) == 1; ++i)
        if (strcmp(name, wanted) == 0) return size;
    assert(false); return 0;
}
int main() {
    ramfs_init(); ramfs_mount_klog();
    vnode* log = vfs_namei("/bootlog.txt");
    memcpy(g_klog_buffer, "first log", 9); g_klog_pos = 9;
    file_info info;
    assert(vfs_stat("/bootlog.txt", &info) == 0 && info.size == 9 && info.type == 1);
    assert(listed_size("/", "bootlog.txt") == 9);
    // Stand in for a disk's zero-byte placeholder, mounted over with the live log.
    file f{};
    assert(vfs_open("/alias.txt", 0x41, &f) == 0);
    vnode* placeholder = f.vn;
    vfs_close(&f);
    assert(placeholder->size == 0);
    assert(vfs_mount(placeholder, log) == 0);
    assert(vfs_stat("/alias.txt", &info) == 0 && info.size == 9);
    assert(listed_size("/", "alias.txt") == 9);
    memcpy(g_klog_buffer + 9, " grew", 5); g_klog_pos += 5;
    assert(vfs_stat("/alias.txt", &info) == 0 && info.size == 14);
    assert(listed_size("/", "alias.txt") == 14);
    assert(listed_size("/", "bootlog.txt") == 14);
    assert(vfs_open("/alias.txt", 0, &f) == 0);
    char text[32];
    assert(vfs_read(&f, text, sizeof(text)) == 14);
    assert(memcmp(text, "first log grew", 14) == 0);
    vfs_close(&f);
    assert(vfs_unlink("/alias.txt") == -1);
    for(unsigned i=0;i<KLOG_BUFFER_SIZE+16;++i)klog_putc('x');
    assert(g_klog_pos==KLOG_BUFFER_SIZE-1&&g_klog_buffer[g_klog_pos]==0);
    assert(vfs_stat("/bootlog.txt",&info)==0&&info.size==KLOG_BUFFER_SIZE-1);
    assert(vfs_open("/bootlog.txt",0,&f)==0);
    f.offset=KLOG_BUFFER_SIZE-17;
    assert(vfs_read(&f,text,16)==16);
    for(unsigned i=0;i<16;++i)assert(text[i]=='x');
    assert(vfs_read(&f,text,1)==0);
    vfs_close(&f);
}
