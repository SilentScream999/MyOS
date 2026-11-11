#ifndef VFS_H
#define VFS_H

// ── vfs.h ───────────────────────────────────────────────────────────────────
//
// Phase 5 — Virtual File System (VFS)
//
// Abstracts file operations across multiple filesystems.
//
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#ifndef STORAGE_HOST_TEST
#include "helpers.h"
#endif
#include "fileinfo.h"

struct vnode;
struct file;

// VNODE TYPES
enum class VnodeType {
    FILE,
    DIRECTORY,
    DEVICE,
};

// VFS OPERATIONS
struct vfs_ops {
    // Read `len` bytes from `vnode` into `buf` at `offset`. Returns bytes read.
    uint64_t (*read)(vnode* vn, void* buf, uint64_t len, uint64_t offset);
    
    // Write `len` bytes from `buf` to `vnode` at `offset`. Returns bytes written.
    uint64_t (*write)(vnode* vn, const void* buf, uint64_t len, uint64_t offset);
    
    // Open a file. Returns 0 on success.
    int (*open)(vnode* vn, file* f);
    
    // Close a file.
    void (*close)(vnode* vn);
    
    // Lookup a child by name in a directory vnode. Returns child vnode or null.
    vnode* (*lookup)(vnode* dir, const char* name);
    
    // Create a new file in a directory vnode.
    vnode* (*create)(vnode* dir, const char* name);
    
    // Create a new directory in a directory vnode.
    vnode* (*mkdir)(vnode* dir, const char* name);

    // Read directory entry at index. Returns 1 if found, 0 on EOF, -1 on error.
    int (*readdir)(vnode* dir, uint64_t index, char* name_out, uint64_t* size_out, VnodeType* type_out);
    int (*unlink)(vnode* dir, const char* name);
    int (*stat)(vnode* vn, file_info* info);
    int (*rmdir)(vnode* dir, const char* name);
    int (*statfs)(vnode* vn, fs_info* out);
    int (*truncate)(vnode* vn, uint64_t size);
    int (*sync)(vnode* vn);
    int (*rename)(vnode* dir, const char* name, vnode* destination, const char* new_name);
    int (*replace)(vnode* dir, const char* name, const void* data, uint64_t len);
};

// VNODE: A node in the VFS tree (represents a file or directory inode)
struct vnode {
    VnodeType type;
    vfs_ops*  ops;
    void*     fs_data;  // Private data for the specific filesystem (e.g., RamFS node)
    uint64_t  size;     // Size of the file in bytes
};

// FILE: An open instance of a vnode (owned by a process file descriptor)
struct file {
    vnode*   vn;        // The underlying vnode
    uint64_t offset;    // Current read/write seek offset
    int      mode;      // O_RDONLY, O_WRONLY, O_RDWR (placeholder)
    int      refcount;  // Reference count for fork/dup
};

// GLOBAL VFS STATE
static vnode* vfs_root = nullptr;
// Called before poweroff/reset; filesystems may commit and close their journal.
static int (*vfs_shutdown_hook)() = nullptr;
struct vfs_mount_entry { vnode* point; vnode* root; };
static vfs_mount_entry vfs_mounts[16];
static unsigned vfs_mount_count = 0;
static vnode* vfs_follow_mount(vnode* vn) {
    for (unsigned i = 0; vn && i < vfs_mount_count; ++i)
        if (vn == vfs_mounts[i].point) return vfs_mounts[i].root;
    return vn;
}

static int vfs_mount(vnode* point, vnode* root) {
    if (!point || !root || point->type != root->type || vfs_mount_count == 16) return -1;
    for (unsigned i = 0; i < vfs_mount_count; ++i)
        if (vfs_mounts[i].point == point) return -1;
    vfs_mounts[vfs_mount_count++] = {point, root};
    return 0;
}

// ── VFS GLOBAL API ────────────────────────────────────────────────────────

static inline void vfs_mount_root(vnode* root_vnode) {
    vfs_root = root_vnode;
    vfs_mount_count = 0;
}

