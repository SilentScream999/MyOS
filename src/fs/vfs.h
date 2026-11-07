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
#include "helpers.h"
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
static vnode* vfs_disk_mountpoint = nullptr;
static vnode* vfs_disk_root = nullptr;
static vnode* vfs_follow_mount(vnode* vn) {
    return vn && vn == vfs_disk_mountpoint ? vfs_disk_root : vn;
}

// ── VFS GLOBAL API ────────────────────────────────────────────────────────

static inline void vfs_mount_root(vnode* root_vnode) {
    vfs_root = root_vnode;
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

// Resolve a path to a vnode
static vnode* vfs_namei(const char* path) {
    if (!vfs_root || !path) return nullptr;
    
    vnode* current = vfs_root;
    char comp[256];
    
    const char* p = path;
    if (*p == '/') {
        p++; // Absolute path
    } else {
        // Relative paths not supported yet without CWD in process
        return nullptr;
    }
    
    while (true) {
        const char* next_p = vfs_next_component(p, comp);
        if (!next_p && comp[0]) return nullptr;
        if (comp[0] == '\0') break; // end of path
        
        if (current->type != VnodeType::DIRECTORY || !current->ops->lookup) {
            return nullptr; // Cannot traverse non-directory
        }
        
        vnode* next = current->ops->lookup(current, comp);
        if (!next) return nullptr; // Not found
        
        current = vfs_follow_mount(next);
        p = next_p;
    }
    
    return current;
}

// Find the parent directory of a path, and return the final name component
// e.g., "/mnt/foo/bar.txt" -> returns vnode for "/mnt/foo" and sets name="bar.txt"
static vnode* vfs_namei_parent(const char* path, char* name) {
    if (!vfs_root || !path) return nullptr;
    
    vnode* current = vfs_root;
    char comp[256];
    char next_comp[256];
    
    const char* p = path;
    if (*p == '/') p++;
    else return nullptr; // Only absolute paths for now
    
    p = vfs_next_component(p, comp);
    if (!p && comp[0]) return nullptr;
    if (comp[0] == '\0') {
        name[0] = '\0';
        return vfs_root; // Path is just "/"
    }
    
    while (true) {
        const char* next_p = vfs_next_component(p, next_comp);
        if (!next_p && next_comp[0]) return nullptr;
        if (next_comp[0] == '\0') {
            // comp is the final component!
            strcpy(name, comp);
            return current;
        }
        
        // comp is a directory to traverse
        if (current->type != VnodeType::DIRECTORY || !current->ops->lookup) return nullptr;
        
        current = vfs_follow_mount(current->ops->lookup(current, comp));
        if (!current) return nullptr;
        
        strcpy(comp, next_comp);
        p = next_p;
    }
    
    strcpy(name, comp);
    return current;
}

// ── HIGH-LEVEL VFS FUNCTIONS ────────────────────────────────────────────────

// Open or create a file
static int vfs_open(const char* path, int flags, file* f) {
    vnode* vn = vfs_namei(path);
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
    
    if (vn->ops->open) {
        return vn->ops->open(vn, f);
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

static int vfs_mkdir(const char* path) {
    char name[256];
    vnode* parent = vfs_namei_parent(path, name);
    if (!parent || !parent->ops || !parent->ops->mkdir) return -1;
    vnode* vn = parent->ops->mkdir(parent, name);
    return vn ? 0 : -1;
}

static int vfs_mkdir_p(const char* path) {
    if (!vfs_root || !path || *path != '/') return -1;
    vnode* current = vfs_root;
    char comp[256];
    const char* p = path + 1;
    while (p) {
        p = vfs_next_component(p, comp);
        if (!p && comp[0]) return -1;
        if (comp[0] == '\0') break;
        if (current->type != VnodeType::DIRECTORY || !current->ops || !current->ops->lookup) return -1;
        vnode* next = current->ops->lookup(current, comp);
        if (!next) {
            if (!current->ops->mkdir) return -1;
            next = current->ops->mkdir(current, comp);
            if (!next) return -1;
        }
        current = vfs_follow_mount(next);
    }
    return 0;
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

static int vfs_readdir(const char* path, uint64_t index, char* name_out, uint64_t* size_out, VnodeType* type_out) {
    vnode* vn = vfs_namei(path);
    if (!vn || vn->type != VnodeType::DIRECTORY || !vn->ops || !vn->ops->readdir) return -1;
    return vn->ops->readdir(vn, index, name_out, size_out, type_out);
}

static int vfs_unlink(const char* path) {
    char name[256];
    vnode* parent = vfs_namei_parent(path, name);
    if (!parent || !name[0] || !parent->ops || !parent->ops->unlink) return -1;
    return parent->ops->unlink(parent, name);
}
static int vfs_stat(const char* path, file_info* info) {
    if (!info) return -1;
    vnode* vn = vfs_namei(path);
    if (!vn) return -1;
    memset(info, 0, sizeof(*info));
    info->size = vn->size;
    info->type = vn->type == VnodeType::FILE ? 1 : vn->type == VnodeType::DIRECTORY ? 2 : 3;
    return vn->ops->stat ? vn->ops->stat(vn, info) : 0;
}

#endif // VFS_H
