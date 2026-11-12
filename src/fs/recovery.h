#ifndef WYZAR_RECOVERY_H
#define WYZAR_RECOVERY_H
#include "ramfs.h"

// Boot-only retirement of an unreachable recovery tree. Runtime mount roots are
// detached and preserved, including all their children and any open handles.
// Unlike ordinary unlink, no lookup/descriptor may retain an unprotected vnode.
namespace recovery {
struct Released { uint64_t nodes, bytes; };
static bool retained(ramfs_node* node, vnode* const* keep, unsigned count) {
    for (unsigned i=0;i<count;++i) if (&node->vn==keep[i]) return true;
    return false;
}
static bool idle(ramfs_node* node, vnode* const* keep, unsigned count, unsigned depth) {
    if (retained(node,keep,count)) return true;
    if (node->opens || depth>128) return false;
    for (auto* child=node->children;child;child=child->sibling)
        if (!idle(child,keep,count,depth+1)) return false;
    return true;
}
static void destroy(ramfs_node* node, vnode* const* keep, unsigned count, Released& released) {
    if (retained(node,keep,count)) { node->parent=nullptr; node->sibling=nullptr; return; }
    for (auto* child=node->children;child;) {
        auto* next=child->sibling;
        destroy(child,keep,count,released); child=next;
    }
    released.bytes+=sizeof(*node)+node->capacity; ++released.nodes;
    kfree(node->data); kfree(node);
}
static bool release(vnode** root, vnode* const* keep, unsigned count, Released* out) {
    storage_guard guard;
    if (!root || !*root || !out || *root==vfs_root || (*root)->ops!=&ramfs_ops ||
        !keep || !count) return false;
    auto* node=(ramfs_node*)(*root)->fs_data;
    if (retained(node,keep,count) || !idle(node,keep,count,0)) return false;
    *out={};
    if (ramfs_root_node==node) ramfs_root_node=nullptr;
    *root=nullptr;
    destroy(node,keep,count,*out); return true;
}
}
#endif
