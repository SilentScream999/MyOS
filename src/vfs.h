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
        comp[i++] = *path++;
    }
    comp[i] = '\0';
    return path;
}

// Resolve a path to a vnode
static vnode* vfs_namei(const char* path) {
    if (!vfs_root) return nullptr;
    
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
        if (comp[0] == '\0') break; // end of path
        
        if (current->type != VnodeType::DIRECTORY || !current->ops->lookup) {
            return nullptr; // Cannot traverse non-directory
        }
        
        vnode* next = current->ops->lookup(current, comp);
        if (!next) return nullptr; // Not found
        
        current = next;
        p = next_p;
    }
    
    return current;
}

// Find the parent directory of a path, and return the final name component
// e.g., "/mnt/foo/bar.txt" -> returns vnode for "/mnt/foo" and sets name="bar.txt"
static vnode* vfs_namei_parent(const char* path, char* name) {
    if (!vfs_root) return nullptr;
    
    vnode* current = vfs_root;
    char comp[256];
    char next_comp[256];
    
    const char* p = path;
    if (*p == '/') p++;
    else return nullptr; // Only absolute paths for now
    
    p = vfs_next_component(p, comp);
    if (comp[0] == '\0') {
        name[0] = '\0';
        return vfs_root; // Path is just "/"
    }
    
    while (true) {
        const char* next_p = vfs_next_component(p, next_comp);
        if (next_comp[0] == '\0') {
            // comp is the final component!
            strcpy(name, comp);
            return current;
        }
        
        // comp is a directory to traverse
        if (current->type != VnodeType::DIRECTORY || !current->ops->lookup) return nullptr;
        
        current = current->ops->lookup(current, comp);
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
        // File doesn't exist, try to create it (if flags imply O_CREAT)
        // For simplicity now, we'll try to create it automatically if it doesn't exist.
        // In a real OS, we'd check O_CREAT flag.
        char name[256];
        vnode* parent = vfs_namei_parent(path, name);
        if (parent && parent->ops->create) {
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
    uint64_t read_bytes = f->vn->ops->read(f->vn, buf, len, f->offset);
    f->offset += read_bytes;
    return read_bytes;
}

static uint64_t vfs_write(file* f, const void* buf, uint64_t len) {
    if (!f || !f->vn || !f->vn->ops->write) return 0;
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

#endif // VFS_H
