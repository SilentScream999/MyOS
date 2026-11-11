#ifndef RAMFS_H
#define RAMFS_H

// ── ramfs.h ──────────────────────────────────────────────────────────────────
//
// Phase 5 — Ramdisk Filesystem Backend
//
// An in-memory hierarchical filesystem implementing the generic VFS interfaces.
//
// ──────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include "vfs.h"
#include "heap.h"
#include "helpers.h"
#include "block.h"

struct ramfs_node {
    char name[256];
    VnodeType type;
    
    // Directory fields
    ramfs_node* children;
    ramfs_node* sibling;
    ramfs_node* parent;
    unsigned opens;
    bool deleted;
    
    // File fields
    uint8_t* data;
    uint64_t size;
    uint64_t capacity;
    
    // Abstract interface
    vnode vn;
};

// VFS operations declarations (to populate vfs_ops struct)
static uint64_t ramfs_read(vnode* vn, void* buf, uint64_t len, uint64_t offset);
static uint64_t ramfs_write(vnode* vn, const void* buf, uint64_t len, uint64_t offset);
static vnode* ramfs_lookup(vnode* dir, const char* name);
static vnode* ramfs_create(vnode* dir, const char* name);
static vnode* ramfs_mkdir(vnode* dir, const char* name);
static int ramfs_readdir(vnode* dir, uint64_t index, char* name_out, uint64_t* size_out, VnodeType* type_out);
static int ramfs_unlink(vnode* dir, const char* name);
static int ramfs_rmdir(vnode* dir, const char* name);
static int ramfs_statfs(vnode* vn, fs_info* out);
static int ramfs_truncate(vnode* vn, uint64_t size);
static int ramfs_sync(vnode*) { return 0; }
static int ramfs_open(vnode* vn, file*) {
    storage_irq_guard guard; auto* n = (ramfs_node*)vn->fs_data;
    if (n->deleted) return -1; ++n->opens; return 0;
}
static void ramfs_close(vnode* vn) {
    storage_irq_guard guard; auto* n = (ramfs_node*)vn->fs_data; if (n->opens) --n->opens;
}
static int ramfs_rename(vnode*, const char*, vnode*, const char*);
static int ramfs_replace(vnode*, const char*, const void*, uint64_t);

static vfs_ops ramfs_ops = {
    .read    = ramfs_read,
    .write   = ramfs_write,
    .open    = ramfs_open,
    .close   = ramfs_close,
    .lookup  = ramfs_lookup,
    .create  = ramfs_create,
    .mkdir   = ramfs_mkdir,
    .readdir = ramfs_readdir,
    .unlink  = ramfs_unlink,
    .stat    = nullptr,
    .rmdir   = ramfs_rmdir,
    .statfs  = ramfs_statfs,
    .truncate = ramfs_truncate,
    .sync = ramfs_sync,
    .rename = ramfs_rename,
    .replace = ramfs_replace,
};

// ── RAMFS IMPLEMENTATION ──────────────────────────────────────────────────

// Helper: allocate a new RamFS node
static ramfs_node* ramfs_alloc_node(const char* name, VnodeType type) {
    ramfs_node* node = (ramfs_node*)kmalloc(sizeof(ramfs_node));
    if (!node) return nullptr;
    memset(node, 0, sizeof(*node));
    
    strcpy(node->name, name);
    node->type     = type;
    node->children = nullptr;
    node->sibling  = nullptr;
    node->data     = nullptr;
    node->size     = 0;
    node->capacity = 0;
    
    // Setup VFS vnode mapping
    node->vn.type    = type;
    node->vn.ops     = &ramfs_ops;
    node->vn.fs_data = node;
    node->vn.size    = 0;
    
    return node;
}

// Global root for RamFS
static ramfs_node* ramfs_root_node = nullptr;

static void ramfs_init() {
    ramfs_root_node = ramfs_alloc_node("/", VnodeType::DIRECTORY);
    vfs_mount_root(&ramfs_root_node->vn);
    print((char*)"[ramfs] Root mounted at /");
}

static uint64_t ramfs_read(vnode* vn, void* buf, uint64_t len, uint64_t offset) {
    storage_guard guard;
    if (vn->type != VnodeType::FILE) return 0;
    
    ramfs_node* node = (ramfs_node*)vn->fs_data;
    if (node->deleted) return 0;
    if (offset >= node->size) return 0; // EOF
    
    uint64_t available = node->size - offset;
    uint64_t to_read   = (len < available) ? len : available;
    
    memcpy(buf, node->data + offset, to_read);
    return to_read;
}

