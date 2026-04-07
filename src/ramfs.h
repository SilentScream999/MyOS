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

struct ramfs_node {
    char name[256];
    VnodeType type;
    
    // Directory fields
    ramfs_node* children;
    ramfs_node* sibling;
    
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

static vfs_ops ramfs_ops = {
    .read   = ramfs_read,
    .write  = ramfs_write,
    .open   = nullptr,
    .close  = nullptr,
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir  = ramfs_mkdir,
};

// ── RAMFS IMPLEMENTATION ──────────────────────────────────────────────────

// Helper: allocate a new RamFS node
static ramfs_node* ramfs_alloc_node(const char* name, VnodeType type) {
    ramfs_node* node = (ramfs_node*)kmalloc(sizeof(ramfs_node));
    if (!node) return nullptr;
    
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
    if (vn->type != VnodeType::FILE) return 0;
    
    ramfs_node* node = (ramfs_node*)vn->fs_data;
    if (offset >= node->size) return 0; // EOF
    
    uint64_t available = node->size - offset;
    uint64_t to_read   = (len < available) ? len : available;
    
    memcpy(buf, node->data + offset, to_read);
    return to_read;
}

static uint64_t ramfs_write(vnode* vn, const void* buf, uint64_t len, uint64_t offset) {
    if (vn->type != VnodeType::FILE) return 0;
    if (len == 0) return 0;
    
    ramfs_node* node = (ramfs_node*)vn->fs_data;
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
    
    memcpy(node->data + offset, buf, len);
    
    if (offset + len > node->size) {
        node->size = offset + len;
        vn->size   = node->size; // Update generic vnode view
    }
    
    return len;
}

static vnode* ramfs_lookup(vnode* dir, const char* name) {
    if (dir->type != VnodeType::DIRECTORY) return nullptr;
    
    ramfs_node* d_node = (ramfs_node*)dir->fs_data;
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
    if (dir->type != VnodeType::DIRECTORY) return nullptr;
    
    ramfs_node* d_node = (ramfs_node*)dir->fs_data;
    
    // Ensure doesn't already exist
    if (ramfs_lookup(dir, name)) return nullptr;
    
    ramfs_node* new_file = ramfs_alloc_node(name, VnodeType::FILE);
    if (!new_file) return nullptr;
    
    new_file->sibling = d_node->children;
    d_node->children  = new_file;
    
    return &new_file->vn;
}

static vnode* ramfs_mkdir(vnode* dir, const char* name) {
    if (dir->type != VnodeType::DIRECTORY) return nullptr;
    
    ramfs_node* d_node = (ramfs_node*)dir->fs_data;
    
    if (ramfs_lookup(dir, name)) return nullptr;
    
    ramfs_node* new_dir = ramfs_alloc_node(name, VnodeType::DIRECTORY);
    if (!new_dir) return nullptr;
    
    new_dir->sibling = d_node->children;
    d_node->children = new_dir;
    
    return &new_dir->vn;
}

static uint64_t klog_vfs_read(vnode* vn, void* buf, uint64_t len, uint64_t offset) {
    if (offset >= g_klog_pos) return 0;
    uint64_t avail = g_klog_pos - offset;
    uint64_t to_read = (len < avail) ? len : avail;
    memcpy(buf, g_klog_buffer + offset, to_read);
    return to_read;
}

static vfs_ops klog_vfs_ops = {
    .read   = klog_vfs_read,
    .write  = nullptr, // bootlog is read-only
    .open   = nullptr,
    .close  = nullptr,
    .lookup = nullptr,
    .create = nullptr,
    .mkdir  = nullptr,
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
