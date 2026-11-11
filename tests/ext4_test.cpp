// The real ext4 VFS adapter over a bounded, fault-injectable memory disk.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>
#define STORAGE_HOST_TEST
static void* kmalloc(size_t n) { return calloc(1, n); }
static void kfree(void* p) { free(p); }
#include "../src/fs/ext4_vfs.h"
struct disk_image {
    std::vector<uint8_t> bytes;
    int writes = -1;
    bool flush_ok = true;
    uint64_t sectors_written = 0, write_commands = 0, flush_commands = 0;
    uint32_t largest_write = 0;
};
static bool read_sector(block_device* d, uint64_t lba, void* p) {
    assert(lba < d->sectors); auto* m = (disk_image*)d->context;
    memcpy(p, m->bytes.data() + lba * 512, 512); return true;
}
static bool write_sector(block_device* d, uint64_t lba, const void* p) {
    assert(lba < d->sectors); auto* m = (disk_image*)d->context;
    if (!m->writes) return false;
    if (m->writes > 0) --m->writes;
    ++m->sectors_written;
    memcpy(m->bytes.data() + lba * 512, p, 512); return true;
}
static bool write_sectors(block_device* d, uint64_t lba, uint32_t count, const void* p) {
    auto* m = (disk_image*)d->context;
    assert(count && count <= 128 && lba < d->sectors && count <= d->sectors - lba);
    ++m->write_commands;
    if (count > m->largest_write) m->largest_write = count;
    for (uint32_t i = 0; i < count; ++i)
        if (!write_sector(d, lba + i, (const uint8_t*)p + i * 512)) return false;
    return true;
}
static bool flush(block_device* d) {
    auto* m = (disk_image*)d->context; ++m->flush_commands; return m->flush_ok;
}
int main(int argc, char** argv) {
    assert(argc >= 3);
    disk_image m; std::ifstream in(argv[1], std::ios::binary | std::ios::ate); assert(in.good());
    m.bytes.resize(in.tellg()); in.seekg(0); in.read((char*)m.bytes.data(), m.bytes.size()); assert(in.good()); in.close();
    block_device disk{&m, m.bytes.size()/512, true, read_sector, write_sector, flush, nullptr, write_sectors};
    wy_ext4_volume v{};
    auto before = m.bytes;
    assert(wy_ext4_probe(&disk, &v) == 1);
    assert(m.bytes == before); // Discovery must never replay or write.
    before.clear(); before.shrink_to_fit();
    // Unknown incompatible features must be rejected before the first write.
    size_t feature_byte = v.start * 512 + 1024 + 99;
    m.bytes[feature_byte] |= 0x80;
    assert(!wy_ext4_mount(&v));
    assert(!v.mounted && !v.failed);
    m.bytes[feature_byte] &= 0x7f;
    assert(wy_ext4_mount(&v)); vfs_mount_root(&v.root.vn);
    uint64_t flushes = m.flush_commands;
    assert(wy_ext4_flush(&v) && wy_ext4_flush(&v));
    assert(m.flush_commands == flushes); // No writes since the last durable barrier.
    file f{}; char out[32]{};
    assert(vfs_open("/etc/root.cfg", 0, &f) == 0);
    assert(vfs_read(&f, out, 11) == 11 && !memcmp(out, "WYZARROOT1\n", 11)); vfs_close(&f);
    if (!strcmp(argv[2], "atomic-fault")) {
        assert(argc == 4);
        assert(vfs_replace("/home/atomic.txt", "old-complete", 12) == 0);
        m.writes = atoi(argv[3]);
        std::string updated(8193, 'N');
        (void)vfs_replace("/home/atomic.txt", updated.data(), updated.size());
        // Do not unmount: model loss immediately after the write failure.
        std::ofstream saved(argv[1], std::ios::binary | std::ios::trunc);
        saved.write((const char*)m.bytes.data(), m.bytes.size()); assert(saved.good());
        return 0;
    }
    if (!strcmp(argv[2], "recover")) {
        assert(vfs_open("/home/persistent-document.txt", 0, &f) == 0);
        assert(vfs_read(&f, out, 9) == 9 && !memcmp(out, "persisted", 9)); vfs_close(&f);
    } else {
        assert(vfs_open("/home/persistent-document.txt", 0x42, &f) == 0);
        assert(vfs_write(&f, "persisted", 9) == 9); vfs_close(&f);
        if (!strcmp(argv[2], "normal")) {
            assert(vfs_replace("/home/atomic.txt", "old-complete", 12) == 0);
            assert(vfs_replace("/home/atomic.txt", "new", 3) == 0);
            assert(vfs_open("/home/atomic.txt", 0, &f) == 0);
            assert(vfs_read(&f, out, sizeof(out)) == 3 && !memcmp(out, "new", 3));
            assert(vfs_replace("/home/atomic.txt", "bad", 3) == -1); // Open destination protected.
            assert(vfs_rename("/home/atomic.txt", "/home/moved.txt") == -1);
            vfs_close(&f);
            vnode* cached = vfs_namei("/home/atomic.txt");
            assert(vfs_rename("/home/atomic.txt", "/home/moved.txt") == 0);
            assert(!vfs_namei("/home/atomic.txt") && vfs_namei("/home/moved.txt") == cached);
            assert(vfs_replace("/home/atomic.txt", "other", 5) == 0);
            assert(vfs_rename("/home/moved.txt", "/home/atomic.txt") == -1);
            assert(vfs_replace("/home/atomic.txt", nullptr, 0) == 0);
            assert(vfs_namei("/home/atomic.txt")->size == 0);
            assert(vfs_mkdir("/home/rename-dir") == 0);
            assert(vfs_replace("/home/rename-dir/child.txt", "child", 5) == 0);
            vnode* child = vfs_namei("/home/rename-dir/child.txt");
            assert(vfs_rename("/home/rename-dir", "/home/renamed-dir") == 0);
            assert(vfs_namei("/home/renamed-dir/child.txt") == child);
            assert(vfs_rename("/home/renamed-dir", "/home/renamed-dir/cycle") == -1);
            assert(vfs_mkdir("/home/A directory with spaces") == 0);
            assert(vfs_mkdir("/home/A directory with spaces") == -1);
            const char* path = "/home/A directory with spaces/caf\xc3\xa9-photo-document.png";
            assert(vfs_open(path, 0x42, &f) == 0);
            std::vector<uint8_t> expected(131077), actual(expected.size());
            for (size_t i = 0; i < expected.size(); ++i) expected[i] = i * 37;
            assert(vfs_write(&f, expected.data(), expected.size()) == expected.size());
            assert(m.largest_write > 1 && m.sectors_written > m.write_commands * 2);
            f.offset = 4093; assert(vfs_write(&f, "BOUNDARY", 8) == 8);
            memcpy(expected.data() + 4093, "BOUNDARY", 8);
            f.offset = 0; assert(vfs_read(&f, actual.data(), actual.size()) == actual.size()); assert(actual == expected);
            assert(vfs_fsync(&f) == 0);
            uint64_t old_offset = f.offset;
            assert(vfs_ftruncate(&f, 5) == 0 && f.vn->size == 5 && f.offset == old_offset);
            assert(vfs_ftruncate(&f, 5000) == 0 && f.vn->size == 5000);
            std::vector<uint8_t> expanded(5000, 0xff);
            f.offset = 0; assert(vfs_read(&f, expanded.data(), expanded.size()) == expanded.size());
            assert(!memcmp(expanded.data(), expected.data(), 5));
            for (size_t i = 5; i < expanded.size(); ++i) assert(expanded[i] == 0);
            assert(vfs_unlink(path) == -1); vfs_close(&f);
            assert(vfs_rmdir("/home/A directory with spaces") == -1);
            vnode* stale = vfs_namei(path); assert(vfs_unlink(path) == 0);
            assert(wy_ext4_read(stale, out, 1, 0) == 0);
            assert(vfs_rmdir("/home/A directory with spaces") == 0);
            assert(vfs_open("/home/short.cfg", 0x242, &f) == 0);
            assert(vfs_write(&f, "long-value", 10) == 10); vfs_close(&f);
            assert(vfs_open("/home/short.cfg", 0x241, &f) == 0);
            assert(vfs_write(&f, "ok", 2) == 2); vfs_close(&f);
            assert(vfs_open("/home/short.cfg", 0x200, &f) == -1); // Read-only truncate forbidden.
            assert(vfs_open("/home/short.cfg", 0, &f) == 0);
            assert(vfs_ftruncate(&f, 0) == -1 && f.vn->size == 2);
            memset(out, 0, sizeof(out)); assert(vfs_read(&f, out, sizeof(out)) == 2 && !memcmp(out, "ok", 2));
            vfs_close(&f);
            assert(vfs_mkdir("/home/growing-directory") == 0);
            for (unsigned i = 0; i < 160; ++i) {
                char name[100]; snprintf(name, sizeof(name), "/home/growing-directory/long-file-name-%03u.txt", i);
                assert(vfs_open(name, 0x42, &f) == 0); vfs_close(&f);
            }
            char name[256]; uint64_t index = 0;
            std::vector<std::string> listing;
            uint64_t steps = wy_ext4_directory_steps;
            while (vfs_readdir("/home/growing-directory", index, name, nullptr, nullptr) == 1) {
                listing.emplace_back(name); ++index;
            }
            assert(index == 160);
            assert(wy_ext4_directory_steps - steps <= 162); // Linear sequential enumeration.
            assert(vfs_readdir("/home/growing-directory", 0, name, nullptr, nullptr) == 1);
            assert(name == listing[0]);
            assert(vfs_readdir("/home/growing-directory", 159, name, nullptr, nullptr) == 1);
            assert(name == listing[159]);
            assert(vfs_readdir("/home/growing-directory", 1, name, nullptr, nullptr) == 1);
            assert(name == listing[1]);
            assert(vfs_unlink((std::string("/home/growing-directory/") + listing[0]).c_str()) == 0);
            assert(vfs_readdir("/home/growing-directory", 2, name, nullptr, nullptr) == 1);
            assert(name == listing[3]); // Mutation invalidates offset.
            assert(vfs_open("/home/growing-directory/new-file.txt", 0x42, &f) == 0); vfs_close(&f);
            index = 0;
            while (vfs_readdir("/home/growing-directory", index, name, nullptr, nullptr) == 1) ++index;
            assert(index == 160);
            fs_info info{}; assert(vfs_statfs("/", &info) == 0);
            assert(!strcmp(info.fs_type, "ext4") && info.free_bytes && info.free_bytes < info.total_bytes);
        } else if (!strcmp(argv[2], "write-fail") || !strcmp(argv[2], "flush-fail") || !strcmp(argv[2], "offline")) {
            assert(vfs_open("/home/persistent-document.txt", 2, &f) == 0);
            if (!strcmp(argv[2], "write-fail")) m.writes = 0;
            if (!strcmp(argv[2], "flush-fail")) m.flush_ok = false;
            if (!strcmp(argv[2], "offline")) disk.online = false;
            assert(vfs_write(&f, "failure", 7) == 0);
            assert(vfs_read(&f, out, 1) == 0);
            assert(!vfs_namei("/home/persistent-document.txt")); vfs_close(&f);
            puts("ext4 I/O failure containment passed"); return 0;
        }
    }
    if (strcmp(argv[2], "crash")) assert(wy_ext4_unmount(&v) == 0);
    std::ofstream saved(argv[1], std::ios::binary | std::ios::trunc);
    saved.write((const char*)m.bytes.data(), m.bytes.size()); assert(saved.good());
    puts("ext4 VFS test passed");
}
