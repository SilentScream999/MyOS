#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#define STORAGE_HOST_TEST
static void* kmalloc(size_t n) { void* p = malloc(n); if (p) memset(p, 0xa5, n); return p; }
static void kfree(void* p) { free(p); }
#include "../src/fs/usb_autogrow.h"
static FILE* image;
static uint64_t tail_start, tail_writes, mbr_writes;
static long budget = -1;
static bool fail_copy;
static bool rd_many(block_device* d, uint64_t lba, uint32_t count, void* p) {
    assert(count && count <= 128 && lba < d->sectors && count <= d->sectors - lba);
    return !fseeko(image, lba * 512, SEEK_SET) && fread(p, 512, count, image) == count;
}
static bool rd(block_device* d, uint64_t lba, void* p) { return rd_many(d, lba, 1, p); }
static bool wr_many(block_device* d, uint64_t lba, uint32_t count, const void* p) {
    assert(count && count <= 128 && lba < d->sectors && count <= d->sectors - lba);
    if (lba >= tail_start) { ++tail_writes; if (budget == 0) return false; if (budget > 0) --budget; }
    if (lba == 0) { ++mbr_writes; if (budget == -3) return false; }
    return !fseeko(image, lba * 512, SEEK_SET) && fwrite(p, 512, count, image) == count;
}
static bool wr(block_device* d, uint64_t lba, const void* p) { return wr_many(d, lba, 1, p); }
static bool flush(block_device*) { return !(budget == -4 && mbr_writes) && !fflush(image) && !fsync(fileno(image)); }
static void progress(const char* text) {
    puts(text); fflush(stdout);
    if (fail_copy && !strcmp(text, "Copying system files...")) budget = 0;
}
int main(int argc, char** argv) {
    assert(argc >= 3);
    image = fopen(argv[1], "r+b"); assert(image);
    struct stat st; assert(!fstat(fileno(image), &st));
    block_device disk{nullptr, (uint64_t)st.st_size / 512, true, rd, wr, flush, rd_many, wr_many};
    if (!strcmp(argv[2], "format")) {
        wy_ext4_volume volume{}; volume.disk = &disk; volume.sectors = disk.sectors; volume.unpublished = true;
        wy_ext4_bind(&volume); ext4_fs fs{}; ext4_mkfs_info info{};
        info.block_size = 4096; info.journal = true; info.label = "WYZARDATA";
        assert(ext4_mkfs(&fs, &volume.bdev, &info, F_SET_EXT4) == EOK);
        assert(wy_ext4_flush(&volume)); fclose(image); return 0;
    }
    uint8_t old_mbr[512], after[512]; assert(rd(&disk, 0, old_mbr));
    wy_ext4_volume seed{}; assert(wy_ext4_probe(&disk, &seed) == 1);
    tail_start = seed.start + seed.sectors;
    if (argc > 3) { budget = atol(argv[3]); fail_copy = budget == -2; }
    int rc = wy_usb_autogrow(&seed, progress);
    assert(rd(&disk, 0, after));
    if (!strcmp(argv[2], "uncertain")) {
        assert(rc == -1 && !disk.online && mbr_writes == 1);
        assert((!memcmp(old_mbr, after, 512)) == (budget == -3));
        fclose(image); return 0;
    }
    if (!strcmp(argv[2], "grow")) {
        assert(rc == 1 && mbr_writes == 1 && tail_writes);
        assert(seed.start == tail_start && seed.sectors == ((disk.sectors - tail_start) & ~7ULL));
        assert(wy_usb_autogrow(&seed) == 0 && mbr_writes == 1); // Never repeat setup.
    } else {
        assert(rc == (!strcmp(argv[2], "skip") ? 0 : -1));
        assert(!memcmp(old_mbr, after, 512) && !mbr_writes);
        if (!strcmp(argv[2], "skip")) assert(!tail_writes);
        if (rc < 0) { fclose(image); return 0; }
    }
    assert(wy_ext4_mount(&seed)); vfs_mount_root(&seed.root.vn);
    file f{}; char bytes[32];
    assert(vfs_open("/etc/root.cfg", 0, &f) == 0);
    assert(vfs_read(&f, bytes, 11) == 11 && !memcmp(bytes, "WYZARROOT1\n", 11)); vfs_close(&f);
    assert(vfs_open("/home/preserved.txt", 0, &f) == 0);
    assert(vfs_read(&f, bytes, sizeof(bytes)) == 16 && !memcmp(bytes, "seed-user-data!!", 16)); vfs_close(&f);
    assert(wy_ext4_unmount(&seed) == 0);
    fclose(image);
    puts("Native first-boot setup passed");
}