static uint64_t ramfs_write(vnode* vn, const void* buf, uint64_t len, uint64_t offset) {
    storage_guard guard;
    if (vn->type != VnodeType::FILE) return 0;
    if (len == 0) return 0;
    
    ramfs_node* node = (ramfs_node*)vn->fs_data;
    if (node->deleted || offset > INT64_MAX || len > INT64_MAX - offset) return 0;
    uint64_t needed_capacity = offset + len;
    
    if (needed_capacity > node->capacity) {
        // Simple reallocation strategy: double capacity or exact needed, whichever is larger.
        uint64_t new_cap = (node->capacity == 0) ? 512 : node->capacity * 2;
        if (new_cap < needed_capacity) new_cap = needed_capacity;
        
        uint8_t* new_data = (uint8_t*)kmalloc(new_cap);
        if (!new_data) return 0; // OOM
        
        if (node->data) {
            memcpy(new_data, node->data, node->size);
            kfree(node->data);
        }
        
        node->data = new_data;
        node->capacity = new_cap;
    }
    
    if (offset > node->size) memset(node->data + node->size, 0, offset - node->size);
    memcpy(node->data + offset, buf, len);
    
    if (offset + len > node->size) {
        node->size = offset + len;
        vn->size   = node->size; // Update generic vnode view
    }
    
    return len;
}

static int ramfs_truncate(vnode* vn, uint64_t size) {
    storage_guard guard;
    if (vn->type != VnodeType::FILE || size > INT64_MAX) return -1;
    auto* n = (ramfs_node*)vn->fs_data;
    if (n->deleted) return -1;
    if (size > n->capacity) {
        auto* data = (uint8_t*)kmalloc(size);
        if (!data) return -1;
        if (n->size) memcpy(data, n->data, n->size);
        if (n->data) kfree(n->data);
        n->data = data; n->capacity = size;
    }
    if (size > n->size) memset(n->data + n->size, 0, size - n->size);
    n->size = vn->size = size;
    return 0;
}

static vnode* ramfs_lookup(vnode* dir, const char* name) {
    storage_guard guard;
    if (dir->type != VnodeType::DIRECTORY) return nullptr;
    
    ramfs_node* d_node = (ramfs_node*)dir->fs_data;
    if (d_node->deleted) return nullptr;
    ramfs_node* child  = d_node->children;
    
    while (child) {
        if (strcmp(child->name, name) == 0) {
            return &child->vn;
        }
        child = child->sibling;
    }
    
    return nullptr;
}

static vnode* ramfs_create(vnode* dir, const char* name) {
    storage_guard guard;
    if (dir->type != VnodeType::DIRECTORY) return nullptr;
    
    ramfs_node* d_node = (ramfs_node*)dir->fs_data;
    
    // Ensure doesn't already exist
    if (ramfs_lookup(dir, name)) return nullptr;
    
    ramfs_node* new_file = ramfs_alloc_node(name, VnodeType::FILE);
    if (!new_file) return nullptr;
    
    new_file->sibling = d_node->children;
    new_file->parent = d_node;
    d_node->children  = new_file;
    
    return &new_file->vn;
}

static vnode* ramfs_mkdir(vnode* dir, const char* name) {
    storage_guard guard;
    if (dir->type != VnodeType::DIRECTORY) return nullptr;
    
    ramfs_node* d_node = (ramfs_node*)dir->fs_data;
    
    if (ramfs_lookup(dir, name)) return nullptr;
    
    ramfs_node* new_dir = ramfs_alloc_node(name, VnodeType::DIRECTORY);
    if (!new_dir) return nullptr;
    
    new_dir->sibling = d_node->children;
    new_dir->parent = d_node;
    d_node->children = new_dir;
    
    return &new_dir->vn;
}

static int ramfs_readdir(vnode* dir, uint64_t index, char* name_out, uint64_t* size_out, VnodeType* type_out) {
    storage_guard guard;
    if (dir->type != VnodeType::DIRECTORY) return -1;
    ramfs_node* d_node = (ramfs_node*)dir->fs_data;
    ramfs_node* child  = d_node->children;
    uint64_t curr = 0;
    while (child) {
        if (curr == index) {
            strcpy(name_out, child->name);
            uint64_t sz = child->size;
            if (child->vn.ops->stat) {
                file_info info;
                if (vfs_stat_node(&child->vn, &info) != 0) return -1;
                sz = info.size;
            }
            if (size_out) *size_out = sz;
            if (type_out) *type_out = child->type;
            return 1;
        }
        curr++;
        child = child->sibling;
    }
    return 0; // EOF
}

static int ramfs_unlink(vnode* dir, const char* name) {
    storage_guard guard;
    if (dir->type != VnodeType::DIRECTORY) return -1;
    ramfs_node* d_node = (ramfs_node*)dir->fs_data;
    ramfs_node* prev = nullptr;
    ramfs_node* curr = d_node->children;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            if (curr->type != VnodeType::FILE || curr->opens) return -1;
            if (prev) prev->sibling = curr->sibling;
            else d_node->children = curr->sibling;
            if (curr->data) kfree(curr->data);
            curr->deleted = true; curr->data = nullptr; curr->size = curr->capacity = curr->vn.size = 0;
            return 0;
        }
        prev = curr;
        curr = curr->sibling;
    }
    return -1;
}

