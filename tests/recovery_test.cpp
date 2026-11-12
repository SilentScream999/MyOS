#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <map>
#include <vector>
#define helpers_h
#define heap_h
#define STORAGE_HOST_TEST
static std::map<void*,size_t> allocations;
static void* kmalloc(size_t size) {
    void* p=std::malloc(size); assert(p); std::memset(p,0xa5,size); allocations[p]=size; return p;
}
static void kfree(void* p) { if(p) { assert(allocations.erase(p)==1); std::free(p); } }
static void print(char*) {}
char g_klog_buffer[65536];
uint64_t g_klog_pos;
#include "../src/fs/recovery.h"
#include "../src/arch/boot_module.h"
static std::vector<uint64_t> freed;
static void free_page(uint64_t page) { freed.push_back(page); }

int main() {
    ramfs_init(); ramfs_mount_klog();
    vnode* original=vfs_root;
    vfs_mkdir("/tmp"); vfs_mkdir("/run"); vfs_mkdir("/bin"); vfs_mkdir("/share"); vfs_mkdir("/share/icons");
    assert(vfs_replace("/bin/init.elf","recovery ELF",12)==0);
    assert(vfs_replace("/share/icons/icon.png","PNG data",8)==0);
    assert(vfs_replace("/run/status","preserved",9)==0);
    vnode* keep[]={vfs_namei("/tmp"),vfs_namei("/run"),vfs_namei("/bootlog.txt")};
    file held{},runtime{};
    assert(vfs_open("/bin/init.elf",0,&held)==0);
    assert(vfs_open("/run/status",0,&runtime)==0);
    recovery::Released report{};
    assert(!recovery::release(&original,keep,3,&report)); // Never free the active recovery root.
    auto* disk=ramfs_alloc_node("disk",VnodeType::DIRECTORY);
    vfs_mount_root(&disk->vn);
    vfs_mkdir("/tmp"); vfs_mkdir("/run");
    assert(vfs_mount(vfs_namei("/tmp"),keep[0])==0);
    assert(vfs_mount(vfs_namei("/run"),keep[1])==0);
    assert(!recovery::release(&original,keep,3,&report)); // Open recovery executable blocks all reclamation.
    assert(original && !((ramfs_node*)held.vn->fs_data)->deleted);
    vfs_close(&held);
    size_t before=allocations.size();
    assert(recovery::release(&original,keep,3,&report));
    assert(!original && !ramfs_root_node && report.nodes==6 && report.bytes>12+8);
    assert(allocations.size()<before);
    for(vnode* root:keep) {
        auto* node=(ramfs_node*)root->fs_data;
        assert(!node->parent && !node->sibling);
    }
    char data[16]{};
    assert(vfs_read(&runtime,data,sizeof(data))==9 && !std::memcmp(data,"preserved",9));
    vfs_close(&runtime);
    assert(vfs_replace("/tmp/test","alive",5)==0);
    assert(vfs_open("/tmp/test",0,&runtime)==0);
    assert(vfs_read(&runtime,data,sizeof(data))==5 && !std::memcmp(data,"alive",5)); vfs_close(&runtime);
    std::memcpy(g_klog_buffer,"log",3); g_klog_pos=3;
    assert(keep[2]->ops->read(keep[2],data,sizeof(data),0)==3 && !std::memcmp(data,"log",3));
    assert(!recovery::release(&original,keep,3,&report)); // Idempotent.
    recovery::destroy(disk,nullptr,0,report);
    for(vnode* root:keep) recovery::destroy((ramfs_node*)root->fs_data,nullptr,0,report);
    vfs_root=nullptr; vfs_mount_count=0; assert(allocations.empty());

    constexpr uint64_t hhdm=0xffff800000000000ull;
    limine_file module{},other{};
    module.address=(void*)(hhdm+0x402100); module.size=0x3100;
    other.address=(void*)(hhdm+0x405300); other.size=0x100;
    limine_file* modules[]={&module,&other};
    limine_module_response list{}; list.module_count=2; list.modules=modules;
    limine_memmap_entry region{0x400000,0x10000,6};
    limine_memmap_entry reserved{0x404000,4096,1};
    limine_memmap_entry* regions[]={&region,&reserved};
    limine_memmap_response map{}; map.entry_count=2; map.entries=regions;
    assert(boot_module_release(&module,&list,&map,hhdm,0x403800,0x200,free_page)==4096);
    assert(freed==std::vector<uint64_t>{0x402000});
    assert(!module.address && !module.size && other.address);
    assert(boot_module_release(&module,&list,&map,hhdm,0x403800,0x200,free_page)==0);
    module.address=(void*)(hhdm+0x402100); module.size=0x3100;
    map.entry_count=1; list.module_count=1; freed.clear();
    assert(boot_module_release(&module,&list,&map,hhdm,0x100000,0x100000,free_page)==4*4096);
    assert((freed==std::vector<uint64_t>{0x402000,0x403000,0x404000,0x405000}));
    module.address=(void*)(hhdm+0x402100); module.size=0x3100; region.type=0; freed.clear();
    assert(boot_module_release(&module,&list,&map,hhdm,0x100000,0x100000,free_page)==0 && freed.empty());
    module.address=(void*)0x100; module.size=4096;
    assert(boot_module_release(&module,&list,&map,hhdm,0x100000,0x100000,free_page)==0 && module.address);
    module.address=(void*)(hhdm+0x400000); module.size=UINT64_MAX;
    assert(boot_module_release(&module,&list,&map,hhdm,0x100000,0x100000,free_page)==0 && module.address);
}