// Simple path parser: extracts the next path component.
// E.g., next_path_component("mnt/foo/bar", buf) puts "mnt" into buf, returns pointer to "foo/bar"
static const char* vfs_next_component(const char* path, char* comp) {
    while (*path == '/') path++; // Skip leading slashes
    if (*path == '\0') {
        comp[0] = '\0';
        return nullptr;
    }
    
    int i = 0;
    while (*path != '/' && *path != '\0') {
        if (i == 255) { comp[0] = 1; comp[1] = 0; return nullptr; }
        comp[i++] = *path++;
    }
    comp[i] = '\0';
    return path;
}

static bool vfs_resolve_path(const char* cwd, const char* in_path, char* out_path, uint64_t max_len) {
    if (!out_path || max_len < 2) return false;
    const char* c = (cwd && cwd[0] == '/') ? cwd : "/";
    if (!in_path || in_path[0] == '\0') {
        uint64_t clen = strlen(c);
        if (clen >= max_len) return false;
        strcpy(out_path, c);
        return true;
    }

    char temp[512];
    uint64_t tlen = 0;

    if (in_path[0] != '/') {
        while (*c && tlen + 1 < sizeof(temp)) {
            temp[tlen++] = *c++;
        }
        if (tlen > 0 && temp[tlen - 1] != '/' && tlen + 1 < sizeof(temp)) {
            temp[tlen++] = '/';
        }
    }
    const char* p = in_path;
    while (*p && tlen + 1 < sizeof(temp)) {
        temp[tlen++] = *p++;
    }
    temp[tlen] = '\0';

    out_path[0] = '/';
    out_path[1] = '\0';
    uint64_t out_len = 1;

    const char* src = temp;
    while (*src) {
        while (*src == '/') src++;
        if (!*src) break;

        char comp[256];
        uint64_t clen = 0;
        while (*src && *src != '/' && clen + 1 < sizeof(comp)) {
            comp[clen++] = *src++;
        }
        comp[clen] = '\0';

        if (strcmp(comp, ".") == 0) {
            continue;
        } else if (strcmp(comp, "..") == 0) {
            if (out_len > 1) {
                while (out_len > 1 && out_path[out_len - 1] != '/') {
                    out_len--;
                }
                if (out_len > 1) {
                    out_len--;
                }
                out_path[out_len] = '\0';
            }
        } else {
            if (out_len > 1) {
                if (out_len + 1 >= max_len) return false;
                out_path[out_len++] = '/';
            }
            for (uint64_t i = 0; i < clen; i++) {
                if (out_len + 1 >= max_len) return false;
                out_path[out_len++] = comp[i];
            }
            out_path[out_len] = '\0';
        }
    }
    return true;
}

// Walk with a namespace parent stack: '..' crosses a mount back to its parent.
// Filesystem-specific '..' entries must never escape or bypass a VFS mount.
static vnode* vfs_walk(const char* path, char* final_name = nullptr, bool create_dirs = false) {
    if (!vfs_root || !path) return nullptr;
    char resolved[256];
    if (*path != '/') {
        if (!vfs_resolve_path("/", path, resolved, sizeof(resolved))) return nullptr;
        path = resolved;
    }
    vnode* parents[128];
    unsigned depth = 0;
    vnode* current = vfs_root;
    const char* p = path + 1;
    char comp[256];
    while (true) {
        const char* next = vfs_next_component(p, comp);
        if (!next && comp[0]) return nullptr;
        if (!comp[0]) {
            if (final_name) final_name[0] = 0;
            return current;
        }
        if (current->type != VnodeType::DIRECTORY) return nullptr;
        const char* rest = next;
        while (*rest == '/') ++rest;
        if (final_name && !*rest) {
            if (strcmp(comp, ".") == 0 || strcmp(comp, "..") == 0) return nullptr;
            strcpy(final_name, comp);
            return current;
        }
        if (strcmp(comp, ".") == 0) { p = next; continue; }
        if (strcmp(comp, "..") == 0) {
            if (depth) current = parents[--depth];
            p = next;
            continue;
        }
        if (depth == 128 || !current->ops || !current->ops->lookup) return nullptr;
        vnode* child = current->ops->lookup(current, comp);
        if (!child && create_dirs && current->ops->mkdir)
            child = current->ops->mkdir(current, comp);
        if (!child) return nullptr;
        parents[depth++] = current;
        current = vfs_follow_mount(child);
        p = next;
        if (*p == '/' && current->type != VnodeType::DIRECTORY) return nullptr;
    }
}

