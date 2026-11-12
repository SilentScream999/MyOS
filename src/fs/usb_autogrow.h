#ifndef WYZAR_USB_AUTOGROW_H
#define WYZAR_USB_AUTOGROW_H
#include "ext4_vfs.h"
#include "storage_policy.h"
#include <ext4_mkfs.h>

// The seed contents are retained. Build a new root in the unused
// tail, flush/verify it, then publish its partition entry. Failed setup can be
// retried from the seed; normal boots only check policy/layout and mount root.
struct wy_seed_entry {
    wy_seed_entry* next;
    uint64_t size;
    uint32_t crc;
    bool directory;
    char relative[512];
};
static bool wy_seed_path(char* out, const char* prefix, const char* relative) {
    size_t a = strlen(prefix), b = strlen(relative);
    if (a + b >= 640) return false;
    memcpy(out, prefix, a); memcpy(out + a, relative, b + 1); return true;
}
static bool wy_seed_signature(const char* prefix) {
    char path[640], signature[12]{}; ext4_file f{}; size_t read = 0;
    if (!wy_seed_path(path, prefix, "etc/root.cfg") || ext4_fopen(&f, path, "rb") != EOK) return false;
    int rc = ext4_fread(&f, signature, 11, &read); ext4_fclose(&f);
    if (rc != EOK || read != 11 || !wy_storage_same(signature, "WYZARROOT1\n", 11)) return false;
    if (!wy_seed_path(path, prefix, "bin/init.elf") || ext4_fopen(&f, path, "rb") != EOK) return false;
    rc = ext4_fread(&f, signature, 4, &read); ext4_fclose(&f);
    return rc == EOK && read == 4 && wy_storage_same(signature, "\177ELF", 4);
}
static bool wy_seed_inventory(wy_seed_entry*& head) {
    head = (wy_seed_entry*)kmalloc(sizeof(wy_seed_entry));
    if (!head) return false;
    memset(head, 0, sizeof(*head)); head->directory = true;
    wy_seed_entry* tail = head; unsigned count = 1;
    for (auto* current = head; current; current = current->next) {
        if (!current->directory) continue;
        char path[640]; wy_seed_path(path, "/seed/", current->relative);
        ext4_dir dir{}; if (ext4_dir_open(&dir, path) != EOK) return false;
        bool ok = true;
        while (const ext4_direntry* e = ext4_dir_entry_next(&dir)) {
            if (!e->inode || !e->name_length ||
                (e->name_length == 1 && e->name[0] == '.') ||
                (e->name_length == 2 && e->name[0] == '.' && e->name[1] == '.')) continue;
            size_t base = strlen(current->relative), total = base + (base ? 1 : 0) + e->name_length;
            if (total >= 512 || count == 32768) { ok = false; break; }
            for (unsigned i = 0; i < e->name_length; ++i) if (!e->name[i] || e->name[i] == '/') ok = false;
            if (!ok) break;
            auto* entry = (wy_seed_entry*)kmalloc(sizeof(wy_seed_entry));
            if (!entry) { ok = false; break; }
            memset(entry, 0, sizeof(*entry));
            memcpy(entry->relative, current->relative, base);
            if (base) entry->relative[base++] = '/';
            memcpy(entry->relative + base, e->name, e->name_length);
            tail->next = entry; tail = entry; ++count;
            wy_seed_path(path, "/seed/", entry->relative);
            uint32_t mode;
            if (ext4_mode_get(path, &mode) != EOK) { ok = false; break; }
            entry->directory = (mode & 0170000) == 0040000;
            if (!entry->directory) {
                if ((mode & 0170000) != 0100000) { ok = false; break; }
                ext4_file f{};
                if (ext4_fopen(&f, path, "rb") != EOK) { ok = false; break; }
                entry->size = ext4_fsize(&f); ext4_fclose(&f);
            }
        }
        if (ext4_dir_close(&dir) != EOK || !ok) return false;
    }
    return true;
}
static bool wy_seed_copy(wy_seed_entry* entries, uint8_t* buffer, bool verify) {
    for (auto* entry = entries->next; entry; entry = entry->next) {
        char source[640], destination[640];
        wy_seed_path(source, "/seed/", entry->relative);
        wy_seed_path(destination, "/target/", entry->relative);
        if (entry->directory) {
            if (!verify) {
                uint32_t mode; int rc = ext4_mode_get(destination, &mode);
                if (rc == ENOENT) { if (ext4_dir_mk(destination) != EOK) return false; }
                else if (rc != EOK || (mode & 0170000) != 0040000) return false;
            }
            continue;
        }
        ext4_file input{}, output{};
        if (ext4_fopen(&input, verify ? destination : source, "rb") != EOK) return false;
        if (ext4_fsize(&input) != entry->size) { ext4_fclose(&input); return false; }
        if (!verify && ext4_fopen(&output, destination, "wb") != EOK) { ext4_fclose(&input); return false; }
        uint64_t left = entry->size; uint32_t crc = 0xffffffffu; bool ok = true;
        while (left) {
            size_t amount = left > 65536 ? 65536 : (size_t)left, read = 0, written = 0;
            if (ext4_fread(&input, buffer, amount, &read) != EOK || read != amount) { ok = false; break; }
            crc = wy_storage_crc(buffer, read, crc);
            if (!verify && (ext4_fwrite(&output, buffer, read, &written) != EOK || written != read)) { ok = false; break; }
            left -= read;
        }
        if (ext4_fclose(&input) != EOK) ok = false;
        if (!verify && ext4_fclose(&output) != EOK) ok = false;
        if (!ok || (verify && crc != entry->crc)) return false;
        if (!verify) entry->crc = crc;
    }
    return true;
}

