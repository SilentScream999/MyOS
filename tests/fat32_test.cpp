// Exercise the kernel adapter against a real FAT32 layout and injected I/O errors.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <array>
#include <string>
#define STORAGE_HOST_TEST
static void* kmalloc(size_t size) { return calloc(1, size); }
static void kfree(void* pointer) { free(pointer); }
static void print(char*) {}
#include "../src/fs/fat32_vfs.h"
static file_datetime test_now{2026,11,18,14,32,59,0};
extern "C" DWORD get_fattime() { return fat_encode_time(test_now); }

static std::vector<uint8_t> image(70000 * 512);
static std::map<uint64_t,std::array<uint8_t,512>> sparse_sectors;
static std::array<uint8_t,512> mbr;
static bool fail_write, fail_flush;
static int64_t writes_left=-1;
static uint64_t sector_writes=0;
static bool read_sector(block_device* disk, uint64_t sector, void* out) {
    if(disk->context){
        if(!sector){memcpy(out,mbr.data(),512);return true;}
        uint64_t start=*(uint64_t*)disk->context;
        if(sector<start){memset(out,0,512);return true;}sector-=start;
    }
    if(sector<image.size()/512)memcpy(out,image.data()+sector*512,512);
    else {auto found=sparse_sectors.find(sector);if(found==sparse_sectors.end())memset(out,0,512);else memcpy(out,found->second.data(),512);}
    return true;
}
static bool write_sector(block_device* disk, uint64_t sector, const void* in) {
    if (fail_write) return false;
    ++sector_writes;
    if(writes_left==0)return false;
    if(writes_left>0)--writes_left;
    if(disk->context){uint64_t start=*(uint64_t*)disk->context;assert(sector>=start);sector-=start;}
    if(sector<image.size()/512)memcpy(image.data()+sector*512,in,512);
    else memcpy(sparse_sectors[sector].data(),in,512);
    return true;
}
static bool flush(block_device*) { return !fail_flush; }
static void put32(size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) image[offset + i] = value >> (i * 8);
}
// Small namespace fixture with independently mounted external volumes.
static vnode namespace_root, namespace_mnt, mount_points[8];
static bool mount_present[8];
static bool mount_has_content[8];
static char mount_names[8][128];
static vfs_ops namespace_ops;
static vnode* namespace_lookup(vnode* dir, const char* name) {
    if(dir==&namespace_root)return strcmp(name,"mnt")==0?&namespace_mnt:nullptr;
    if(dir!=&namespace_mnt)return nullptr;
    for(unsigned i=0;i<8;++i)if(mount_present[i]&&strcmp(mount_names[i],name)==0)return &mount_points[i];
    return nullptr;
}
static vnode* namespace_mkdir(vnode* dir,const char* name) {
    if(dir!=&namespace_mnt||strlen(name)>=128)return nullptr;
    for(unsigned i=0;i<8;++i)if(!mount_present[i]){
        mount_present[i]=true;strcpy(mount_names[i],name);return &mount_points[i];
    }
    return nullptr;
}
static int namespace_rmdir(vnode* dir,const char* name) {
    vnode* point=namespace_lookup(dir,name);if(!point)return -1;
    if(mount_has_content[point-mount_points])return -1;
    mount_present[point-mount_points]=false;return 0;
}
static int namespace_readdir(vnode* dir,uint64_t index,char* name,uint64_t* size,VnodeType* type) {
    if(dir!=&namespace_mnt){
        for(unsigned i=0;i<8;++i)if(dir==&mount_points[i]){
            if(index||!mount_has_content[i])return 0;
            strcpy(name,"keep.txt");if(size)*size=4;if(type)*type=VnodeType::FILE;return 1;
        }
        return -1;
    }
    for(unsigned i=0;i<8;++i)if(mount_present[i]){
        if(index--!=0)continue;
        strcpy(name,mount_names[i]);
        if(size)*size=0;if(type)*type=VnodeType::DIRECTORY;return 1;
    }
    return 0;
}
int main(int argc,char** argv) {
    bool exfat=argc>1&&strcmp(argv[1],"exfat")==0;
    image[0] = 0xeb; image[1] = 0x58; image[2] = 0x90;
    memcpy(image.data() + 3, "MSDOS5.0", 8);
    image[11] = 0; image[12] = 2; image[13] = 1; image[14] = 32;
    image[16] = 2; image[21] = 0xf8;
    put32(32, 70000); put32(36, 600); put32(44, 2);
    image[48] = 1; image[50] = 6; image[66] = 0x29;
    memcpy(image.data() + 82, "FAT32   ", 8);
    image[510] = 0x55; image[511] = 0xaa;
    put32(512, 0x41615252); put32(512 + 484, 0x61417272);
    put32(512 + 488, 68767); put32(512 + 492, 3); put32(512 + 508, 0xaa550000);
    for (unsigned fat : {32u, 632u}) {
        put32(fat * 512, 0x0ffffff8); put32(fat * 512 + 4, 0xffffffff); put32(fat * 512 + 8, 0x0fffffff);
    }
    block_device disk{nullptr, 70000, true, read_sector, write_sector, flush, nullptr, nullptr};
    if(exfat)disk.sectors=16*1024*1024;
    fat_volume_count = 1; auto& v = fat_volumes[0]; v.disk = &disk; v.sectors = disk.sectors;
    strcpy(v.drive, "0:"); VolToPart[0] = {0, 0}; fat_init_ops();
    if(exfat){
        std::vector<uint8_t> work(65536);MKFS_PARM parameters{};parameters.fmt=FM_EXFAT|FM_SFD;parameters.au_size=65536;
        assert(f_mkfs("0:",&parameters,work.data(),work.size())==FR_OK);
    }
    assert(f_mount(&v.fs, v.drive, 1) == FR_OK && v.fs.fs_type == (exfat?FS_EXFAT:FS_FAT32));
    assert(f_setlabel("0:TRAVEL")==FR_OK);
    char drive_name[64];fat_drive_name(&v,"",drive_name);assert(strncmp(drive_name,"USB Drive (",11)==0);
    fat_drive_name(&v,"..TRAVEL/DRIVE",drive_name);assert(strcmp(drive_name,"TRAVEL_DRIVE")==0);
    v.root.volume = &v; v.root.vn = {VnodeType::DIRECTORY, &fat_ops, &v.root, 0};
    vfs_mount_root(&v.root.vn);
    assert(vfs_mkdir("/Transfers") == 0);
    FIL orphan{};assert(f_open(&orphan,"0:/.explorer-copy-777.tmp",FA_WRITE|FA_CREATE_NEW)==FR_OK);
    assert(f_close(&orphan)==FR_OK);
    file live{};assert(vfs_open("/.explorer-copy-778.tmp",0xc2,&live)==0);
    vfs_close(&live);
    assert(vfs_open("/.explorer-copy-user.tmp",0xc2,&live)==0);vfs_close(&live);
    // Reopen the active stage, then browse for the first time.
    assert(vfs_open("/.explorer-copy-778.tmp",0,&live)==0);
    char cleanup_name[256];assert(vfs_readdir("/",0,cleanup_name,nullptr,nullptr)==1);
    assert(!vfs_namei("/.explorer-copy-777.tmp"));
    assert(vfs_namei("/.explorer-copy-778.tmp")&&vfs_namei("/.explorer-copy-user.tmp"));vfs_close(&live);
    file f{}; const char* path = "/Transfers/Long filename caf\xc3\xa9.txt";
    assert(vfs_open(path, 0xc2, &f) == 0);
    std::vector<char> content(150001);
    for (size_t i = 0; i < content.size(); ++i) content[i] = (char)(i * 17);
    assert(vfs_write(&f, content.data(), content.size()) == content.size());
    assert(vfs_fsync(&f) == 0); vfs_close(&f);
    assert(vfs_open(path, 0, &f) == 0);
    std::vector<char> out(content.size()); assert(vfs_read(&f, out.data(), out.size()) == out.size());
    assert(out == content); vfs_close(&f);
    file_info info{}; assert(vfs_stat(path, &info) == 0 && info.size == content.size());
    assert(info.created.year==2026&&info.created.month==11&&info.created.day==18);
    assert(info.modified.hour==14&&info.modified.minute==32&&info.modified.second==58);
    assert(fat_encode_time({})==((1u<<21)|(1u<<16)));
    if(exfat){
        // Build a valid >4 GiB fixture without writing the intervening data.
        FIL large{};UINT written=0;uint64_t offset=0x100001000ULL;
        assert(f_open(&large,"0:/large.bin",FA_WRITE|FA_CREATE_NEW)==FR_OK);
        assert(f_lseek(&large,offset)==FR_OK&&f_tell(&large)==offset);
        assert(f_write(&large,"end",3,&written)==FR_OK&&written==3&&f_close(&large)==FR_OK);
        assert(vfs_stat("/large.bin",&info)==0&&info.size==offset+3);
        file large_file{};char tail[4]={};assert(vfs_open("/large.bin",2,&large_file)==0);
        large_file.offset=offset;assert(vfs_read(&large_file,tail,3)==3&&memcmp(tail,"end",3)==0);
        assert(vfs_write(&large_file,"!",1)==1&&vfs_fsync(&large_file)==0);
        vfs_close(&large_file);assert(vfs_unlink("/large.bin")==0);
    }
    char name[256]; uint64_t size; VnodeType type;
    assert(vfs_readdir("/Transfers", 0, name, &size, &type) == 1);
    assert(strcmp(name, "Long filename caf\xc3\xa9.txt") == 0);
    assert(vfs_readdir("/Transfers", 1, name, &size, &type) == 0);
    assert(vfs_rename(path, "/Transfers/copied.txt") == 0);
    assert(vfs_open("/Transfers/COPIED.TXT", 2, &f) == 0);
    assert(vfs_ftruncate(&f, 3) == 0); assert(vfs_ftruncate(&f, 4) == 0);
    char zeros[4] = {1,1,1,1};
    assert(fat_read(f.vn, zeros, 4, 0) == 4 && zeros[3] == 0);
    if(!exfat)assert(fat_write(f.vn, "x", 1, 0xffffffff) == 0); vfs_close(&f);
    assert(vfs_mkdir("/Other") == 0);
    assert(vfs_rename("/Transfers", "/Other/Renamed") == 0);
    assert(vfs_stat("/Other/Renamed/copied.txt", &info) == 0 && info.size == 4);
    fs_info capacity{}; assert(vfs_statfs("/", &capacity) == 0);
    assert(strcmp(capacity.fs_type, exfat?"exFAT":"FAT32") == 0 && capacity.free_bytes < capacity.total_bytes);
    assert(vfs_rmdir("/Other/Renamed") == -1);
    assert(vfs_unlink("/Other/Renamed/copied.txt") == 0);
    assert(vfs_rmdir("/Other/Renamed") == 0);
    assert(vfs_replace("/notes.txt",nullptr,0)==0);
    assert(vfs_replace("/notes.txt","typed in Notepad",16)==0);
    file note{};char note_text[32]={};assert(vfs_open("/notes.txt",0,&note)==0);
    assert(vfs_replace("/notes.txt","blocked",7)==-1);
    assert(vfs_read(&note,note_text,32)==16&&memcmp(note_text,"typed in Notepad",16)==0);vfs_close(&note);
    assert(vfs_replace("/notes.txt","updated",7)==0);
    assert(vfs_replace("/Save caf\xc3\xa9.txt","Save As",7)==0);
    assert(vfs_replace("/Other","invalid",7)==-1);
    auto before_save=image;auto before_sparse=sparse_sectors;
    std::string old_text="updated",new_text="new complete text";
    uint64_t first_write=sector_writes;
    assert(vfs_replace("/notes.txt",new_text.data(),new_text.size())==0);
    unsigned save_writes=(unsigned)(sector_writes-first_write);assert(save_writes>0&&save_writes<1024);
    auto remount=[&](){
        assert(f_mount(nullptr,"0:",0)==FR_OK);v.failed=false;
        v.root.save_recovery_done=false;v.root.cleanup_done=false;
        assert(f_mount(&v.fs,"0:",1)==FR_OK);
    };
    // Fail at each sector-write boundary, then remount without committing the
    // old cache. Recovery must expose a complete old or complete new document.
    for(unsigned failure=0;failure<save_writes;++failure){
        image=before_save;sparse_sectors=before_sparse;remount();writes_left=failure;
        int result=vfs_replace("/notes.txt",new_text.data(),new_text.size());writes_left=-1;remount();
        file recovered{};char recovered_text[64]={};assert(vfs_open("/notes.txt",0,&recovered)==0);
        uint64_t count=vfs_read(&recovered,recovered_text,sizeof(recovered_text));vfs_close(&recovered);
        std::string recovered_value(recovered_text,count);
        assert(recovered_value==old_text||recovered_value==new_text);
        if(result==0)assert(recovered_value==new_text);
    }
    printf("Replacement recovery checked at %u sector-write boundaries\n",save_writes);
    image=before_save;sparse_sectors=before_sparse;remount();
    assert(vfs_open("/failure.txt", 0xc2, &f) == 0);
    fail_write = true; assert(vfs_write(&f, "test", 4) == 0);
    assert(v.failed && vfs_stat("/Other", &info) == -1);
    fail_write = false; v.failed = false; fail_flush = true;
    assert(fat_sync(&v.root.vn) == -1 && v.failed);
    disk.online = false; assert(disk_status(0) == STA_NOINIT);
    namespace_ops.lookup=namespace_lookup;namespace_ops.mkdir=namespace_mkdir;
    namespace_ops.rmdir=namespace_rmdir;namespace_ops.readdir=namespace_readdir;
    namespace_root={VnodeType::DIRECTORY,&namespace_ops,nullptr,0};namespace_mnt=namespace_root;
    for(auto& point:mount_points)point=namespace_root;
    mount_present[0]=true;strcpy(mount_names[0],"usb1");vfs_mount_root(&namespace_root);
    assert(vfs_mount(&mount_points[0],&v.root.vn)==0);
    v.mount_point=&mount_points[0];strcpy(v.mount_path,"/mnt/usb1");
    // One failed child must not make the parent namespace unreadable.
    assert(vfs_readdir("/mnt",0,name,&size,&type)==1&&strcmp(name,"usb1")==0);
    fat_retire_unavailable();
    assert(vfs_readdir("/mnt",0,name,&size,&type)==0&&vfs_mount_count==0);
    assert(!vfs_namei("/mnt/usb1"));
    vnode* existing_folder=namespace_mkdir(&namespace_mnt,"TRAVEL");assert(existing_folder);
    mount_has_content[existing_folder-mount_points]=true;
    fail_flush=false;block_device replacement=disk;replacement.online=true;
    fat_probe_disk(&replacement);
    std::string mounted=fat_volumes[1].mount_path;
    assert(mounted=="/mnt/TRAVEL (2)"&&vfs_namei(mounted.c_str())==&fat_volumes[1].root.vn);
    mount_has_content[existing_folder-mount_points]=false;
    assert(namespace_rmdir(&namespace_mnt,"TRAVEL")==0);
    assert(vfs_mkdir((mounted+"/Reconnected").c_str())==0);
    assert(vfs_replace((mounted+"/Notepad save.txt").c_str(),"first save",10)==0);
    assert(vfs_replace((mounted+"/Notepad save.txt").c_str(),"saved edits",11)==0);
    assert(vfs_readdir("/mnt",0,name,&size,&type)==1);
    assert(vfs_read(&f,out.data(),1)==0); // Old handles stay retired.
    vfs_close(&f);
    file busy{};assert(vfs_open((mounted+"/busy.txt").c_str(),0xc2,&busy)==0);
    assert(vfs_eject(mounted.c_str())==-2&&replacement.online);vfs_close(&busy);
    replacement.flush_unsupported=exfat;
    assert(vfs_eject(mounted.c_str())== (exfat?1:0)&&!replacement.online);
    assert(vfs_readdir("/mnt",0,name,&size,&type)==0);
    block_device flush_failure=disk;flush_failure.online=true;
    assert(fat_mount_slice(&flush_failure,0,flush_failure.sectors));fail_flush=true;
    std::string failed_mount=fat_volumes[2].mount_path;
    assert(vfs_eject(failed_mount.c_str())==-3&&flush_failure.online);
    fat_retire_unavailable();assert(!vfs_namei(failed_mount.c_str()));
    fail_flush=false;uint64_t partition_start=2048;
    mbr[0]=0x33;mbr[510]=0x55;mbr[511]=0xaa;mbr[450]=exfat?0x07:0x0c;
    wy_storage_p32(mbr.data()+454,partition_start);wy_storage_p32(mbr.data()+458,disk.sectors);
    auto saved_mbr=mbr;
    block_device partitioned{&partition_start,disk.sectors+partition_start,true,read_sector,write_sector,flush,nullptr,nullptr};
    fat_probe_disk(&partitioned);assert(vfs_namei("/mnt/TRAVEL"));
    file partition_file{};assert(vfs_open("/mnt/TRAVEL/partition.txt",0xc2,&partition_file)==0);
    assert(vfs_write(&partition_file,"partition",9)==9);vfs_close(&partition_file);
    assert(vfs_eject("/mnt/TRAVEL")==0&&mbr==saved_mbr);
    for(unsigned i=0;i<fat_volume_count;++i)
        for(auto* n=fat_volumes[i].nodes;n;){auto* next=n->next;free(n);n=next;}
    puts(exfat?"exFAT transfers, >4 GiB file, timestamps, cleanup and eject passed":"FAT32 transfers, timestamps, cleanup, busy/failed eject and reconnect passed");
}