static vnode* vfs_namei(const char* path) { return vfs_walk(path); }
static vnode* vfs_namei_parent(const char* path, char* name) { return vfs_walk(path, name); }

// ── HIGH-LEVEL VFS FUNCTIONS ────────────────────────────────────────────────

// Open or create a file
static int vfs_open(const char* path, int flags, file* f) {
    if (!f || (flags & 3) == 3 || ((flags & 0x200) && !(flags & 3))) return -1;
    vnode* vn = vfs_namei(path);
    if (vn && (flags & 0xc0) == 0xc0) return -1; // O_CREAT | O_EXCL
    if (!vn) {
        if (!(flags & 0x40)) return -1; // O_CREAT; reads never create files.
        // File doesn't exist and O_CREAT was requested.
        char name[256];
        vnode* parent = vfs_namei_parent(path, name);
        if (parent && name[0] && parent->type == VnodeType::DIRECTORY && parent->ops->create) {
            vn = parent->ops->create(parent, name);
            if (!vn) return -1;
        } else {
            return -1;
        }
    }
    
    f->vn = vn;
    f->offset = 0;
    f->mode = flags;
    f->refcount = 1;
    
    if (vn->ops->open && vn->ops->open(vn, f) != 0) { f->vn = nullptr; return -1; }
    if ((flags & 0x200) && (!vn->ops->truncate || vn->ops->truncate(vn, 0) != 0)) {
        if (vn->ops->close) vn->ops->close(vn);
        f->vn = nullptr; return -1;
    }
    
    return 0; // Success
}

static uint64_t vfs_read(file* f, void* buf, uint64_t len) {
    if (!f || !f->vn || !f->vn->ops->read) return 0;
    if ((f->mode & 3) == 1) return 0;
    uint64_t read_bytes = f->vn->ops->read(f->vn, buf, len, f->offset);
    f->offset += read_bytes;
    return read_bytes;
}

static uint64_t vfs_write(file* f, const void* buf, uint64_t len) {
    if (!f || !f->vn || !f->vn->ops->write) return 0;
    if ((f->mode & 3) == 0) return 0;
    uint64_t written = f->vn->ops->write(f->vn, buf, len, f->offset);
    f->offset += written;
    
    // Update vnode size if we wrote past the end
    if (f->offset > f->vn->size) {
        f->vn->size = f->offset;
    }
    
    return written;
}

static void vfs_close(file* f) {
    if (!f || !f->vn) return;
    f->refcount--;
    if (f->refcount <= 0) {
        if (f->vn->ops->close) {
            f->vn->ops->close(f->vn);
        }
        f->vn = nullptr;
    }
}

static int vfs_ftruncate(file* f, uint64_t size) {
    if (!f || !f->vn || !(f->mode & 3) || !f->vn->ops->truncate) return -1;
    return f->vn->ops->truncate(f->vn, size);
}
static int vfs_fsync(file* f) {
    if (!f || !f->vn || !f->vn->ops->sync) return -1;
    return f->vn->ops->sync(f->vn);
}

static int vfs_mkdir(const char* path) {
    char name[256];
    vnode* parent = vfs_namei_parent(path, name);
    if (!parent || !parent->ops || !parent->ops->mkdir) return -1;
    vnode* vn = parent->ops->mkdir(parent, name);
    return vn ? 0 : -1;
}

static int vfs_mkdir_p(const char* path) {
    vnode* vn = vfs_walk(path, nullptr, true);
    return vn && vn->type == VnodeType::DIRECTORY ? 0 : -1;
}

