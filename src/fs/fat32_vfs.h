#ifndef WYZAR_FAT32_VFS_H
#define WYZAR_FAT32_VFS_H
#include "vfs.h"
#include "block.h"
#include "fat_time.h"
#include "storage_policy.h"
#include "third_party/fatfs/ff.h"
#include "third_party/fatfs/diskio.h"
#ifndef STORAGE_HOST_TEST
#include "arch/rtc.h"
extern "C" DWORD get_fattime() {
    RtcTime now{};
    if (!rtc_read_time(&now)) return fat_encode_time({});
    return fat_encode_time({now.year, now.month, now.day, now.hour, now.minute, now.second, 0});
}
#endif

struct fat_volume;
struct fat_node {
    vnode vn;
    fat_volume* volume;
    fat_node* parent;
    fat_node* next;
    char name[256];
    unsigned opens;
    bool deleted;
    bool created_here, cleanup_done, save_recovery_done;
};
struct fat_volume {
    FATFS fs;
    block_device* disk;
    uint64_t start, sectors;
    char drive[4];
    fat_node root;
    fat_node* nodes;
    vnode* mount_point;
    char mount_path[128];
    bool failed;
};
static fat_volume fat_volumes[8];
static unsigned fat_volume_count;
extern "C" { PARTITION VolToPart[8] = {}; }
static vfs_ops fat_ops;
static int fat_eject(vnode* vn);
static void fat_recover_saves(vnode* dir, const char* path);

static void fat_decimal(uint64_t value, char* out) {
    char digits[24]; unsigned count = 0;
    do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
    unsigned at = 0;
    while (count) out[at++] = digits[--count];
    out[at] = 0;
}