// 0: fixed/already prepared/no useful extra space. 1: activated new root.
// -1: failed; caller uses RAM recovery. Never mount an uncertain old root after
// a failed publication, since its partition entry might already have changed.
static int wy_usb_autogrow(wy_ext4_volume* seed, void (*progress)(const char*) = nullptr) {
    storage_guard guard;
    uint8_t policy[512], mbr[512], sb[1024];
    auto* disk = seed->disk;
    if (!block_read(disk, WY_STORAGE_POLICY_LBA, policy)) return -1;
    if (!wy_storage_policy_valid(policy) || wy_storage_u32(policy + 20) == 0) return 0;
    if (seed->start != wy_storage_u64(policy + 24) || seed->sectors != wy_storage_u64(policy + 32)) return 0;
    if (!block_read(disk, 0, mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa ||
        mbr[450] != 0xef || wy_storage_u32(mbr + 454) != 2048 || wy_storage_u32(mbr + 458) != 262144 ||
        mbr[466] != 0x83 || wy_storage_u32(mbr + 470) != seed->start ||
        wy_storage_u32(mbr + 474) != seed->sectors || seed->start != 264192) return -1;
    for (unsigned i = 478; i < 510; ++i) if (mbr[i]) return -1;
    if (disk->sectors > 0xffffffffULL || seed->sectors % 8 || !disk->flush) return -1;
    uint64_t start = seed->start + seed->sectors;
    if (start >= disk->sectors) return 0;
    uint64_t sectors = (disk->sectors - start) & ~7ULL;
    if (sectors <= seed->sectors) return 0; // Never shrink usable root capacity.
    if (!block_read_sectors(disk, seed->start + 2, 2, sb) || !wy_storage_same(sb + 104, policy + 40, 16) ||
        !wy_ext4_supported_profile(sb, seed->sectors)) return -1;

    auto* target = (wy_ext4_volume*)kmalloc(sizeof(wy_ext4_volume));
    auto* fs = (ext4_fs*)kmalloc(sizeof(ext4_fs));
    auto* buffer = (uint8_t*)kmalloc(65536);
    wy_seed_entry* inventory = nullptr;
    bool sr = false, sm = false, dr = false, dm = false, success = false;
    if (!target || !fs || !buffer) { if (target) kfree(target); if (fs) kfree(fs); if (buffer) kfree(buffer); return -1; }
    memset(target, 0, sizeof(*target)); memset(fs, 0, sizeof(*fs));
    target->disk = disk; target->start = start; target->sectors = sectors; target->unpublished = true;
    wy_ext4_bind(seed); wy_ext4_bind(target);
    do {
        if (progress) progress("Preparing USB storage (one time)...");
        if (ext4_device_register(&seed->bdev, "seed") != EOK) break; sr = true;
        if (ext4_mount("seed", "/seed/", false) != EOK) break; sm = true;
        if (ext4_recover("/seed/") != EOK || !wy_seed_signature("/seed/") || !wy_seed_inventory(inventory)) break;
        if (ext4_umount("/seed/") != EOK) break; sm = false;
        if (progress) progress("Creating expanded filesystem...");
        ext4_mkfs_info info{};
        info.block_size = 4096; info.journal = true; info.label = "WYZARDATA";
        for (unsigned i = 0; i < 16; ++i) info.uuid[i] = sb[104 + i] ^ (uint8_t)(0x5a + i);
        info.uuid[6] = (info.uuid[6] & 15) | 0x40; info.uuid[8] = (info.uuid[8] & 63) | 0x80;
        if (ext4_mkfs(fs, &target->bdev, &info, F_SET_EXT4) != EOK || target->failed || !wy_ext4_flush(target)) break;
        wy_ext4_bind(target);
        // Separate private prefixes prevent source filenames shadowing target paths.
        if (ext4_mount("seed", "/seed/", true) != EOK) break; sm = true;
        if (ext4_device_register(&target->bdev, "grow") != EOK) break; dr = true;
        if (ext4_mount("grow", "/target/", false) != EOK) break; dm = true;
        // This root is not visible yet. Populate without a live journal; an
        // interruption discards the entire target and retries from the seed.
        // Normal root mounting starts its journal after publication.
        if (progress) progress("Copying system files...");
        if (!wy_seed_copy(inventory, buffer, false)) break;
        if (progress) progress("Verifying system files...");
        if (!wy_seed_copy(inventory, buffer, true) || !wy_seed_signature("/target/")) break;
        if (ext4_umount("/target/") != EOK) break; dm = false;
        if (!wy_ext4_flush(target) || ext4_umount("/seed/") != EOK) break; sm = false;
        ext4_device_unregister("grow"); dr = false;
        ext4_device_unregister("seed"); sr = false;
        uint8_t current[512];
        if (!block_read(disk, 0, current) || !wy_storage_same(current, mbr, 512)) break;
        wy_storage_p32(mbr + 470, (uint32_t)start); wy_storage_p32(mbr + 474, (uint32_t)sectors);
        if (progress) progress("Activating expanded storage...");
        if (!block_write(disk, 0, mbr) || !disk->flush(disk) || !block_read(disk, 0, current) || !wy_storage_same(current, mbr, 512)) {
            disk->online = false; break;
        }
        memset(seed, 0, sizeof(*seed)); seed->disk = disk; seed->start = start; seed->sectors = sectors;
        success = true;
        if (progress) progress("USB storage ready.");
    } while (false);
    bool retain_target = false;
    if (dm) {
        // Failed I/O cannot flush the unpublished filesystem. Do not spin
        // trying to drain dirty buffers; RAM recovery never reuses this mount.
        if (target->failed || !disk->online) retain_target = true;
        else {
            if (ext4_umount("/target/") != EOK) retain_target = true;
        }
    }
    if (sm) ext4_umount("/seed/");
    if (dr) ext4_device_unregister("grow");
    if (sr) ext4_device_unregister("seed");
    while (inventory) { auto* next = inventory->next; kfree(inventory); inventory = next; }
    kfree(buffer); kfree(fs); if (!retain_target) kfree(target);
    if (!success) seed->failed = true;
    return success ? 1 : -1;
}
#endif
