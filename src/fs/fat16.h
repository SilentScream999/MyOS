#ifndef WYZAR_FAT16_H
#define WYZAR_FAT16_H
#include "block.h"
#ifndef STORAGE_HOST_TEST
#include "vfs.h"
#include "heap.h"
#include "rtc.h"
#endif

// FAT16 with short (8.3) names. Deliberately mounts only WYZARDATA-labelled
// MBR partitions: never guess which attached drive should receive OS writes.
struct fat16_volume;
struct fat16_node {
    vnode vn;
    fat16_volume* volume;
    fat16_node* next;
    uint32_t entry_sector;
    uint16_t entry_offset, cluster;
    bool root, deleted, long_name;
    uint32_t open_count;
    uint32_t chain_index, chain_generation;
    uint16_t chain_cluster;
};
struct fat16_volume {
    block_device* disk;
    uint32_t start, sectors, fat_start, root_start, data_start, clusters;
    uint16_t fat_sectors, root_entries;
    uint8_t spc, fats;
    bool failed;
    uint32_t chain_generation;
    uint32_t fat_cache_lba;
    bool fat_cache_valid;
    uint8_t fat_cache[512];
    fat16_node* nodes;
    fat16_node root;
};
static uint16_t fat_u16(const uint8_t* p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t fat_u32(const uint8_t* p) { return fat_u16(p) | (uint32_t)fat_u16(p + 2) << 16; }
static void fat_p16(uint8_t* p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
static void fat_p32(uint8_t* p, uint32_t n) { fat_p16(p, n); fat_p16(p + 2, n >> 16); }
// Injectable clock keeps the on-disk timestamp logic testable without hardware.
static bool (*fat16_clock)(file_datetime*) = nullptr;
static bool fat_now(file_datetime* out) {
    if (fat16_clock) return fat16_clock(out);
#ifndef STORAGE_HOST_TEST
    RtcTime t;
    if (!rtc_read_time(&t)) return false;
    out->year = t.year; out->month = t.month; out->day = t.day;
    out->hour = t.hour; out->minute = t.minute; out->second = t.second; out->reserved = 0;
    return true;
#else
    return false;
#endif
}
static void fat_stamp(uint8_t* entry, bool creation) {
    file_datetime t;
    if (!fat_now(&t) || t.year < 1980 || t.year > 2107 || t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59 || t.second > 59) return;
    uint16_t date = ((t.year - 1980) << 9) | (t.month << 5) | t.day;
    uint16_t time = (t.hour << 11) | (t.minute << 5) | (t.second / 2);
    if (creation) {
        // Use the same two-second precision as modification time so creating
        // and immediately writing a file cannot appear to go back one second.
        entry[13] = 0;
        fat_p16(entry + 14, time); fat_p16(entry + 16, date);
    }
    fat_p16(entry + 18, date);
    fat_p16(entry + 22, time); fat_p16(entry + 24, date);
}
static void fat_decode_datetime(uint16_t date, uint16_t time, uint8_t fraction, file_datetime* out) {
    memset(out, 0, sizeof(*out));
    if (!(date & 31) || !((date >> 5) & 15) || ((date >> 5) & 15) > 12 ||
        (time >> 11) > 23 || ((time >> 5) & 63) > 59 || (time & 31) > 29 || fraction > 199) return;
    out->year = 1980 + (date >> 9); out->month = (date >> 5) & 15; out->day = date & 31;
    out->hour = time >> 11; out->minute = (time >> 5) & 63;
    out->second = (time & 31) * 2 + fraction / 100;
}
static bool fat_read_sector(fat16_volume* v, uint32_t lba, void* buf) {
    return !v->failed && lba >= v->start && (uint64_t)lba - v->start < v->sectors && block_read(v->disk, lba, buf);
}
static bool fat_read_sectors(fat16_volume* v, uint32_t lba, uint32_t count, void* buf) {
    return !v->failed && lba >= v->start && (uint64_t)lba - v->start < v->sectors &&
           count <= v->sectors - (lba - v->start) && block_read_sectors(v->disk, lba, count, buf);
}
static bool fat_write_sector(fat16_volume* v, uint32_t lba, const void* buf) {
    if (v->failed || lba < v->start || (uint64_t)lba - v->start >= v->sectors) return false;
    if (block_write(v->disk, lba, buf)) return true;
    v->failed = true;
    return false;
}
static bool fat_flush(fat16_volume* v) {
    if (v->failed || !v->disk->online || !v->disk->flush(v->disk)) {
        v->failed = true;
        return false;
    }
    return true;
}
static bool fat_cluster_valid(fat16_volume* v, uint32_t c) {
    return c >= 2 && c < v->clusters + 2 && c < 0xfff0;
}
static uint32_t fat_cluster_sector(fat16_volume* v, uint16_t c) {
    return v->data_start + (uint32_t)(c - 2) * v->spc;
}
static bool fat_get(fat16_volume* v, uint16_t c, uint16_t* next) {
    if (!fat_cluster_valid(v, c) || v->failed || !v->disk->online) return false;
    uint32_t lba = v->fat_start + c / 256;
    if (!v->fat_cache_valid || v->fat_cache_lba != lba) {
        v->fat_cache_valid = false;
        if (!fat_read_sector(v, lba, v->fat_cache)) return false;
        v->fat_cache_lba = lba;
        v->fat_cache_valid = true;
    }
    *next = fat_u16(v->fat_cache + (c % 256) * 2);
    return true;
}
static bool fat_set(fat16_volume* v, uint16_t c, uint16_t next) {
    uint8_t sector[512];
    if (!fat_cluster_valid(v, c)) return false;
    v->fat_cache_valid = false;
    for (uint8_t i = 0; i < v->fats; ++i) {
        uint32_t lba = v->fat_start + i * v->fat_sectors + c / 256;
        if (!fat_read_sector(v, lba, sector)) return false;
        fat_p16(sector + (c % 256) * 2, next);
        if (!fat_write_sector(v, lba, sector)) return false;
    }
    ++v->chain_generation;
    return true;
}
static uint16_t fat_allocate(fat16_volume* v) {
    uint8_t sector[512];
    for (uint32_t c = 2; c < v->clusters + 2 && c < 0xfff0; ++c) {
        if (c == 2 || c % 256 == 0)
            if (!fat_read_sector(v, v->fat_start + c / 256, sector)) return 0;
        if (fat_u16(sector + (c % 256) * 2)) continue;
        // Zero before linking: no stale disk contents become visible.
        uint8_t zero[512]; memset(zero, 0, sizeof(zero));
        for (uint8_t s = 0; s < v->spc; ++s)
            if (!fat_write_sector(v, fat_cluster_sector(v, c) + s, zero)) return 0;
        if (!fat_set(v, c, 0xffff)) return 0;
        return (uint16_t)c;
    }
    return 0;
}
static bool fat_chain_at(fat16_node* n, uint32_t index, bool grow, uint16_t* result) {
    fat16_volume* v = n->volume;
    if (index >= v->clusters) return false;
    if (!n->cluster && grow) n->cluster = fat_allocate(v);
    uint16_t c = n->cluster;
    if (!fat_cluster_valid(v, c)) return false;
    uint32_t start = 0;
    // Sequential reads must not restart at the first cluster for every sector.
    // A FAT mutation invalidates all hints, including hints on other vnodes.
    if (n->chain_cluster && n->chain_generation == v->chain_generation && n->chain_index <= index) {
        c = n->chain_cluster;
        start = n->chain_index;
    }
    for (uint32_t i = start; i < index; ++i) {
        uint16_t next;
        if (!fat_get(v, c, &next)) return false;
        if (next >= 0xfff8 && grow) {
            next = fat_allocate(v);
            if (!next || !fat_set(v, c, next)) return false;
        }
        if (!fat_cluster_valid(v, next)) return false;
        c = next;
    }
    n->chain_cluster = c;
    n->chain_index = index;
    n->chain_generation = v->chain_generation;
    *result = c;
    return true;
}
// Directory slot address. Traversals are bounded even for corrupt cyclic chains.
static bool fat_slot(fat16_node* dir, uint32_t index, uint32_t* lba, uint16_t* off) {
    fat16_volume* v = dir->volume;
    *off = (index % 16) * 32;
    if (dir->root) {
        if (index >= v->root_entries) return false;
        *lba = v->root_start + index / 16;
        return true;
    }
    uint16_t c;
    if (!fat_chain_at(dir, index / (16 * v->spc), false, &c)) return false;
    *lba = fat_cluster_sector(v, c) + (index / 16) % v->spc;
    return true;
}
static char fat_upper(char c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static bool fat_short_name(const char* name, uint8_t out[11]) {
    for (int i = 0; i < 11; ++i) out[i] = ' ';
    int pos = 0, count = 0;
    bool ext = false;
    for (; *name; ++name) {
        char c = fat_upper(*name);
        if (c == '.') {
            if (ext || pos == 0) return false;
            ext = true; pos = 8; count = 0; continue;
        }
        bool legal = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!legal || count >= (ext ? 3 : 8)) return false;
        out[pos++] = c; ++count;
    }
    return count > 0;
}
static void fat_display_name(const uint8_t* e, char* out) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; ++i) out[n++] = e[i];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; ++i) out[n++] = e[i];
    }
    out[n] = 0;
}
static bool fat_regular_entry(const uint8_t* e) {
    return e[0] && e[0] != 0xe5 && e[0] != '.' && !(e[11] & 0x08) && e[11] != 0x0f;
}
static uint64_t fat16_read(vnode*, void*, uint64_t, uint64_t);
static uint64_t fat16_write(vnode*, const void*, uint64_t, uint64_t);
static vnode* fat16_lookup(vnode*, const char*);
static vnode* fat16_create(vnode*, const char*);
static vnode* fat16_mkdir(vnode*, const char*);
static int fat16_readdir(vnode*, uint64_t, char*, uint64_t*, VnodeType*);
static int fat16_unlink(vnode*, const char*);
static int fat16_stat(vnode*, file_info*);
static int fat16_rmdir(vnode*, const char*);
static int fat16_statfs(vnode*, fs_info*);
static int fat16_open(vnode* vn, file*) {
    storage_guard guard;
    fat16_node* n = (fat16_node*)vn->fs_data;
    if (n->deleted || n->volume->failed || !n->volume->disk->online) return -1;
    ++n->open_count; return 0;
}
static void fat16_close(vnode* vn) {
    storage_guard guard;
    fat16_node* n = (fat16_node*)vn->fs_data;
    if (n->open_count) --n->open_count;
}
static vfs_ops fat16_ops = {
    .read = fat16_read, .write = fat16_write, .open = fat16_open, .close = fat16_close,
    .lookup = fat16_lookup, .create = fat16_create, .mkdir = fat16_mkdir, .readdir = fat16_readdir,
    .unlink = fat16_unlink, .stat = fat16_stat, .rmdir = fat16_rmdir, .statfs = fat16_statfs,
};
static fat16_node* fat_node(fat16_volume* v, uint32_t lba, uint16_t off, const uint8_t* e, bool long_name = false) {
    for (fat16_node* n = v->nodes; n; n = n->next)
        if (!n->deleted && n->entry_sector == lba && n->entry_offset == off) return n;
    fat16_node* n = (fat16_node*)kmalloc(sizeof(fat16_node));
    if (!n) return nullptr;
    n->volume = v; n->root = false; n->entry_sector = lba; n->entry_offset = off;
    n->cluster = fat_u16(e + 26);
    n->deleted = false; n->long_name = long_name; n->open_count = 0;
    n->chain_cluster = 0; n->chain_index = n->chain_generation = 0;
    n->vn.type = (e[11] & 0x10) ? VnodeType::DIRECTORY : VnodeType::FILE;
    n->vn.ops = &fat16_ops; n->vn.fs_data = n; n->vn.size = fat_u32(e + 28);
    n->next = v->nodes; v->nodes = n;
    return n;
}
static vnode* fat16_lookup(vnode* vn, const char* name) {
    storage_guard guard;
    if (vn->type != VnodeType::DIRECTORY) return nullptr;
    fat16_node* dir = (fat16_node*)vn->fs_data;
    uint8_t shortname[11], buf[512];
    if (!fat_short_name(name, shortname)) return nullptr;
    uint32_t cached = 0xffffffff;
    bool preceding_lfn = false;
    for (uint32_t i = 0; i < dir->volume->clusters * dir->volume->spc * 16; ++i) {
        uint32_t lba; uint16_t off;
        if (!fat_slot(dir, i, &lba, &off)) break;
        if (lba != cached && !fat_read_sector(dir->volume, lba, buf)) return nullptr;
        cached = lba;
        const uint8_t* e = buf + off;
        if (!e[0]) break;
        bool match = fat_regular_entry(e);
        for (int j = 0; j < 11; ++j) if (e[j] != shortname[j]) match = false;
        if (match) {
            fat16_node* n = fat_node(dir->volume, lba, off, e, preceding_lfn);
            return n ? &n->vn : nullptr;
        }
        preceding_lfn = e[0] != 0xe5 && e[11] == 0x0f;
    }
    return nullptr;
}
static uint64_t fat16_read(vnode* vn, void* out, uint64_t len, uint64_t offset) {
    storage_guard guard;
    if (vn->type != VnodeType::FILE || offset >= vn->size) return 0;
    fat16_node* n = (fat16_node*)vn->fs_data;
    if (n->deleted) return 0;
    if (len > vn->size - offset) len = vn->size - offset;
    uint64_t done = 0;
    uint8_t buf[512];
    while (done < len) {
        uint64_t pos = offset + done;
        uint16_t c;
        if (!fat_chain_at(n, pos / (512 * n->volume->spc), false, &c)) break;
        uint32_t sector = (pos / 512) % n->volume->spc;
        uint32_t lba = fat_cluster_sector(n->volume, c) + sector;
        if (pos % 512 == 0 && len - done >= 512) {
            uint64_t limit = (len - done) / 512;
            if (limit > 128) limit = 128; // bounded 64 KiB requests
            uint64_t sectors = n->volume->spc - sector;
            if (sectors > limit) sectors = limit;
            uint32_t index = pos / (512 * n->volume->spc);
            uint16_t last = c;
            // Extend only after following a FAT link to the adjacent physical
            // cluster. Adjacent file offsets need not be contiguous on disk.
            while (sectors < limit) {
                uint16_t next;
                if (!fat_chain_at(n, ++index, false, &next) || next != last + 1) break;
                uint64_t take = limit - sectors;
                if (take > n->volume->spc) take = n->volume->spc;
                sectors += take;
                last = next;
            }
            if (!fat_read_sectors(n->volume, lba, sectors, (uint8_t*)out + done)) break;
            done += sectors * 512;
            continue;
        }
        if (!fat_read_sector(n->volume, lba, buf)) break;
        uint64_t take = 512 - pos % 512;
        if (take > len - done) take = len - done;
        memcpy((uint8_t*)out + done, buf + pos % 512, take);
        done += take;
    }
    return done;
}
static uint64_t fat16_write(vnode* vn, const void* data, uint64_t len, uint64_t offset) {
    storage_guard guard;
    // No holes, truncation or >4 GiB files in this first backend.
    if (vn->type != VnodeType::FILE || offset > vn->size || offset > 0xffffffff || len > 0xffffffff - offset || !len) return 0;
    fat16_node* n = (fat16_node*)vn->fs_data;
    if (n->deleted) return 0;
    fat16_volume* v = n->volume;
    uint8_t buf[512];
    // Respect host-created read-only files.
    if (!fat_read_sector(v, n->entry_sector, buf) || (buf[n->entry_offset + 11] & 1)) return 0;
    uint64_t done = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint16_t c;
        if (!fat_chain_at(n, pos / (512 * v->spc), true, &c)) break;
        uint32_t lba = fat_cluster_sector(v, c) + (pos / 512) % v->spc;
        uint64_t take = 512 - pos % 512;
        if (take > len - done) take = len - done;
        if (!fat_read_sector(v, lba, buf)) break;
        memcpy(buf + pos % 512, (const uint8_t*)data + done, take);
        if (!fat_write_sector(v, lba, buf)) break;
        done += take;
    }
    // Order data/FAT before publishing size and first cluster in directory.
    if (!fat_flush(v) || !fat_read_sector(v, n->entry_sector, buf)) return 0;
    uint64_t size = vn->size;
    if (done && offset + done > size) size = offset + done;
    fat_p16(buf + n->entry_offset + 26, n->cluster);
    fat_p32(buf + n->entry_offset + 28, size);
    if (done) fat_stamp(buf + n->entry_offset, false);
    if (!fat_write_sector(v, n->entry_sector, buf) || !fat_flush(v)) return 0;
    vn->size = size;
    return done;
}
static vnode* fat_make(vnode* vn, const char* name, bool directory) {
    storage_guard guard;
    if (vn->type != VnodeType::DIRECTORY || fat16_lookup(vn, name)) return nullptr;
    fat16_node* dir = (fat16_node*)vn->fs_data;
    fat16_volume* v = dir->volume;
    uint8_t shortname[11], buf[512];
    if (!fat_short_name(name, shortname)) return nullptr;
    uint32_t cached = 0xffffffff;
    for (uint32_t i = 0; i < v->clusters * v->spc * 16; ++i) {
        uint32_t lba; uint16_t off;
        if (!fat_slot(dir, i, &lba, &off)) break; // Directory growth is intentionally deferred.
        if (lba != cached && !fat_read_sector(v, lba, buf)) return nullptr;
        cached = lba;
        if (buf[off] && buf[off] != 0xe5) continue;
        uint16_t c = directory ? fat_allocate(v) : 0;
        if (directory && !c) return nullptr;
        if (directory) {
            uint8_t dots[512]; memset(dots, 0, sizeof(dots));
            for (int j = 0; j < 11; ++j) dots[j] = dots[32 + j] = ' ';
            dots[0] = dots[32] = dots[33] = '.';
            dots[11] = dots[43] = 0x10;
            fat_p16(dots + 26, c);
            fat_p16(dots + 58, dir->root ? 0 : dir->cluster);
            if (!fat_write_sector(v, fat_cluster_sector(v, c), dots)) return nullptr;
        }
        for (int j = 0; j < 32; ++j) buf[off + j] = 0;
        memcpy(buf + off, shortname, 11);
        buf[off + 11] = directory ? 0x10 : 0x20;
        fat_stamp(buf + off, true);
        fat_p16(buf + off + 26, c);
        if (!fat_flush(v) || !fat_write_sector(v, lba, buf) || !fat_flush(v)) return nullptr;
        fat16_node* n = fat_node(v, lba, off, buf + off);
        return n ? &n->vn : nullptr;
    }
    return nullptr;
}
static vnode* fat16_create(vnode* dir, const char* name) { return fat_make(dir, name, false); }
static vnode* fat16_mkdir(vnode* dir, const char* name) { return fat_make(dir, name, true); }
static int fat16_readdir(vnode* vn, uint64_t index, char* name, uint64_t* size, VnodeType* type) {
    storage_guard guard;
    if (vn->type != VnodeType::DIRECTORY) return -1;
    fat16_node* dir = (fat16_node*)vn->fs_data;
    uint8_t buf[512]; uint64_t found = 0; uint32_t cached = 0xffffffff;
    for (uint32_t i = 0; i < dir->volume->clusters * dir->volume->spc * 16; ++i) {
        uint32_t lba; uint16_t off;
        if (!fat_slot(dir, i, &lba, &off)) return 0;
        if (lba != cached && !fat_read_sector(dir->volume, lba, buf)) return -1;
        cached = lba;
        const uint8_t* e = buf + off;
        if (!e[0]) return 0;
        if (!fat_regular_entry(e) || found++ != index) continue;
        fat_display_name(e, name);
        if (size) *size = fat_u32(e + 28);
        if (type) *type = (e[11] & 0x10) ? VnodeType::DIRECTORY : VnodeType::FILE;
        return 1;
    }
    return -1;
}
static int fat16_stat(vnode* vn, file_info* info) {
    storage_guard guard;
    fat16_node* n = (fat16_node*)vn->fs_data;
    if (n->deleted || n->volume->failed || !n->volume->disk->online) return -1;
    if (n->root) return 0; // FAT16's fixed root has no directory-entry timestamps.
    uint8_t buf[512];
    if (!fat_read_sector(n->volume, n->entry_sector, buf)) return -1;
    const uint8_t* e = buf + n->entry_offset;
    fat_decode_datetime(fat_u16(e + 16), fat_u16(e + 14), e[13], &info->created);
    fat_decode_datetime(fat_u16(e + 24), fat_u16(e + 22), 0, &info->modified);
    return 0;
}
static int fat16_unlink(vnode* dir, const char* name) {
    storage_guard guard;
    vnode* vn = fat16_lookup(dir, name);
    if (!vn || vn->type != VnodeType::FILE) return -1;
    fat16_node* n = (fat16_node*)vn->fs_data;
    fat16_volume* v = n->volume;
    // Reject active handles and host long-name aliases; never let an old vnode
    // address a newly created file which reuses this directory slot.
    if (n->open_count || n->deleted || n->long_name) return -1;
    uint8_t buf[512];
    if (!fat_read_sector(v, n->entry_sector, buf) || (buf[n->entry_offset + 11] & 1)) return -1;
    // Validate the complete chain before changing the directory or FAT. A bitmap
    // detects cycles, including cycles shorter than the file's declared length.
    uint32_t bitmap_size = (v->clusters + 9) / 8;
    uint8_t* seen = (uint8_t*)kmalloc(bitmap_size);
    if (!seen) return -1;
    memset(seen, 0, bitmap_size);
    uint16_t c = n->cluster;
    bool valid = true;
    while (c) {
        if (!fat_cluster_valid(v, c) || (seen[c / 8] & (1u << (c % 8)))) { valid = false; break; }
        seen[c / 8] |= 1u << (c % 8);
        uint16_t next;
        if (!fat_get(v, c, &next)) { valid = false; break; }
        if (next >= 0xfff8) break;
        c = next;
        if (!c) { valid = false; break; }
    }
    if (!n->cluster && vn->size) valid = false;
    kfree(seen);
    if (!valid) return -1;
    // Remove the name durably before reclaiming clusters. Interruption can leak
    // space, but won't leave a live directory entry pointing at reused clusters.
    buf[n->entry_offset] = 0xe5;
    if (!fat_write_sector(v, n->entry_sector, buf) || !fat_flush(v)) return -1;
    n->deleted = true;
    c = n->cluster;
    while (c) {
        uint16_t next;
        if (!fat_get(v, c, &next) || !fat_set(v, c, 0)) { v->failed = true; return -1; }
        c = next >= 0xfff8 ? 0 : next;
    }
    return fat_flush(v) ? 0 : -1;
}
static int fat16_rmdir(vnode* dir, const char* name) {
    storage_guard guard;
    vnode* vn = fat16_lookup(dir, name);
    if (!vn || vn->type != VnodeType::DIRECTORY) return -1;
    fat16_node* n = (fat16_node*)vn->fs_data;
    fat16_volume* v = n->volume;
    if (n->root || n->deleted || n->open_count || n->long_name) return -1;

    // Check if directory is empty (only '.' and '..' or deleted entries allowed)
    uint8_t buf[512];
    uint32_t cached = 0xffffffff;
    for (uint32_t i = 0; i < v->clusters * v->spc * 16; ++i) {
        uint32_t lba; uint16_t off;
        if (!fat_slot(n, i, &lba, &off)) break;
        if (lba != cached && !fat_read_sector(v, lba, buf)) return -1;
        cached = lba;
        const uint8_t* e = buf + off;
        if (!e[0]) break;
        if (e[0] == 0xe5) continue;
        if (e[0] == '.') {
            if (e[1] == ' ' || (e[1] == '.' && e[2] == ' ')) continue;
        }
        return -1; // Directory not empty
    }

    if (!fat_read_sector(v, n->entry_sector, buf) || (buf[n->entry_offset + 11] & 1)) return -1;

    buf[n->entry_offset] = 0xe5;
    if (!fat_write_sector(v, n->entry_sector, buf) || !fat_flush(v)) return -1;
    n->deleted = true;

    uint16_t c = n->cluster;
    while (c) {
        uint16_t next;
        if (!fat_get(v, c, &next) || !fat_set(v, c, 0)) { v->failed = true; return -1; }
        c = next >= 0xfff8 ? 0 : next;
    }
    return fat_flush(v) ? 0 : -1;
}
static int fat16_statfs(vnode* vn, fs_info* out) {
    storage_guard guard;
    if (!vn || !out) return -1;
    fat16_node* n = (fat16_node*)vn->fs_data;
    fat16_volume* v = n->volume;
    if (v->failed || !v->disk->online) return -1;

    uint8_t sec[512];
    uint32_t free_clusters = 0;
    for (uint32_t s = 0; s < v->fat_sectors; ++s) {
        if (!fat_read_sector(v, v->fat_start + s, sec)) break;
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = s * 256 + i;
            if (c < 2) continue;
            if (c >= v->clusters + 2) break;
            uint16_t val = fat_u16(sec + i * 2);
            if (val == 0) free_clusters++;
        }
    }

    memset(out, 0, sizeof(*out));
    out->block_size = (uint32_t)v->spc * 512;
    out->total_blocks = v->clusters;
    out->free_blocks = free_clusters;
    out->total_bytes = (uint64_t)v->clusters * out->block_size;
    out->free_bytes = (uint64_t)free_clusters * out->block_size;
    strcpy(out->fs_type, "FAT16");
    return 0;
}
static fat16_volume* fat16_probe(block_device* disk) {
    storage_guard guard;
    uint8_t mbr[512], b[512];
    if (!block_read(disk, 0, mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa) return nullptr;
    for (int p = 0; p < 4; ++p) {
        const uint8_t* entry = mbr + 446 + p * 16;
        if (entry[4] != 0x06 && entry[4] != 0x0e) continue;
        uint32_t start = fat_u32(entry + 8), count = fat_u32(entry + 12);
        if (!start || !count || (uint64_t)start + count > disk->sectors || !block_read(disk, start, b)) continue;
        if (b[510] != 0x55 || b[511] != 0xaa || fat_u16(b + 11) != 512) continue;
        const char label[] = "WYZARDATA  ";
        bool labelled = true;
        for (int j = 0; j < 11; ++j) if (b[43 + j] != label[j]) labelled = false;
        if (!labelled) continue;
        uint8_t spc = b[13], fats = b[16];
        uint16_t reserved = fat_u16(b + 14), roots = fat_u16(b + 17), fatsecs = fat_u16(b + 22);
        uint32_t total = fat_u16(b + 19); if (!total) total = fat_u32(b + 32);
        uint32_t metadata = reserved + (uint32_t)fats * fatsecs + ((uint32_t)roots * 32 + 511) / 512;
        if (!spc || spc > 64 || (spc & (spc - 1)) || !reserved || !roots || !fatsecs || (fats != 1 && fats != 2) || total > count || metadata >= total) continue;
        uint32_t clusters = (total - metadata) / spc;
        if (clusters < 4085 || clusters >= 65525 || (clusters + 2) * 2 > (uint32_t)fatsecs * 512) continue;
        fat16_volume* v = (fat16_volume*)kmalloc(sizeof(fat16_volume));
        if (!v) return nullptr;
        v->disk = disk; v->start = start; v->sectors = total; v->spc = spc; v->fats = fats;
        v->fat_start = start + reserved; v->fat_sectors = fatsecs; v->root_entries = roots;
        v->root_start = v->fat_start + fats * fatsecs; v->data_start = start + metadata;
        v->clusters = clusters; v->failed = false; v->nodes = nullptr;
        v->chain_generation = 0;
        v->fat_cache_valid = false; v->fat_cache_lba = 0;
        v->root.volume = v; v->root.root = true; v->root.cluster = 0; v->root.next = nullptr;
        v->root.deleted = false; v->root.long_name = false; v->root.open_count = 0;
        v->root.chain_cluster = 0; v->root.chain_index = v->root.chain_generation = 0;
        v->root.vn.type = VnodeType::DIRECTORY; v->root.vn.ops = &fat16_ops;
        v->root.vn.fs_data = &v->root; v->root.vn.size = 0;
        return v;
    }
    return nullptr;
}
#endif