static void vfs_ensure_parent_dirs(const char* path) {
    char parent_path[256];
    int last_slash = -1;
    for (int i = 0; path[i] && i < 255; i++) {
        if (path[i] == '/') last_slash = i;
    }
    if (last_slash > 0) {
        for (int i = 0; i < last_slash; i++) parent_path[i] = path[i];
        parent_path[last_slash] = '\0';
        vfs_mkdir_p(parent_path);
    }
}

static int vfs_stat_node(vnode* vn, file_info* info) {
    if (!vn || !info) return -1;
    memset(info, 0, sizeof(*info));
    info->size = vn->size;
    info->type = vn->type == VnodeType::FILE ? 1 : vn->type == VnodeType::DIRECTORY ? 2 : 3;
    return vn->ops->stat ? vn->ops->stat(vn, info) : 0;
}

static int vfs_readdir(const char* path, uint64_t index, char* name_out, uint64_t* size_out, VnodeType* type_out) {
    vnode* vn = vfs_namei(path);
    if (!vn || vn->type != VnodeType::DIRECTORY || !vn->ops || !vn->ops->readdir) return -1;
    int result = vn->ops->readdir(vn, index, name_out, size_out, type_out);
    if (result == 1 && vfs_mount_count && vn->ops->lookup) {
        vnode* covered = vn->ops->lookup(vn, name_out);
        vnode* visible = vfs_follow_mount(covered);
        if (visible != covered) {
            file_info info;
            if (vfs_stat_node(visible, &info) != 0) return -1;
            if (size_out) *size_out = info.size;
            if (type_out) *type_out = visible->type;
        }
    }
    return result;
}

static int vfs_unlink(const char* path) {
    char name[256];
    vnode* parent = vfs_namei_parent(path, name);
    if (!parent || !name[0] || !parent->ops || !parent->ops->unlink) return -1;
    vnode* covered = parent->ops->lookup ? parent->ops->lookup(parent, name) : nullptr;
    if (vfs_follow_mount(covered) != covered) return -1;
    return parent->ops->unlink(parent, name);
}
static int vfs_rename(const char* path, const char* new_path) {
    char name[256], new_name[256];
    vnode* parent = vfs_namei_parent(path, name);
    vnode* destination = vfs_namei_parent(new_path, new_name);
    if (!parent || !destination || parent->type != VnodeType::DIRECTORY || destination->type != VnodeType::DIRECTORY ||
        !parent->ops || !parent->ops->lookup || parent->ops != destination->ops ||
        !parent->ops->rename || !name[0] || !new_name[0]) return -1;
    vnode* source = parent->ops->lookup(parent, name);
    vnode* target = destination->ops->lookup(destination, new_name);
    if (!source || vfs_follow_mount(source) != source || vfs_follow_mount(target) != target) return -1;
    return parent->ops->rename(parent, name, destination, new_name);
}
static int vfs_replace(const char* path, const void* data, uint64_t len) {
    if (!data && len) return -1;
    char name[256]; vnode* parent = vfs_namei_parent(path, name);
    if (!parent || !name[0] || !parent->ops->replace) return -1;
    vnode* target = parent->ops->lookup(parent, name);
    if (vfs_follow_mount(target) != target) return -1;
    return parent->ops->replace(parent, name, data, len);
}
static int vfs_stat(const char* path, file_info* info) {
    return vfs_stat_node(vfs_namei(path), info);
}

static int vfs_rmdir(const char* path) {
    char name[256];
    vnode* parent = vfs_namei_parent(path, name);
    if (!parent || !name[0] || !parent->ops || !parent->ops->rmdir) return -1;
    vnode* covered = parent->ops->lookup ? parent->ops->lookup(parent, name) : nullptr;
    if (covered && vfs_follow_mount(covered) != covered) return -1;
    return parent->ops->rmdir(parent, name);
}

static int vfs_statfs(const char* path, fs_info* out) {
    if (!out) return -1;
    vnode* vn = (path && path[0]) ? vfs_namei(path) : vfs_root;
    if (!vn && vfs_root) vn = vfs_root;
    if (!vn || !vn->ops || !vn->ops->statfs) return -1;
    return vn->ops->statfs(vn, out);
}

#endif // VFS_H