extern "C" DSTATUS disk_status(BYTE id) {
    return id >= fat_volume_count || !fat_volumes[id].disk->online || fat_volumes[id].failed ? STA_NOINIT : 0;
}
extern "C" DSTATUS disk_initialize(BYTE id) { return disk_status(id); }
extern "C" DRESULT disk_read(BYTE id, BYTE* data, LBA_t sector, UINT count) {
    if (disk_status(id)) return RES_NOTRDY;
    auto& v = fat_volumes[id];
    if (!count || sector >= v.sectors || count > v.sectors - sector) return RES_PARERR;
    if (block_read_sectors(v.disk, v.start + sector, count, data)) return RES_OK;
    v.failed = true; return RES_ERROR;
}
extern "C" DRESULT disk_write(BYTE id, const BYTE* data, LBA_t sector, UINT count) {
    if (disk_status(id)) return RES_NOTRDY;
    auto& v = fat_volumes[id];
    if (!count || sector >= v.sectors || count > v.sectors - sector) return RES_PARERR;
    if (block_write_sectors(v.disk, v.start + sector, count, data)) return RES_OK;
    v.failed = true; return RES_ERROR;
}
extern "C" DRESULT disk_ioctl(BYTE id, BYTE command, void* data) {
    if (disk_status(id)) return RES_NOTRDY;
    auto& v = fat_volumes[id];
    if (command == GET_SECTOR_COUNT) { *(LBA_t*)data = v.sectors; return RES_OK; }
    if (command == GET_BLOCK_SIZE) { *(DWORD*)data = 1; return RES_OK; }
    if (command != CTRL_SYNC) return RES_PARERR;
    if (v.disk->flush && v.disk->flush(v.disk)) return RES_OK;
    v.failed = true; return RES_ERROR;
}
static bool fat_path(fat_node* n, char* out) {
    if (!n || n->deleted || n->volume->failed || !n->volume->disk->online) return false;
    fat_node* parents[64]; unsigned depth = 0;
    for (auto* p = n; p->parent; p = p->parent) {
        if (p->deleted || depth == 64) return false;
        parents[depth++] = p;
    }
    strcpy(out, n->volume->drive); unsigned used = 2;
    while (depth) {
        const char* name = parents[--depth]->name;
        unsigned len = strlen(name);
        if (used + len + 1 >= 512) return false;
        out[used++] = '/'; strcpy(out + used, name); used += len;
    }
    out[used] = 0; return true;
}
static bool fat_child_path(vnode* dir, const char* name, char* out) {
    if (!name || !*name || strlen(name) > 255 || strcmp(name,".") == 0 || strcmp(name,"..") == 0) return false;
    for (const char* p = name; *p; ++p) if (*p == '/' || *p == '\\' || *p == ':') return false;
    if (!fat_path((fat_node*)dir->fs_data, out)) return false;
    unsigned used = strlen(out), len = strlen(name);
    if (used + len + 1 >= 512) return false;
    out[used++] = '/'; strcpy(out + used, name); return true;
}
static vnode* fat_lookup(vnode* dir, const char* name) {
    storage_guard guard; char path[512]; FILINFO info{};
    if (!fat_path((fat_node*)dir->fs_data, path)) return nullptr;
    fat_recover_saves(dir, path);
    if (!fat_child_path(dir, name, path) || f_stat(path, &info) != FR_OK) return nullptr;
    auto* parent = (fat_node*)dir->fs_data;
    for (auto* n = parent->volume->nodes; n; n = n->next) {
        // FatFs returns the canonical name for case-insensitive lookups.
        if (!n->deleted && n->parent == parent && strcmp(n->name, info.fname) == 0) {
            n->vn.size = info.fsize; return &n->vn;
        }
    }
    auto* n = (fat_node*)kmalloc(sizeof(fat_node));
    if (!n) return nullptr;
    memset(n, 0, sizeof(*n));
    n->volume = parent->volume; n->parent = parent; strcpy(n->name, info.fname);
    n->vn = {info.fattrib & AM_DIR ? VnodeType::DIRECTORY : VnodeType::FILE, &fat_ops, n, info.fsize};
    n->next = n->volume->nodes; n->volume->nodes = n; return &n->vn;
}
static int fat_open(vnode* vn, file*) {
    storage_guard guard; char path[512]; auto* n = (fat_node*)vn->fs_data;
    if (!fat_path(n, path)) return -1;
    ++n->opens; return 0;
}
static void fat_close(vnode* vn) {
    storage_irq_guard guard; auto* n = (fat_node*)vn->fs_data;
    if (n->opens) --n->opens;
}
static uint64_t fat_read(vnode* vn, void* out, uint64_t len, uint64_t offset) {
    storage_guard guard; char path[512]; FIL f{}; UINT done = 0;
    if (len > 0xffffffff || !fat_path((fat_node*)vn->fs_data, path) || f_open(&f, path, FA_READ) != FR_OK) return 0;
    if (offset >= f_size(&f)) { f_close(&f); return 0; }
    FRESULT rc = f_lseek(&f, offset);
    if (rc == FR_OK) rc = f_read(&f, out, len, &done);
    FRESULT closed = f_close(&f);
    return rc == FR_OK && closed == FR_OK ? done : 0;
}
// FatFs seek expansion leaves undefined contents. Explicitly zero new bytes
// before publishing a larger file through the VFS.
static FRESULT fat_extend(FIL* f, uint64_t size) {
    uint8_t zero[512] = {};
    FRESULT rc = f_lseek(f, f_size(f));
    while (rc == FR_OK && f_size(f) < size) {
        uint64_t left = size - f_size(f);
        UINT amount = left > sizeof(zero) ? sizeof(zero) : (UINT)left, done = 0;
        rc = f_write(f, zero, amount, &done);
        if (rc == FR_OK && done != amount) rc = FR_DENIED;
    }
    return rc;
}
static uint64_t fat_write(vnode* vn, const void* in, uint64_t len, uint64_t offset) {
    storage_guard guard; char path[512]; FIL f{}; UINT done = 0;
    auto* node = (fat_node*)vn->fs_data;
    uint64_t maximum = node->volume->fs.fs_type == FS_EXFAT ? UINT64_MAX : 0xffffffffULL;
    if (offset > maximum || len > maximum - offset || len > 0xffffffff ||
        !fat_path((fat_node*)vn->fs_data, path) || f_open(&f, path, FA_WRITE) != FR_OK) return 0;
    FRESULT rc = FR_OK;
    if (offset > f_size(&f)) rc = fat_extend(&f, offset);
    if (rc == FR_OK) rc = f_lseek(&f, offset);
    if (rc == FR_OK) rc = f_write(&f, in, len, &done);
    vn->size = f_size(&f);
    FRESULT closed = f_close(&f);
    return rc == FR_OK && closed == FR_OK ? done : 0;
}
static vnode* fat_create(vnode* dir, const char* name) {
    storage_guard guard; char path[512]; FIL f{};
    if (!fat_child_path(dir, name, path) || f_open(&f, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) return nullptr;
    if (f_close(&f) != FR_OK) return nullptr;
    vnode* result = fat_lookup(dir, name);
    if (result) ((fat_node*)result->fs_data)->created_here = true;
    return result;
}
static vnode* fat_mkdir(vnode* dir, const char* name) {
    storage_guard guard; char path[512];
    if (!fat_child_path(dir, name, path) || f_mkdir(path) != FR_OK) return nullptr;
    return fat_lookup(dir, name);
}
static bool fat_transfer_name(const char* name) {
    const char* prefix = ".explorer-copy-";
    while (*prefix) if (*name++ != *prefix++) return false;
    unsigned digits = 0;
    while (*name >= '0' && *name <= '9') { ++name; if (++digits > 20) return false; }
    return digits && strcmp(name, ".tmp") == 0;
}
// Clean each browsed folder once per mount. Files created during this mount
// and files with open VFS handles are protected, including the create/open gap.
static bool fat_cleanup_transfers(vnode* vn, const char* path) {
    auto* parent = (fat_node*)vn->fs_data;
    if (parent->cleanup_done) return true;
    DIR dir{}; FILINFO info{};
    if (f_opendir(&dir, path) != FR_OK) return false;
    bool complete = false;
    for (unsigned step = 0; step < 8192; ++step) {
        if (f_readdir(&dir, &info) != FR_OK) break;
        if (!info.fname[0]) { complete = true; break; }
        if ((info.fattrib & AM_DIR) || !fat_transfer_name(info.fname)) continue;
        bool active = false;
        for (auto* n = parent->volume->nodes; n; n = n->next)
            if (!n->deleted && n->parent == parent && strcmp(n->name, info.fname) == 0 &&
                (n->opens || n->created_here)) active = true;
        if (active) continue;
        char child[512];
        if (!fat_child_path(vn, info.fname, child) || f_unlink(child) != FR_OK) break;
        for (auto* n = parent->volume->nodes; n; n = n->next)
            if (n->parent == parent && strcmp(n->name, info.fname) == 0) n->deleted = true;
    }
    f_closedir(&dir);
    parent->cleanup_done = complete;
    return complete;
}
static int fat_readdir(vnode* vn, uint64_t index, char* name, uint64_t* size, VnodeType* type) {
    storage_guard guard; char path[512]; DIR dir{}; FILINFO info{};
    if (!fat_path((fat_node*)vn->fs_data, path)) return -1;
    fat_recover_saves(vn, path);
    if (index == 0) fat_cleanup_transfers(vn, path);
    if (f_opendir(&dir, path) != FR_OK) return -1;
    FRESULT rc = FR_OK;
    for (uint64_t i = 0; i <= index; ++i) {
        rc = f_readdir(&dir, &info);
        if (rc != FR_OK || !info.fname[0]) break;
    }
    f_closedir(&dir);
    if (rc != FR_OK) return -1;
    if (!info.fname[0]) return 0;
    strcpy(name, info.fname); if (size) *size = info.fsize;
    if (type) *type = info.fattrib & AM_DIR ? VnodeType::DIRECTORY : VnodeType::FILE;
    return 1;
}
static int fat_remove(vnode* dir, const char* name, VnodeType type) {
    storage_guard guard; char path[512]; vnode* vn = fat_lookup(dir, name);
    if (!vn || vn->type != type || ((fat_node*)vn->fs_data)->opens || !fat_child_path(dir, name, path)) return -1;
    if (f_unlink(path) != FR_OK) return -1;
    ((fat_node*)vn->fs_data)->deleted = true; return 0;
}
static int fat_unlink(vnode* dir, const char* name) { return fat_remove(dir, name, VnodeType::FILE); }
static int fat_rmdir(vnode* dir, const char* name) { return fat_remove(dir, name, VnodeType::DIRECTORY); }
static int fat_sync(vnode* vn) {
    storage_guard guard; auto* n = (fat_node*)vn->fs_data;
    return disk_ioctl(n->volume - fat_volumes, CTRL_SYNC, nullptr) == RES_OK ? 0 : -1;
}
static int fat_stat(vnode* vn, file_info* info) {
    storage_guard guard; char path[512]; FILINFO fi{}; auto* n = (fat_node*)vn->fs_data;
    if (!fat_path(n, path) || (n->parent && f_stat(path, &fi) != FR_OK)) return -1;
    memset(info, 0, sizeof(*info)); info->size = vn->size = fi.fsize;
    info->type = vn->type == VnodeType::DIRECTORY ? 2 : 1;
    info->modified = fat_decode_time(fi.fdate, fi.ftime);
    info->created = fat_decode_time(fi.crdate, fi.crtime);
    return 0;
}
static int fat_statfs(vnode* vn, fs_info* info) {
    storage_guard guard; auto* n = (fat_node*)vn->fs_data; char path[512]; DWORD free; FATFS* fs;
    if (!fat_path(n, path) || f_getfree(n->volume->drive, &free, &fs) != FR_OK) return -1;
    memset(info, 0, sizeof(*info)); info->block_size = fs->csize * 512;
    info->total_blocks = fs->n_fatent - 2; info->free_blocks = free;
    info->total_bytes = (uint64_t)info->total_blocks * info->block_size;
    info->free_bytes = (uint64_t)free * info->block_size;
    strcpy(info->fs_type, fs->fs_type == FS_EXFAT ? "exFAT" : "FAT32"); return 0;
}
static int fat_truncate(vnode* vn, uint64_t size) {
    storage_guard guard; char path[512]; FIL f{};
    auto* node = (fat_node*)vn->fs_data;
    if ((node->volume->fs.fs_type != FS_EXFAT && size > 0xffffffff) ||
        !fat_path(node, path) || f_open(&f, path, FA_WRITE) != FR_OK) return -1;
    FRESULT rc;
    if (size > f_size(&f)) rc = fat_extend(&f, size);
    else {
        rc = f_lseek(&f, size);
        if (rc == FR_OK) rc = f_truncate(&f);
    }
    vn->size = f_size(&f); FRESULT closed = f_close(&f);
    return rc == FR_OK && closed == FR_OK ? 0 : -1;
}
static int fat_rename(vnode* dir, const char* name, vnode* destination, const char* new_name) {
    storage_guard guard; char from[512], to[512];
    auto* parent = (fat_node*)destination->fs_data;
    auto* source = fat_lookup(dir, name);
    if (!source || ((fat_node*)dir->fs_data)->volume != parent->volume ||
        !fat_child_path(dir, name, from) || !fat_child_path(destination, new_name, to)) return -1;
    auto* n = (fat_node*)source->fs_data;
    if (n->opens) return -1;
    for (auto* p = parent; p; p = p->parent) if (p == n) return -1;
    if (f_rename(from, to) != FR_OK) return -1;
    n->parent = parent; strcpy(n->name, new_name); return 0;
}
struct fat_save_paths {
    char folder[512], target[512], fresh[512], backup[512], record[512], ready[512];
};
// User tasks have 16 KiB kernel stacks. Keep the multi-path save workspace
// off those stacks, including when recovery is nested inside path lookup.
struct fat_save_workspace {
    fat_save_paths* data;
    fat_save_workspace() : data((fat_save_paths*)kmalloc(sizeof(fat_save_paths))) {}
    ~fat_save_workspace() { kfree(data); }
};
struct fat_save_record { char magic[12], target[256]; uint32_t checksum; };
static bool fat_save_child(const char* parent, const char* name, char* out) {
    unsigned length = strlen(parent);
    if (length + strlen(name) + 2 > 512) return false;
    strcpy(out, parent); out[length++] = '/'; strcpy(out + length, name); return true;
}
static bool fat_save_locations(vnode* dir, const char* name, fat_save_paths* p) {
    return fat_child_path(dir, name, p->folder) && fat_save_child(p->folder, "new", p->fresh) &&
        fat_save_child(p->folder, "old", p->backup) && fat_save_child(p->folder, "target", p->record) &&
        fat_save_child(p->folder, "ready", p->ready);
}
static bool fat_save_write(const char* path, const void* data, uint64_t length) {
    FIL f{};
    if (f_open(&f, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) return false;
    bool ok = true;
    for (uint64_t offset = 0; offset < length;) {
        uint64_t left = length - offset;
        UINT amount = left > 65536 ? 65536 : (UINT)left, done = 0;
        if (f_write(&f, (const uint8_t*)data + offset, amount, &done) != FR_OK || done != amount) { ok = false; break; }
        offset += done;
    }
    return f_close(&f) == FR_OK && ok;
}
static int fat_save_exists(const char* path) {
    FILINFO info{}; FRESULT rc = f_stat(path, &info);
    if (rc == FR_NO_FILE || rc == FR_NO_PATH) return 0;
    return rc == FR_OK && !(info.fattrib & AM_DIR) ? 1 : -1;
}
static bool fat_save_remove(const char* path) {
    FRESULT rc = f_unlink(path);
    return rc == FR_OK || rc == FR_NO_FILE || rc == FR_NO_PATH;
}
// An interrupted FAT rename may leave aliases of one cluster chain. Retain
// those backups for filesystem repair instead of freeing the visible data.
static bool fat_save_independent(const char* first, const char* second) {
    if (!fat_save_exists(first) || !fat_save_exists(second)) return true;
    FIL a{}, b{};
    if (f_open(&a, first, FA_READ) != FR_OK) return false;
    if (f_open(&b, second, FA_READ) != FR_OK) { f_close(&a); return false; }
    bool independent = !a.obj.sclust || a.obj.sclust != b.obj.sclust;
    f_close(&a); f_close(&b); return independent;
}
static bool fat_recover_save(vnode* dir, fat_save_paths* p) {
    FIL f{}; fat_save_record record{}; UINT read = 0;
    if (f_open(&f, p->record, FA_READ) != FR_OK) return false;
    bool valid = f_size(&f) == sizeof(record) && f_read(&f, &record, sizeof(record), &read) == FR_OK && read == sizeof(record);
    f_close(&f);
    if (!valid || !wy_storage_same(record.magic, "WYZARSAVE1", 11) || record.target[255] ||
        record.checksum != (wy_storage_crc(&record, sizeof(record) - 4) ^ 0xffffffffu) ||
        !fat_child_path(dir, record.target, p->target)) return false;
    auto* parent = (fat_node*)dir->fs_data;
    for (auto* n = parent->volume->nodes; n; n = n->next)
        if (!n->deleted && n->parent == parent && n->opens && strcmp(n->name, record.target) == 0) return false;
    int target = fat_save_exists(p->target), backup = fat_save_exists(p->backup);
    if (target < 0 || backup < 0) return false;
    if (!target && backup) {
        if (f_rename(p->backup, p->target) != FR_OK) return false;
    } else if (!target && fat_save_exists(p->ready) == 1 && fat_save_exists(p->fresh) == 1) {
        if (f_rename(p->fresh, p->target) != FR_OK) return false;
    }
    if (!fat_save_independent(p->backup, p->target) || !fat_save_independent(p->fresh, p->target)) return false;
    return fat_save_remove(p->fresh) && fat_save_remove(p->backup) && fat_save_remove(p->ready) &&
        fat_save_remove(p->record) && f_unlink(p->folder) == FR_OK;
}
static bool fat_save_name(const char* name) {
    const char* prefix = ".wyzar-save-";
    while (*prefix) if (*name++ != *prefix++) return false;
    unsigned digits = 0;
    while (*name >= '0' && *name <= '9') { ++name; if (++digits > 20) return false; }
    return digits && !*name;
}
static void fat_recover_saves(vnode* dir, const char* path) {
    auto* parent = (fat_node*)dir->fs_data;
    if (parent->save_recovery_done) return;
    DIR entries{}; FILINFO info{};
    if (f_opendir(&entries, path) != FR_OK) return;
    bool complete = false, recovered = true;
    for (unsigned i = 0; i < 8192; ++i) {
        if (f_readdir(&entries, &info) != FR_OK) break;
        if (!info.fname[0]) { complete = true; break; }
        if (!(info.fattrib & AM_DIR) || !fat_save_name(info.fname)) continue;
        fat_save_workspace workspace;
        if (!workspace.data || !fat_save_locations(dir, info.fname, workspace.data) ||
            !fat_recover_save(dir, workspace.data)) recovered = false;
    }
    f_closedir(&entries);
    parent->save_recovery_done = complete && recovered;
}
static int fat_replace(vnode* dir, const char* name, const void* data, uint64_t length) {
    storage_guard guard; auto* parent = (fat_node*)dir->fs_data;
    fat_save_workspace workspace;
    if (!workspace.data) return -1;
    auto& paths = *workspace.data; FILINFO original{};
    if ((!data && length) || (parent->volume->fs.fs_type != FS_EXFAT && length > 0xffffffff) ||
        !fat_child_path(dir, name, paths.target)) return -1;
    vnode* target = fat_lookup(dir, name);
    FRESULT exists = f_stat(paths.target, &original);
    if (exists != FR_OK && exists != FR_NO_FILE) return -1;
    if (exists == FR_OK && ((original.fattrib & (AM_DIR | AM_RDO)) || !target || ((fat_node*)target->fs_data)->opens)) return -1;
    static uint64_t serial;
    bool available = false;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        char number[24], folder[48] = ".wyzar-save-";
        fat_decimal(++serial, number); strcpy(folder + 12, number);
        if (!fat_save_locations(dir, folder, &paths)) return -1;
        FRESULT rc = f_mkdir(paths.folder);
        if (rc == FR_OK) { available = true; break; }
        if (rc != FR_EXIST) return -1;
    }
    if (!available) return -1;
    parent->save_recovery_done = false;
    fat_save_record record{}; strcpy(record.magic, "WYZARSAVE1");
    strcpy(record.target, exists == FR_OK ? original.fname : name);
    record.checksum = wy_storage_crc(&record, sizeof(record) - 4) ^ 0xffffffffu;
    bool staged = fat_save_write(paths.record, &record, sizeof(record)) && fat_save_write(paths.fresh, data, length) &&
        fat_save_write(paths.ready, nullptr, 0);
    if (!staged) {
        // The original has not moved. Clean this call's owned staging files
        // even if an incomplete target record cannot be used for recovery.
        fat_save_remove(paths.fresh); fat_save_remove(paths.ready); fat_save_remove(paths.record);
        f_unlink(paths.folder); return -1;
    }
    bool moved = exists != FR_OK || f_rename(paths.target, paths.backup) == FR_OK;
    bool published = moved && f_rename(paths.fresh, paths.target) == FR_OK;
    if (published && target) ((fat_node*)target->fs_data)->deleted = true;
    bool recovered = fat_recover_save(dir, &paths);
    return published && recovered ? 0 : -1;
}
static void fat_init_ops() {
    fat_ops.read = fat_read; fat_ops.write = fat_write; fat_ops.open = fat_open; fat_ops.close = fat_close;
    fat_ops.lookup = fat_lookup; fat_ops.create = fat_create; fat_ops.mkdir = fat_mkdir; fat_ops.readdir = fat_readdir;
    fat_ops.unlink = fat_unlink; fat_ops.rmdir = fat_rmdir; fat_ops.sync = fat_sync; fat_ops.stat = fat_stat;
    fat_ops.statfs = fat_statfs; fat_ops.truncate = fat_truncate; fat_ops.rename = fat_rename;
    fat_ops.eject = fat_eject;
    fat_ops.replace = fat_replace;
}
static void fat_drive_name(fat_volume* volume, const char* label, char* out) {
    while (*label == ' ' || *label == '.') ++label;
    unsigned used = 0;
    while (*label && used < 63) {
        unsigned char c = *label++;
        bool invalid = c < 32 || c == 127;
        for (const char* p = "/\\:*?\"<>|"; *p; ++p) if (c == (unsigned char)*p) invalid = true;
        out[used++] = invalid ? '_' : c;
    }
    while (used && (out[used - 1] == ' ' || out[used - 1] == '.')) --used;
    out[used] = 0;
    if (used) return;
    strcpy(out, "USB Drive ("); used = strlen(out);
    uint64_t bytes = volume->sectors * 512;
    uint64_t unit = bytes >= (1ull << 30) ? (1ull << 30) : bytes >= (1ull << 20) ? (1ull << 20) : 1024;
    char number[24];
    if (unit == (1ull << 30)) {
        uint64_t tenths = (bytes * 10 + unit / 2) / unit;
        fat_decimal(tenths / 10, number); strcpy(out + used, number); used += strlen(number);
        out[used++] = '.'; out[used++] = '0' + tenths % 10; strcpy(out + used, " GiB)");
    } else {
        fat_decimal((bytes + unit / 2) / unit, number); strcpy(out + used, number); used += strlen(number);
        strcpy(out + used, unit == 1024 ? " KiB)" : " MiB)");
    }
}
static bool fat_mountpoint_available(const char* path) {
    vnode* point = vfs_namei(path);
    if (!point) return true;
    if (point->type != VnodeType::DIRECTORY || !point->ops || !point->ops->readdir) return false;
    for (unsigned i = 0; i < vfs_mount_count; ++i) if (vfs_mounts[i].root == point) return false;
    // Reuse empty mount folders left by an earlier boot. Preserve folders
    // containing user data and choose a numbered suffix for those collisions.
    for (unsigned i = 0; i < 3; ++i) {
        char name[256]; uint64_t size; VnodeType type;
        int result = point->ops->readdir(point, i, name, &size, &type);
        if (!result) return true;
        if (result < 0 || (strcmp(name, ".") && strcmp(name, ".."))) return false;
    }
    return false;
}
static bool fat_mount_slice(block_device* disk, uint64_t start, uint64_t sectors) {
    if (!sectors || start >= disk->sectors || sectors > disk->sectors - start || fat_volume_count == 8) return false;
    for (unsigned i = 0; i < fat_volume_count; ++i) if (fat_volumes[i].disk == disk && fat_volumes[i].start == start) return true;
    unsigned id = fat_volume_count++; auto& v = fat_volumes[id];
    memset(&v, 0, sizeof(v)); v.disk = disk; v.start = start; v.sectors = sectors;
    v.drive[0] = '0' + id; v.drive[1] = ':'; VolToPart[id] = {(BYTE)id, 0};
    if (f_mount(&v.fs, v.drive, 1) != FR_OK || (v.fs.fs_type != FS_FAT32 && v.fs.fs_type != FS_EXFAT)) {
        f_mount(nullptr, v.drive, 0); --fat_volume_count; return false;
    }
    char label[64] = {}, base[64], mount[128];
    if (f_getlabel(v.drive, label, nullptr) != FR_OK) {
        f_mount(nullptr, v.drive, 0); --fat_volume_count; return false;
    }
    fat_drive_name(&v, label, base);
    bool available = false;
    for (unsigned attempt = 1; attempt <= 64; ++attempt) {
        strcpy(mount, "/mnt/"); strcpy(mount + 5, base);
        if (attempt > 1) {
            unsigned at = strlen(mount); char number[24]; fat_decimal(attempt, number);
            strcpy(mount + at, " ("); at += 2; strcpy(mount + at, number); at += strlen(number);
            strcpy(mount + at, ")");
        }
        if (fat_mountpoint_available(mount)) { available = true; break; }
    }
    if (!available) { f_mount(nullptr, v.drive, 0); --fat_volume_count; return false; }
    v.root.volume = &v; v.root.vn = {VnodeType::DIRECTORY, &fat_ops, &v.root, 0};
    if (vfs_mkdir_p(mount) != 0 || vfs_mount(vfs_namei(mount), &v.root.vn) != 0) {
        f_mount(nullptr, v.drive, 0); vfs_rmdir(mount); --fat_volume_count; return false;
    }
    // Fetch the covered vnode directly from the attachment, before following it.
    v.mount_point = vfs_mounts[vfs_mount_count - 1].point;
    strcpy(v.mount_path, mount);
    print((char*)(v.fs.fs_type == FS_EXFAT ? "[storage] exFAT mounted at:" : "[storage] FAT32 mounted at:"));
    print(mount); return true;
}
static void fat_retire_unavailable() {
    storage_guard guard;
    for (unsigned i = 0; i < fat_volume_count; ++i) {
        auto& v = fat_volumes[i];
        if (!v.mount_point || (!v.failed && v.disk->online)) continue;
        v.failed = true;
        if (vfs_unmount(v.mount_point, &v.root.vn) != 0) continue;
        v.mount_point = nullptr;
        f_mount(nullptr, v.drive, 0);
        vfs_rmdir(v.mount_path);
        print((char*)"[storage] External drive detached:"); print(v.mount_path);
    }
}
static int fat_eject(vnode* vn) {
    storage_guard guard;
    auto* volume = ((fat_node*)vn->fs_data)->volume;
    if (!volume->mount_point || volume->failed || !volume->disk->online) return -1;
    block_device* disk = volume->disk;
    for (unsigned i = 0; i < fat_volume_count; ++i) {
        auto& v = fat_volumes[i];
        if (v.disk != disk || !v.mount_point) continue;
        if (v.root.opens) return -2;
        for (auto* n = v.nodes; n; n = n->next) if (n->opens) return -2;
    }
    for (unsigned i = 0; i < fat_volume_count; ++i)
        if (fat_volumes[i].disk == disk && fat_volumes[i].mount_point &&
            wyzar_fatfs_sync(i) != FR_OK) return -3;
    bool unsupported = disk->flush_unsupported;
    disk->online = false;
    fat_retire_unavailable();
    return unsupported ? 1 : 0;
}
static void fat_probe_disk(block_device* disk) {
    uint8_t sector[512];
    if (!block_read(disk, 0, sector) || sector[510] != 0x55 || sector[511] != 0xaa) return;
    if (wy_storage_same(sector + 3, "EXFAT   ", 8)) { fat_mount_slice(disk, 0, disk->sectors); return; }
    if ((sector[0] == 0xeb || sector[0] == 0xe9) && sector[11] == 0 && sector[12] == 2 &&
        sector[17] == 0 && sector[18] == 0 && sector[22] == 0 && sector[23] == 0 && wy_storage_u32(sector + 36)) {
        fat_mount_slice(disk, 0, disk->sectors); return;
    }
    for (unsigned p = 0; p < 4; ++p) {
        auto* entry = sector + 446 + p * 16;
        if (entry[4] == 0x0b || entry[4] == 0x0c || entry[4] == 0x07)
            fat_mount_slice(disk, wy_storage_u32(entry + 8), wy_storage_u32(entry + 12));
    }
}
// The external-drive worker scans after root selection. Existing volume
// objects remain stable when hardware disconnects; handles then fail closed.
#ifndef STORAGE_HOST_TEST
static void fat_scan_external() {
    storage_guard guard; fat_init_ops();
    fat_retire_unavailable();
    static bool legacy_folders_checked = false;
    if (!legacy_folders_checked) {
        legacy_folders_checked = true;
        for (unsigned i = 0; i < 8; ++i) {
            char path[] = "/mnt/usb1"; path[8] += i;
            if (vfs_namei(path) && fat_mountpoint_available(path) && vfs_rmdir(path) == 0) ++vfs_storage_generation;
        }
    }
    static bool scanned[8];
    for (int d = 0; d < usb_disk_count; ++d) {
        block_device* disk = &usb_disks[d].disk;
        if (!disk->online || (wy_ext4_system && wy_ext4_system->disk == disk)) continue;
        if (scanned[d]) continue;
        scanned[d] = true;
        bool known = false;
        for (unsigned i = 0; i < fat_volume_count; ++i) if (fat_volumes[i].disk == disk) known = true;
        if (known) continue;
        fat_probe_disk(disk);
    }
}
static void fat_mount_worker() {
    for (;;) {
        fat_scan_external();
        task_sleep_ms(500);
    }
}
#endif
#endif