static int ramfs_rename(vnode* dir, const char* name, vnode* dest, const char* new_name) {
    storage_guard guard;
    if (!name[0] || !new_name[0] || strlen(new_name) > 255 || !strcmp(name, ".") || !strcmp(name, "..") ||
        !strcmp(new_name, ".") || !strcmp(new_name, "..")) return -1;
    vnode* source = ramfs_lookup(dir, name);
    if (!source || source->ops != &ramfs_ops) return -1;
    auto* n = (ramfs_node*)source->fs_data;
    if (dir == dest && !strcmp(name, new_name)) return 0;
    if (n->opens || ramfs_lookup(dest, new_name)) return -1;
    auto* parent = (ramfs_node*)dir->fs_data;
    auto* destination = (ramfs_node*)dest->fs_data;
    for (auto* p = destination; p; p = p->parent) if (p == n) return -1;
    auto** link = &parent->children;
    while (*link && *link != n) link = &(*link)->sibling;
    if (!*link) return -1;
    *link = n->sibling; n->sibling = destination->children;
    destination->children = n; n->parent = destination; strcpy(n->name, new_name);
    return 0;
}
static int ramfs_replace(vnode* dir, const char* name, const void* data, uint64_t len) {
    storage_guard guard;
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || len > INT64_MAX) return -1;
    vnode* target = ramfs_lookup(dir, name);
    if (target && (target->type != VnodeType::FILE || target->ops != &ramfs_ops ||
                   ((ramfs_node*)target->fs_data)->opens)) return -1;
    uint8_t* replacement = len ? (uint8_t*)kmalloc(len) : nullptr;
    if (len && !replacement) return -1;
    if (len) memcpy(replacement, data, len);
    if (!target) target = ramfs_create(dir, name);
    if (!target) { if (replacement) kfree(replacement); return -1; }
    auto* n = (ramfs_node*)target->fs_data;
    auto* old = n->data;
    n->data = replacement; n->capacity = n->size = target->size = len;
    if (old) kfree(old);
    return 0;
}
static int ramfs_rmdir(vnode* dir, const char* name) {
    storage_guard guard;
    if (dir->type != VnodeType::DIRECTORY) return -1;
    ramfs_node* d_node = (ramfs_node*)dir->fs_data;
    ramfs_node* prev = nullptr;
    ramfs_node* curr = d_node->children;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            if (curr->type != VnodeType::DIRECTORY) return -1;
            if (curr->children != nullptr || curr->opens) return -1; // Directory is not empty/in use
            if (prev) prev->sibling = curr->sibling;
            else d_node->children = curr->sibling;
            curr->deleted = true;
            return 0;
        }
        prev = curr;
        curr = curr->sibling;
    }
    return -1;
}

static int ramfs_statfs(vnode*, fs_info* out) {
    storage_guard guard;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->block_size = 4096;
    out->total_blocks = 16384; // 64 MiB RAM disk representation
    out->free_blocks  = 16384;
    out->total_bytes  = (uint64_t)out->total_blocks * out->block_size;
    out->free_bytes   = (uint64_t)out->free_blocks * out->block_size;
    strcpy(out->fs_type, "RAMFS");
    return 0;
}

static uint64_t klog_vfs_read(vnode* vn, void* buf, uint64_t len, uint64_t offset) {
    if (offset >= g_klog_pos) return 0;
    uint64_t avail = g_klog_pos - offset;
    uint64_t to_read = (len < avail) ? len : avail;
    memcpy(buf, g_klog_buffer + offset, to_read);
    return to_read;
}
static int klog_vfs_stat(vnode*, file_info* info) {
    info->size = g_klog_pos;
    return 0;
}

static vfs_ops klog_vfs_ops = {
    .read   = klog_vfs_read,
    .write  = nullptr, // bootlog is read-only
    .open   = nullptr,
    .close  = nullptr,
    .lookup = nullptr,
    .create = nullptr,
    .mkdir  = nullptr,
    .stat   = klog_vfs_stat,
};

static void ramfs_mount_klog() {
    if (!ramfs_root_node) return;
    ramfs_node* klog_node = ramfs_alloc_node("bootlog.txt", VnodeType::FILE);
    if (klog_node) {
        klog_node->vn.ops = &klog_vfs_ops;
        klog_node->sibling = ramfs_root_node->children;
        ramfs_root_node->children = klog_node;
        print((char*)"[ramfs] /bootlog.txt mounted.");
    }
}

#endif // RAMFS_H
