#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#define STORAGE_HOST_TEST
static void* kmalloc(size_t size) { return calloc(1,size); }
static void kfree(void* pointer) { free(pointer); }
#include "../src/fs/ext4_vfs.h"
#include <ext4_mkfs.h>

static FILE* image;
static bool read_many(block_device*,uint64_t lba,uint32_t count,void* data) {
    return !fseeko(image,lba*512,SEEK_SET) && fread(data,512,count,image)==count;
}
static bool write_many(block_device*,uint64_t lba,uint32_t count,const void* data) {
    return !fseeko(image,lba*512,SEEK_SET) && fwrite(data,512,count,image)==count;
}
static bool read_one(block_device* disk,uint64_t lba,void* data) { return read_many(disk,lba,1,data); }
static bool write_one(block_device* disk,uint64_t lba,const void* data) { return write_many(disk,lba,1,data); }
static bool flush(block_device*) { return !fflush(image) && !fsync(fileno(image)); }
static void put(const char* path,const char* data) {
    ext4_file file{};size_t written=0;
    assert(ext4_fopen(&file,path,"wb")==EOK);
    assert(ext4_fwrite(&file,data,strlen(data),&written)==EOK && written==strlen(data));
    assert(ext4_fclose(&file)==EOK);
}
int main(int argc,char** argv) {
    assert(argc==3);
    image=fopen(argv[1],"r+b");assert(image);
    struct stat stat{};assert(!fstat(fileno(image),&stat));
    block_device disk{nullptr,(uint64_t)stat.st_size/512,true,read_one,write_one,flush,read_many,write_many};
    wy_ext4_volume volume{};volume.disk=&disk;volume.sectors=disk.sectors;
    const bool old=!strcmp(argv[2],"old"),fresh=!strcmp(argv[2],"reformat");
    if(old || fresh){
        volume.unpublished=true;
        wy_ext4_bind(&volume);ext4_fs fs{};ext4_mkfs_info info{};
        info.block_size=4096;info.journal=true;info.label="WYZARDATA";
        assert(ext4_mkfs(&fs,&volume.bdev,&info,F_SET_EXT4)==EOK);
        assert(wy_ext4_flush(&volume));
        wy_ext4_bind(&volume);
        assert(ext4_device_register(&volume.bdev,"system")==EOK);
        assert(ext4_mount("system","/",false)==EOK);
        if(old)assert(ext4_journal_start("/")==EOK);
        assert(ext4_dir_mk("/etc")==EOK);
        put("/etc/root.cfg",old?"old-root-marker!":"WYZARROOT1\n");
        assert(ext4_dir_mk("/home")==EOK);
        put("/home/document.txt",old?"previous-generation":"current-generation");
        if(old)assert(ext4_journal_stop("/")==EOK);
        assert(ext4_umount("/")==EOK);
        assert(wy_ext4_flush(&volume));
    }else{
        assert(wy_ext4_mount(&volume));
        ext4_file file{};char marker[32]{};size_t read=0;
        assert(ext4_fopen(&file,"/etc/root.cfg","rb")==EOK);
        assert(ext4_fread(&file,marker,sizeof(marker),&read)==EOK);
        assert(ext4_fclose(&file)==EOK);
        if(read!=11 || memcmp(marker,"WYZARROOT1\n",11)){
            fprintf(stderr,"Recovered marker has %zu bytes: %s\n",read,marker);
            return 1;
        }
        memset(marker,0,sizeof(marker));
        assert(ext4_fopen(&file,"/home/document.txt","rb")==EOK);
        assert(ext4_fread(&file,marker,sizeof(marker),&read)==EOK);
        assert(ext4_fclose(&file)==EOK);
        assert(read==18 && !memcmp(marker,"current-generation",18));
        // Deliberately omit journal stop/unmount to model unplugging after
        // an otherwise read-only boot with no outstanding device writes.
        assert(flush(&disk));
        if(!strcmp(argv[2],"verify"))assert(wy_ext4_unmount(&volume)==0);
    }
    assert(!fclose(image));
    return 0;
}
