#ifndef WYZAR_EXT4_VFS_H
#define WYZAR_EXT4_VFS_H
#include "vfs.h"
#include "block.h"
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_types.h>

// One system volume for now. Probing is read-only; mount/replay happens only
// after the boot code has established that the root candidate is unique.
struct wy_ext4_volume;
struct wy_ext4_node {
    vnode vn;
    wy_ext4_volume* volume;
    wy_ext4_node* parent;
    wy_ext4_node* next;
    char name[256];
    unsigned opens;
    bool deleted;
    bool cursor_valid;
    uint64_t cursor_index, cursor_offset;
};
struct wy_ext4_volume {
    block_device* disk;
    uint64_t start, sectors;
    ext4_blockdev_iface iface;
    ext4_blockdev bdev;
    uint8_t scratch[512];
    wy_ext4_node root;
    wy_ext4_node* nodes[256];
    bool failed, mounted, needs_flush, unpublished;
};
static wy_ext4_volume* wy_ext4_system = nullptr;
#ifdef STORAGE_HOST_TEST
static uint64_t wy_ext4_directory_steps;
#endif
static int wy_ext4_unmount(wy_ext4_volume* v);
static int wy_ext4_shutdown() { return wy_ext4_system ? wy_ext4_unmount(wy_ext4_system) : 0; }
static uint32_t wy_ext4_u32(const uint8_t* p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static bool wy_ext4_ready(wy_ext4_volume* v) { return v && !v->failed && v->disk->online; }
static int wy_ext4_io_open(ext4_blockdev* bd) {
    return wy_ext4_ready((wy_ext4_volume*)bd->bdif->p_user) ? EOK : EIO;
}
static int wy_ext4_io_read(ext4_blockdev* bd, void* out, uint64_t lba, uint32_t count) {
    auto* v = (wy_ext4_volume*)bd->bdif->p_user;
    if (!wy_ext4_ready(v) || !out || !count || lba >= v->sectors || count > v->sectors - lba) return EIO;
    // Keep the USB backend's maximum transfer size independent of filesystem blocks.
    for (uint32_t done = 0; done < count; ) {
        uint32_t n = count - done; if (n > 128) n = 128;
        if (!block_read_sectors(v->disk, v->start + lba + done, n, (uint8_t*)out + (uint64_t)done * 512)) {
            v->failed = true; return EIO;
        }
        done += n;
    }
    return EOK;
}
static bool wy_ext4_flush(wy_ext4_volume* v) {
    if (!wy_ext4_ready(v)) return false;
    if (!v->needs_flush) return true;
    if (!v->disk->flush || !v->disk->flush(v->disk)) {
        v->failed = true; return false;
    }
    v->needs_flush = false;
    return true;
}
static int wy_ext4_io_write(ext4_blockdev* bd, const void* in, uint64_t lba, uint32_t count) {
    auto* v = (wy_ext4_volume*)bd->bdif->p_user;
    if (!wy_ext4_ready(v) || !in || !count || lba >= v->sectors || count > v->sectors - lba) return EIO;
    v->needs_flush = true;
    for (uint32_t done = 0; done < count; ) {
        uint32_t n = count - done; if (n > 128) n = 128;
        if (!block_write_sectors(v->disk, v->start + lba + done, n, (const uint8_t*)in + (uint64_t)done * 512)) {
            v->failed = true; return EIO;
        }
        done += n;
    }
    // lwext4 has no barrier callback. Make every completed library block write
    // durable, including journal records before commit and checkpoint writes.
    // Conservative and slow, but never acknowledge a volatile journal commit.
    return v->unpublished || wy_ext4_flush(v) ? EOK : EIO;
}
static int wy_ext4_io_close(ext4_blockdev* bd) {
    return wy_ext4_flush((wy_ext4_volume*)bd->bdif->p_user) ? EOK : EIO;
}
static bool wy_ext4_path(wy_ext4_node* n, char* out) {
    if (!n || n->deleted || !wy_ext4_ready(n->volume)) return false;
    wy_ext4_node* stack[128]; unsigned depth = 0;
    while (n->parent) {
        if (depth == 128 || n->deleted) return false;
        stack[depth++] = n; n = n->parent;
    }
    unsigned pos = 0; out[pos++] = '/';
    while (depth) {
        const char* name = stack[--depth]->name;
        size_t len = strlen(name);
        if (pos + len + (depth ? 1 : 0) >= 512) return false;
        memcpy(out + pos, name, len); pos += len;
        if (depth) out[pos++] = '/';
    }
    out[pos] = 0; return true;
}
static bool wy_ext4_child_path(wy_ext4_node* dir, const char* name, char* out) {
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strlen(name) > 255 ||
        dir->vn.type != VnodeType::DIRECTORY || !wy_ext4_path(dir, out)) return false;
    for (const char* p = name; *p; ++p) if (*p == '/') return false;
    size_t n = strlen(out), len = strlen(name);
    if (n > 1) out[n++] = '/';
    if (n + len >= 512) return false;
    memcpy(out + n, name, len + 1); return true;
}
static vfs_ops wy_ext4_ops;
static unsigned wy_ext4_bucket(wy_ext4_node* parent, const char* name) {
    uint32_t hash = (uintptr_t)parent >> 4;
    for (const char* p = name; *p; ++p) hash = (hash ^ (uint8_t)*p) * 16777619u;
    return hash & 255u;
}
static vnode* wy_ext4_lookup(vnode* vn, const char* name) {
    storage_guard guard;
    auto* dir = (wy_ext4_node*)vn->fs_data;
    char path[512]; if (!wy_ext4_child_path(dir, name, path)) return nullptr;
    auto*& bucket = dir->volume->nodes[wy_ext4_bucket(dir, name)];
    for (auto* n = bucket; n; n = n->next)
        if (!n->deleted && n->parent == dir && !strcmp(n->name, name)) return &n->vn;
    uint32_t mode;
    if (ext4_mode_get(path, &mode) != EOK) return nullptr;
    uint32_t kind = mode & 0170000;
    if (kind != 0100000 && kind != 0040000) return nullptr; // Symlink/device policy comes later.
    auto* n = (wy_ext4_node*)kmalloc(sizeof(wy_ext4_node));
    if (!n) return nullptr;
    memset(n, 0, sizeof(*n)); n->volume = dir->volume; n->parent = dir; strcpy(n->name, name);
    n->vn = {kind == 0040000 ? VnodeType::DIRECTORY : VnodeType::FILE, &wy_ext4_ops, n, 0};
    if (kind == 0100000) {
        ext4_file f{};
        if (ext4_fopen(&f, path, "rb") != EOK) { kfree(n); return nullptr; }
        n->vn.size = ext4_fsize(&f); ext4_fclose(&f);
    }
    n->next = bucket; bucket = n; return &n->vn;
}
static int wy_ext4_open(vnode* vn, file*) {
    storage_irq_guard guard;
    auto* n = (wy_ext4_node*)vn->fs_data;
    if (n->deleted || !wy_ext4_ready(n->volume)) return -1;
    ++n->opens; return 0;
}
static void wy_ext4_close(vnode* vn) {
    storage_irq_guard guard; auto* n = (wy_ext4_node*)vn->fs_data; if (n->opens) --n->opens;
}
static uint64_t wy_ext4_read(vnode* vn, void* out, uint64_t len, uint64_t offset) {
    storage_guard guard;
    auto* n = (wy_ext4_node*)vn->fs_data; char path[512];
    if (vn->type != VnodeType::FILE || !out || offset > INT64_MAX || !wy_ext4_path(n, path)) return 0;
    ext4_file f{}; size_t done = 0;
    if (ext4_fopen(&f, path, "rb") != EOK) return 0;
    int rc = ext4_fseek(&f, offset, 0);
    if (rc == EOK) rc = ext4_fread(&f, out, len, &done);
    ext4_fclose(&f); return rc == EOK ? done : 0;
}
static uint64_t wy_ext4_write(vnode* vn, const void* in, uint64_t len, uint64_t offset) {
    storage_guard guard;
    auto* n = (wy_ext4_node*)vn->fs_data; char path[512];
    if (vn->type != VnodeType::FILE || !in || offset > INT64_MAX || len > INT64_MAX - offset || !wy_ext4_path(n, path)) return 0;
    ext4_file f{}; size_t done = 0;
    if (ext4_fopen(&f, path, "r+b") != EOK) return 0;
    int rc = ext4_fseek(&f, offset, 0);
    if (rc == EOK) rc = ext4_fwrite(&f, in, len, &done);
    vn->size = ext4_fsize(&f);
    if (ext4_fclose(&f) != EOK || rc != EOK || !wy_ext4_flush(n->volume)) return 0;
    return done;
}
static vnode* wy_ext4_create(vnode* vn, const char* name) {
    storage_guard guard;
    auto* dir = (wy_ext4_node*)vn->fs_data; char path[512];
    if (!wy_ext4_child_path(dir, name, path)) return nullptr;
    uint32_t mode; if (ext4_mode_get(path, &mode) == EOK) return nullptr;
    dir->cursor_valid = false;
    ext4_file f{};
    if (ext4_fopen(&f, path, "wb") != EOK) return nullptr;
    if (ext4_fclose(&f) != EOK || !wy_ext4_flush(dir->volume)) return nullptr;
    return wy_ext4_lookup(vn, name);
}
static vnode* wy_ext4_mkdir(vnode* vn, const char* name) {
    storage_guard guard;
    auto* dir = (wy_ext4_node*)vn->fs_data; char path[512];
    if (!wy_ext4_child_path(dir, name, path)) return nullptr;
    uint32_t mode; if (ext4_mode_get(path, &mode) == EOK) return nullptr;
    dir->cursor_valid = false;
    if (ext4_dir_mk(path) != EOK || !wy_ext4_flush(dir->volume)) return nullptr;
    return wy_ext4_lookup(vn, name);
}
static int wy_ext4_readdir(vnode* vn, uint64_t index, char* name, uint64_t* size, VnodeType* type) {
    storage_guard guard;
    auto* n = (wy_ext4_node*)vn->fs_data; char path[512];
    if (!name || !wy_ext4_path(n, path)) return -1;
    ext4_dir dir{}; if (ext4_dir_open(&dir, path) != EOK) return -1;
    uint64_t requested = index;
    if (n->cursor_valid && index == n->cursor_index) {
        dir.next_off = n->cursor_offset;
        index = 0;
    }
    n->cursor_valid = false;
    int result = 0;
    while (const ext4_direntry* e = ext4_dir_entry_next(&dir)) {
#ifdef STORAGE_HOST_TEST
        ++wy_ext4_directory_steps;
#endif
        if (!e->inode || !e->name_length ||
            (e->name_length == 1 && e->name[0] == '.') ||
            (e->name_length == 2 && e->name[0] == '.' && e->name[1] == '.')) continue;
        if (index--) continue;
        memcpy(name, e->name, e->name_length); name[e->name_length] = 0;
        vnode* child = wy_ext4_lookup(vn, name);
        if (!child) { result = -1; break; }
        if (size) *size = child->size;
        if (type) *type = child->type;
        n->cursor_index = requested + 1;
        n->cursor_offset = dir.next_off;
        n->cursor_valid = true;
        result = 1; break;
    }
    ext4_dir_close(&dir); return wy_ext4_ready(n->volume) ? result : -1;
}
static int wy_ext4_remove(vnode* dir, const char* name, bool directory) {
    storage_guard guard;
    vnode* vn = wy_ext4_lookup(dir, name);
    if (!vn || (vn->type == VnodeType::DIRECTORY) != directory) return -1;
    auto* n = (wy_ext4_node*)vn->fs_data; char path[512];
    if (n->opens || !wy_ext4_path(n, path)) return -1;
    if (directory) {
        char child[256];
        if (wy_ext4_readdir(vn, 0, child, nullptr, nullptr) != 0) return -1;
    }
    ((wy_ext4_node*)dir->fs_data)->cursor_valid = false;
    int rc = directory ? ext4_dir_rm(path) : ext4_fremove(path);
    if (rc != EOK) return -1;
    n->deleted = true; return wy_ext4_flush(n->volume) ? 0 : -1;
}
static int wy_ext4_unlink(vnode* dir, const char* name) { return wy_ext4_remove(dir, name, false); }
static int wy_ext4_rmdir(vnode* dir, const char* name) { return wy_ext4_remove(dir, name, true); }
static int wy_ext4_stat(vnode* vn, file_info* info) {
    auto* n = (wy_ext4_node*)vn->fs_data;
    if (n->deleted || !wy_ext4_ready(n->volume)) return -1;
    info->size = vn->size; return 0; // Unknown civil timestamps stay zero.
}
static int wy_ext4_statfs(vnode* vn, fs_info* out) {
    storage_guard guard;
    auto* n = (wy_ext4_node*)vn->fs_data;
    ext4_mount_stats stats{};
    if (!out || !wy_ext4_ready(n->volume) || ext4_mount_point_stats("/", &stats) != EOK) return -1;
    memset(out, 0, sizeof(*out));
    out->block_size = stats.block_size;
    out->total_bytes = stats.blocks_count * stats.block_size;
    out->free_bytes = stats.free_blocks_count * stats.block_size;
    out->total_blocks = stats.blocks_count > UINT32_MAX ? UINT32_MAX : (uint32_t)stats.blocks_count;
    out->free_blocks = stats.free_blocks_count > UINT32_MAX ? UINT32_MAX : (uint32_t)stats.free_blocks_count;
    strcpy(out->fs_type, "ext4"); return 0;
}
static int wy_ext4_truncate(vnode* vn, uint64_t size) {
    storage_guard guard;
    auto* n = (wy_ext4_node*)vn->fs_data; char path[512];
    if (vn->type != VnodeType::FILE || size > INT64_MAX || !wy_ext4_path(n, path)) return -1;
    ext4_file f{};
    if (ext4_fopen(&f, path, "r+b") != EOK) return -1;
    int rc = EOK;
    // lwext4 ftruncate only shrinks. Explicitly zero-fill extension so no old
    // allocation bytes become visible, including the tail of a partial block.
    if (size > ext4_fsize(&f)) {
        uint8_t zero[4096]{};
        rc = ext4_fseek(&f, ext4_fsize(&f), 0);
        while (rc == EOK && ext4_fsize(&f) < size) {
            uint64_t left = size - ext4_fsize(&f);
            size_t amount = left > sizeof(zero) ? sizeof(zero) : (size_t)left, done = 0;
            rc = ext4_fwrite(&f, zero, amount, &done);
            if (rc == EOK && done != amount) rc = EIO;
        }
    } else rc = ext4_ftruncate(&f, size);
    vn->size = ext4_fsize(&f);
    if (ext4_fclose(&f) != EOK || rc != EOK || !wy_ext4_flush(n->volume)) return -1;
    return 0;
}
static int wy_ext4_sync(vnode* vn) {
    storage_guard guard;
    auto* n = (wy_ext4_node*)vn->fs_data;
    if (n->deleted || !wy_ext4_ready(n->volume)) return -1;
    return ext4_cache_flush("/") == EOK && wy_ext4_flush(n->volume) ? 0 : -1;
}
static int wy_ext4_rename(vnode* dir, const char* name, vnode* dest, const char* new_name) {
    storage_guard guard;
    auto* parent = (wy_ext4_node*)dir->fs_data;
    auto* destination = (wy_ext4_node*)dest->fs_data;
    char old_path[512], new_path[512];
    if (parent->volume != destination->volume || !wy_ext4_child_path(parent, name, old_path) ||
        !wy_ext4_child_path(destination, new_name, new_path)) return -1;
    vnode* vn = wy_ext4_lookup(dir, name);
    if (!vn) return -1;
    auto* n = (wy_ext4_node*)vn->fs_data;
    if (parent == destination && !strcmp(name, new_name)) return 0;
    if (n->opens || wy_ext4_lookup(dest, new_name)) return -1;
    for (auto* p = destination; p; p = p->parent) if (p == n) return -1;
    parent->cursor_valid = destination->cursor_valid = false;
    if (ext4_frename(old_path, new_path) != EOK || !wy_ext4_flush(parent->volume)) return -1;
    auto** link = &parent->volume->nodes[wy_ext4_bucket(parent, name)];
    while (*link && *link != n) link = &(*link)->next;
    if (*link) *link = n->next;
    n->parent = destination; strcpy(n->name, new_name);
    auto*& bucket = parent->volume->nodes[wy_ext4_bucket(destination, new_name)];
    n->next = bucket; bucket = n;
    return 0;
}
static int wy_ext4_replace(vnode* dir, const char* name, const void* data, uint64_t len) {
    storage_guard guard;
    auto* parent = (wy_ext4_node*)dir->fs_data;
    char path[512], temporary[512], temporary_name[32];
    if (len > INT64_MAX || !wy_ext4_child_path(parent, name, path)) return -1;
    vnode* target = wy_ext4_lookup(dir, name);
    if (target && (target->type != VnodeType::FILE || ((wy_ext4_node*)target->fs_data)->opens)) return -1;
    uint32_t mode;
    int exists = ext4_mode_get(path, &mode);
    if (exists != EOK && exists != ENOENT) return -1;
    if (exists == EOK && (!target || (mode & 0170000) != 0100000)) return -1;
    static uint64_t serial;
    bool available = false;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        strcpy(temporary_name, ".wy-save-");
        char digits[21]; unsigned count = 0; uint64_t value = ++serial;
        do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
        unsigned pos = 9; while (count) temporary_name[pos++] = digits[--count]; temporary_name[pos] = 0;
        if (!wy_ext4_child_path(parent, temporary_name, temporary)) return -1;
        if (ext4_mode_get(temporary, &mode) == ENOENT) { available = true; break; }
    }
    if (!available) return -1;
    parent->cursor_valid = false;
    ext4_file f{}; size_t written = 0;
    if (ext4_fopen(&f, temporary, "wb") != EOK) return -1;
    int rc = len ? ext4_fwrite(&f, data, len, &written) : EOK;
    int close_rc = ext4_fclose(&f);
    if (rc != EOK || close_rc != EOK || written != len || !wy_ext4_flush(parent->volume)) return -1;
    rc = exists == EOK ? ext4_fexchange(temporary, path) : ext4_frename(temporary, path);
    if (rc != EOK || !wy_ext4_flush(parent->volume)) { parent->volume->failed = true; return -1; }
    if (target) target->size = len;
    // Publication is already durable. A crash during cleanup can leave the
    // previous file under .wy-save-N, but cannot expose a partial destination.
    if (exists == EOK && ext4_fremove(temporary) != EOK) return -1;
    return wy_ext4_flush(parent->volume) ? 0 : -1;
}
static void wy_ext4_init_ops() {
    wy_ext4_ops = {wy_ext4_read, wy_ext4_write, wy_ext4_open, wy_ext4_close,
        wy_ext4_lookup, wy_ext4_create, wy_ext4_mkdir, wy_ext4_readdir,
        wy_ext4_unlink, wy_ext4_stat, wy_ext4_rmdir, wy_ext4_statfs,
        wy_ext4_truncate, wy_ext4_sync, wy_ext4_rename, wy_ext4_replace};
}
// Returns all labelled candidates, including unsupported ones, so an invalid
// second system partition cannot cause us to silently choose another root.
static unsigned wy_ext4_probe(block_device* disk, wy_ext4_volume* out) {
    storage_guard guard; uint8_t mbr[512], sb[1024]; unsigned found = 0;
    if (!block_read(disk, 0, mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa) return 0;
    for (unsigned p = 0; p < 4; ++p) {
        const uint8_t* e = mbr + 446 + p * 16;
        uint64_t start = wy_ext4_u32(e + 8), sectors = wy_ext4_u32(e + 12);
        if (e[4] != 0x83 || !start || sectors < 8 || start >= disk->sectors || sectors > disk->sectors - start) continue;
        if (!block_read_sectors(disk, start + 2, 2, sb) || sb[56] != 0x53 || sb[57] != 0xef) continue;
        const char label[16] = "WYZARDATA"; bool match = true;
        for (unsigned i = 0; i < 16; ++i) if (sb[120 + i] != (uint8_t)label[i]) match = false;
        if (!match) continue;
        if (++found == 1 && out) { memset(out, 0, sizeof(*out)); out->disk = disk; out->start = start; out->sectors = sectors; }
    }
    return found;
}
static void wy_ext4_bind(wy_ext4_volume* v) {
    memset(&v->iface, 0, sizeof(v->iface)); memset(&v->bdev, 0, sizeof(v->bdev));
    v->iface.open = wy_ext4_io_open; v->iface.bread = wy_ext4_io_read;
    v->iface.bwrite = wy_ext4_io_write; v->iface.close = wy_ext4_io_close;
    v->iface.ph_bsize = 512; v->iface.ph_bcnt = v->sectors;
    v->iface.ph_bbuf = v->scratch; v->iface.p_user = v;
    v->bdev.bdif = &v->iface; v->bdev.part_size = v->sectors * 512;
}
static bool wy_ext4_supported_profile(const uint8_t* sb, uint64_t sectors) {
    uint32_t compat = wy_ext4_u32(sb + 92), incompat = wy_ext4_u32(sb + 96), ro = wy_ext4_u32(sb + 100);
    return (compat & 4) && !(compat & ~0x2cu) && !(incompat & ~0x46u) && !(ro & ~0x43u) &&
        wy_ext4_u32(sb + 24) == 2 && wy_ext4_u32(sb + 4) <= sectors / 8 &&
        !wy_ext4_u32(sb + 232) && !wy_ext4_u32(sb + 228);
}
static vnode* wy_ext4_mount(wy_ext4_volume* v) {
    storage_guard guard;
    if (!wy_ext4_ready(v) || !v->disk->flush) return nullptr;
    // Deliberately support our generated feature profile first. Never silently
    // mount an arbitrary modern ext4 volume with unsupported write semantics.
    uint8_t sb[1024];
    if (!block_read_sectors(v->disk, v->start + 2, 2, sb)) return nullptr;
    if (!wy_ext4_supported_profile(sb, v->sectors)) return nullptr;
    wy_ext4_bind(v);
    if (ext4_device_register(&v->bdev, "system") != EOK) return nullptr;
    if (ext4_mount("system", "/", false) != EOK) { ext4_device_unregister("system"); return nullptr; }
    if (ext4_recover("/") != EOK || ext4_journal_start("/") != EOK || !wy_ext4_flush(v)) {
        v->failed = true; return nullptr;
    }
    v->mounted = true; wy_ext4_init_ops();
    wy_ext4_system = v; vfs_shutdown_hook = wy_ext4_shutdown;
    v->root.volume = v; v->root.vn = {VnodeType::DIRECTORY, &wy_ext4_ops, &v->root, 0};
    return &v->root.vn;
}
static int wy_ext4_unmount(wy_ext4_volume* v) {
    storage_guard guard;
    if (!v || !v->mounted || !wy_ext4_ready(v)) return -1;
    if (ext4_journal_stop("/") != EOK || ext4_umount("/") != EOK || !wy_ext4_flush(v)) { v->failed = true; return -1; }
    ext4_device_unregister("system"); v->mounted = false;
    if (wy_ext4_system == v) { wy_ext4_system = nullptr; vfs_shutdown_hook = nullptr; }
    return 0;
}
#endif
