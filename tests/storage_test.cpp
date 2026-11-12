// Host-side tests of the real VFS/FAT16 backend using a memory block device.
// clang++ -std=c++20 -fsanitize=address,undefined -Isrc -Isrc/arch -Isrc/core -Isrc/drivers -Isrc/fs -Isrc/gui tests/storage_test.cpp -o /tmp/storage-test
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#define STORAGE_HOST_TEST
#define helpers_h
static void* kmalloc(size_t size) { return calloc(1, size); }
static void kfree(void* p) { free(p); }
using std::strcmp;
using std::strlen;
using std::strcpy;
using std::memset;
using std::memcpy;
#include "../src/fs/vfs.h"
#include "../src/fs/fat16.h"

struct memory_disk {
    std::vector<uint8_t> bytes;
    int writes_until_fail = -1;
    bool flush_ok = true;
    uint64_t sector_reads = 0, batch_reads = 0, batched_sectors = 0;
    bool read_ok = true;
};
static bool read_sector(block_device* d, uint64_t lba, void* out) {
    auto* m = (memory_disk*)d->context;
    ++m->sector_reads;
    if (!m->read_ok) return false;
    assert(lba < d->sectors);
    memcpy(out, m->bytes.data() + lba * 512, 512); return true;
}
static bool read_sectors(block_device* d, uint64_t lba, uint32_t count, void* out) {
    auto* m = (memory_disk*)d->context;
    assert(count && lba < d->sectors && count <= d->sectors - lba);
    ++m->batch_reads; m->batched_sectors += count;
    if (!m->read_ok) return false;
    memcpy(out, m->bytes.data() + lba * 512, (uint64_t)count * 512);
    return true;
}
static bool write_sector(block_device* d, uint64_t lba, const void* in) {
    auto* m = (memory_disk*)d->context;
    if (m->writes_until_fail == 0) return false;
    if (m->writes_until_fail > 0) --m->writes_until_fail;
    memcpy(m->bytes.data() + lba * 512, in, 512); return true;
}
static bool flush_disk(block_device* d) { return ((memory_disk*)d->context)->flush_ok; }
static file_datetime test_time = {2026, 11, 7, 12, 30, 35, 0};
static bool clock_ok = true;
static bool test_clock(file_datetime* t) { *t = test_time; return clock_ok; }
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream in(argv[1], std::ios::binary);
    assert(in.good());
    memory_disk m;
    in.seekg(0, std::ios::end);
    m.bytes.resize(in.tellg());
    in.seekg(0);
    in.read((char*)m.bytes.data(), m.bytes.size());
    assert(in.good());
    block_device d{&m, m.bytes.size() / 512, true, read_sector, write_sector, flush_disk};
    assert(!block_read(&d, d.sectors, nullptr));
    uint8_t probe[1024];
    assert(!block_read_sectors(&d, d.sectors - 1, 2, probe));
    assert(!block_read_sectors(&d, UINT64_MAX, 2, probe));
    assert(!block_read_sectors(&d, 0, 0, probe));
    assert(!block_read_sectors(&d, 0, 1, nullptr));
    assert(block_read_sectors(&d, 0, 2, probe)); // one-sector-only backend fallback
    assert(memcmp(probe, m.bytes.data(), sizeof(probe)) == 0);
    d.read_sectors = read_sectors;
    fat16_volume* v = fat16_probe(&d); assert(v);
    fat16_clock = test_clock;
    vfs_mount_root(&v->root.vn);
    file f{};
    assert(vfs_open("/absent.txt", 0, &f) == -1);
    assert(!fat16_lookup(vfs_root, "absent.txt"));
    assert(vfs_open("/hello.txt", 0x42, &f) == 0);
    std::vector<uint8_t> expected(11003), actual(11003);
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = i * 37;
    assert(vfs_write(&f, expected.data(), expected.size()) == expected.size());
    assert(f.vn->size == expected.size());
    f.offset = 509;
    const char patch[] = "cross-sector overwrite";
    assert(vfs_write(&f, patch, sizeof(patch)) == sizeof(patch));
    memcpy(expected.data() + 509, patch, sizeof(patch));
    f.offset = 0;
    assert(vfs_read(&f, actual.data(), actual.size()) == actual.size());
    assert(actual == expected);
    assert(vfs_read(&f, actual.data(), 1) == 0);
    // Keep both vnodes and handles alive: reading README must not redirect an
    // already-open HELLO handle, or a later lookup of HELLO, to README's cluster.
    file readme;
    assert(vfs_open("/readme.txt", 0, &readme) == 0);
    assert(readme.vn != f.vn);
    uint8_t readme_data[512];
    auto readme_size = vfs_read(&readme, readme_data, sizeof(readme_data));
    assert(readme_size > 0 && readme_size < sizeof(readme_data));
    for (int round = 0; round < 5; ++round) {
        readme.offset = 0;
        uint8_t check[512];
        assert(vfs_read(&readme, check, sizeof(check)) == readme_size);
        assert(memcmp(check, readme_data, readme_size) == 0);
        f.offset = 0;
        assert(vfs_read(&f, actual.data(), actual.size()) == actual.size());
        assert(actual == expected);
    }
    vfs_close(&readme);
    vfs_close(&f);
    // Fresh volume/vnode state simulates reboot and rereads on-disk metadata.
    v = fat16_probe(&d); assert(v); vfs_mount_root(&v->root.vn);
    assert(vfs_open("/HELLO.TXT", 0, &f) == 0);
    assert(vfs_read(&f, actual.data(), actual.size()) == actual.size());
    assert(actual == expected);
    assert(vfs_write(&f, "x", 1) == 0); // read-only descriptor
    assert(vfs_mkdir("/docs") == 0);
    assert(vfs_open("/docs/note.txt", 0x42, &f) == 0);
    assert(vfs_write(&f, "persistent", 10) == 10);
    assert(vfs_open("/toolongname.txt", 0x41, &f) == -1);
    assert(vfs_open("/bad?.txt", 0x41, &f) == -1);
    char oversized[300]; memset(oversized, 'a', 299); oversized[0] = '/'; oversized[299] = 0;
    assert(!vfs_namei(oversized));
    assert(vfs_open(oversized, 0x41, &f) == -1);
    char name[256]; uint64_t size; VnodeType type;
    assert(vfs_readdir("/docs", 0, name, &size, &type) == 1);
    assert(strcmp(name, "NOTE.TXT") == 0 && size == 10 && type == VnodeType::FILE);
    assert(vfs_readdir("/docs", 1, name, &size, &type) == 0);
    // Check lookup, parent resolution and recursive mkdir through a VFS mount.
    assert(vfs_mkdir("/alias") == 0);
    assert(vfs_mount(vfs_namei("/alias"), vfs_root) == 0);
    assert(vfs_namei("/alias/hello.txt") == vfs_namei("/hello.txt"));
    assert(vfs_open("/alias/mounted.txt", 0x41, &f) == 0);
    assert(vfs_namei("/mounted.txt") == f.vn);
    assert(vfs_mkdir_p("/alias/tree/sub") == 0);
    assert(vfs_namei("/tree/sub"));
    assert(vfs_namei("/alias/../hello.txt") == vfs_namei("/hello.txt"));
    assert(vfs_namei("/alias/./hello.txt") == vfs_namei("/hello.txt"));
    assert(vfs_namei("/../../hello.txt") == vfs_namei("/hello.txt"));
    assert(!vfs_namei("/hello.txt/"));
    assert(!vfs_namei("/hello.txt/.."));
    assert(vfs_unlink("/alias") == -1);
    vfs_mount_count = 0;
    // Creation time survives edits and a fresh mount; modified time advances.
    assert(vfs_open("/times.txt", 0x42, &f) == 0);
    file_info info;
    assert(vfs_stat("/times.txt", &info) == 0);
    assert(info.created.year == 2026 && info.created.second == 34 && info.modified.second == 34);
    test_time.hour = 13; test_time.second = 49;
    assert(vfs_write(&f, "dated", 5) == 5);
    assert(vfs_stat("/times.txt", &info) == 0);
    assert(info.created.hour == 12 && info.modified.hour == 13 && info.modified.second == 48);
    assert(vfs_unlink("/times.txt") == -1); // busy file
    vfs_close(&f);
    v = fat16_probe(&d); assert(v); vfs_mount_root(&v->root.vn);
    assert(vfs_stat("/times.txt", &info) == 0 && info.created.hour == 12 && info.modified.hour == 13);
    vnode* old_node = vfs_namei("/times.txt");
    uint16_t old_cluster = ((fat16_node*)old_node->fs_data)->cluster;
    assert(vfs_unlink("/times.txt") == 0 && !vfs_namei("/times.txt"));
    uint16_t entry;
    assert(fat_get(v, old_cluster, &entry) && entry == 0);
    assert(vfs_unlink("/times.txt") == -1);
    assert(vfs_unlink("/docs") == -1 && vfs_unlink("/") == -1);
    // Reused slots get a fresh vnode and timestamp, never old size or content.
    assert(vfs_open("/times.txt", 0x42, &f) == 0 && f.vn != old_node && f.vn->size == 0);
    vfs_close(&f);
    assert(vfs_unlink("/times.txt") == 0); // empty file, no cluster chain
    // Multi-cluster reclamation and read-only protection.
    assert(vfs_open("/delete.txt", 0x42, &f) == 0);
    assert(vfs_write(&f, expected.data(), expected.size()) == expected.size());
    auto* del = (fat16_node*)f.vn->fs_data;
    uint16_t chain[3];
    assert(fat_chain_at(del, 0, false, &chain[0]) && fat_chain_at(del, 1, false, &chain[1]) && fat_chain_at(del, 2, false, &chain[2]));
    vfs_close(&f);
    m.bytes[del->entry_sector * 512 + del->entry_offset + 11] |= 1;
    assert(vfs_unlink("/delete.txt") == -1);
    m.bytes[del->entry_sector * 512 + del->entry_offset + 11] &= ~1;
    assert(vfs_unlink("/delete.txt") == 0);
    for (uint16_t c : chain) assert(fat_get(v, c, &entry) && !entry);
    assert(vfs_open("/cycle.txt", 0x42, &f) == 0);
    assert(vfs_write(&f, "cycle", 5) == 5);
    auto* cycle = (fat16_node*)f.vn->fs_data;
    vfs_close(&f);
    assert(fat_set(v, cycle->cluster, cycle->cluster));
    assert(vfs_unlink("/cycle.txt") == -1 && vfs_namei("/cycle.txt"));
    assert(fat_set(v, cycle->cluster, 0xffff));
    assert(vfs_unlink("/cycle.txt") == 0);
    clock_ok = false;
    assert(vfs_open("/unknown.txt", 0x42, &f) == 0);
    vfs_close(&f);
    assert(vfs_stat("/unknown.txt", &info) == 0 && !info.created.year && !info.modified.year);
    clock_ok = true;
    assert(vfs_unlink("/unknown.txt") == 0);
    uint8_t stamp[32] = {};
    file_datetime decoded;
    test_time = {2107, 12, 31, 23, 59, 59, 0};
    fat_stamp(stamp, true);
    fat_decode_datetime(fat_u16(stamp + 16), fat_u16(stamp + 14), stamp[13], &decoded);
    assert(decoded.year == 2107 && decoded.month == 12 && decoded.day == 31 && decoded.second == 58);
    test_time = {1980, 1, 1, 0, 0, 0, 0};
    fat_stamp(stamp, true);
    fat_decode_datetime(fat_u16(stamp + 16), fat_u16(stamp + 14), stamp[13], &decoded);
    assert(decoded.year == 1980 && decoded.month == 1 && decoded.day == 1);
    test_time = {2026, 11, 7, 13, 30, 0, 0};
    // A large read is batched, preserves unaligned head/tail bytes, and does
    // not reread FAT metadata once for each link in the same FAT sector.
    assert(vfs_open("/large.raw", 0x42, &f) == 0);
    std::vector<uint8_t> large(1024 * 1024 + 13), result(large.size());
    for (size_t i = 0; i < large.size(); ++i) large[i] = (i * 13) ^ (i >> 8);
    assert(vfs_write(&f, large.data(), large.size()) == large.size());
    f.offset = 0; m.batch_reads = m.batched_sectors = m.sector_reads = 0;
    assert(vfs_read(&f, result.data(), result.size()) == result.size());
    assert(result == large);
    assert(m.batched_sectors == 2048 && m.batch_reads <= 20);
    assert(m.sector_reads < 10); // partial tail plus the two FAT sectors
    f.offset = 509;
    assert(vfs_read(&f, result.data(), 12003) == 12003);
    assert(memcmp(result.data(), large.data() + 509, 12003) == 0);
    f.offset = 0; m.read_ok = false;
    assert(vfs_read(&f, result.data(), 8192) == 0);
    m.read_ok = true;
    // Force fragmentation by putting another file's allocation between links.
    file fragmented{}, spacer{};
    assert(vfs_open("/frag.raw", 0x42, &fragmented) == 0);
    assert(vfs_write(&fragmented, large.data(), 4096) == 4096);
    assert(vfs_open("/spacer.raw", 0x42, &spacer) == 0);
    assert(vfs_write(&spacer, large.data(), 4096) == 4096);
    assert(vfs_write(&fragmented, large.data() + 4096, 4096) == 4096);
    fragmented.offset = 0;
    m.batch_reads = 0;
    assert(vfs_read(&fragmented, result.data(), 8192) == 8192);
    assert(memcmp(result.data(), large.data(), 8192) == 0);
    assert(m.batch_reads == 2); // never merge across the spacer allocation
    vfs_close(&fragmented); vfs_close(&spacer); vfs_close(&f);
    // Invalid BPB and out-of-device partitions fail to mount.
    uint8_t saved = m.bytes[v->start * 512 + 13];
    m.bytes[v->start * 512 + 13] = 3; assert(!fat16_probe(&d));
    m.bytes[v->start * 512 + 13] = saved;
    auto count = d.sectors; d.sectors = v->start; assert(!fat16_probe(&d)); d.sectors = count;
    // A full FAT returns a short/zero write rather than exposing bogus clusters.
    auto original = m.bytes;
    for (uint32_t c = 2; c < v->clusters + 2 && c < 0xfff0; ++c) assert(fat_set(v, c, 0xffff));
    assert(vfs_open("/full.txt", 0x41, &f) == 0);
    assert(vfs_write(&f, "x", 1) == 0);
    m.bytes = original;
    v = fat16_probe(&d); assert(v); vfs_mount_root(&v->root.vn);
    assert(vfs_open("/hello.txt", 2, &f) == 0);
    m.writes_until_fail = 0;
    assert(vfs_write(&f, "x", 1) == 0 && v->failed);
    assert(!fat16_create(vfs_root, "later.txt"));
    m.writes_until_fail = -1;
    v = fat16_probe(&d); assert(v); vfs_mount_root(&v->root.vn);
    assert(vfs_open("/hello.txt", 2, &f) == 0);
    m.flush_ok = false;
    assert(vfs_write(&f, "x", 1) == 0 && v->failed);
    d.online = false;
    assert(!fat16_probe(&d));

    // Test vfs_resolve_path
    char res[256];
    assert(vfs_resolve_path("/home", "docs", res, sizeof(res)) && strcmp(res, "/home/docs") == 0);
    assert(vfs_resolve_path("/home/user", "..", res, sizeof(res)) && strcmp(res, "/home") == 0);
    assert(vfs_resolve_path("/", "..", res, sizeof(res)) && strcmp(res, "/") == 0);
    assert(vfs_resolve_path("/home", "/bin", res, sizeof(res)) && strcmp(res, "/bin") == 0);
    assert(vfs_resolve_path("/home", ".", res, sizeof(res)) && strcmp(res, "/home") == 0);
    assert(vfs_resolve_path("/home", "a/./b/../c", res, sizeof(res)) && strcmp(res, "/home/a/c") == 0);

    // Online volume again for rmdir and statfs tests
    d.online = true;
    m.writes_until_fail = -1;
    m.flush_ok = true;
    v = fat16_probe(&d); assert(v); vfs_mount_root(&v->root.vn);

    // Test vfs_statfs
    fs_info st;
    assert(vfs_statfs("/", &st) == 0);
    assert(strcmp(st.fs_type, "FAT16") == 0);
    assert(st.total_bytes > 0 && st.free_bytes > 0 && st.free_bytes <= st.total_bytes);

    // Test rmdir: empty vs non-empty
    assert(vfs_mkdir("/newdir") == 0);
    assert(vfs_open("/newdir/file.txt", 0x41, &f) == 0);
    assert(vfs_write(&f, "content", 7) == 7);
    vfs_close(&f);
    assert(vfs_rmdir("/newdir") == -1); // not empty
    assert(vfs_unlink("/newdir/file.txt") == 0);
    assert(vfs_rmdir("/newdir") == 0); // now empty -> success!
    assert(!vfs_namei("/newdir"));

    puts("storage tests passed: remount, multi-cluster I/O, directories, validation, full disk, I/O and flush failure, rmdir, statfs, path resolution");
}
