#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#define NATIVE_HOST_TEST
#include "../user/image_viewer.cpp"
static std::map<std::string,std::vector<uint8_t>> files;
static std::string reading;
static uint64_t offset;
static bool fail_read;
static uint64_t native_call(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t,uint64_t){
    switch(n){
    case 4:if(!files.count((char*)a))return -1;*(file_info*)b={};((file_info*)b)->type=1;((file_info*)b)->size=files[(char*)a].size();return 0;
    case 2:if(!files.count((char*)a))return -1;reading=(char*)a;offset=0;return 10;
    case 0:{if(fail_read)return -1;auto& bytes=files[reading];uint64_t count=c;
        if(count>7)count=7;if(count>bytes.size()-offset)count=bytes.size()-offset;
        memcpy((void*)b,bytes.data()+offset,(size_t)count);offset+=count;return count;}
    case 3:case 201:return 0;
    default:return -1;
    }
}
static std::vector<uint8_t> read(const std::string& path){std::ifstream in(path,std::ios::binary);assert(in.good());return {(std::istreambuf_iterator<char>(in)),{}};}
static void capture(const std::string& filename){
    std::ofstream out(filename,std::ios::binary);out<<"P6\n"<<surface.w<<" "<<surface.h<<"\n255\n";
    for(int i=0;i<surface.w*surface.h;++i){auto p=surface.pixels[i];char rgb[]={(char)(p>>16),(char)(p>>8),(char)p};out.write(rgb,3);}
}
int main(int argc,char** argv){
    assert(argc==4);std::vector<uint32_t> pixels(800*560);surface={pixels.data(),800,560};
    render();capture(std::string(argv[3])+"-empty.ppm");
    files["/home/alpha.png"]=read(std::string(argv[1])+"/alpha.png");
    files["/home/photo.jpg"]=read(argv[2]);files["/home/bad.png"]={1,2,3};
    assert(load("/home/alpha.png")&&picture.width==2&&picture.height==1&&fit);
    auto* original=picture.pixels;
    assert(!load("/home/bad.png")&&picture.pixels==original);
    assert(!load("relative.png")&&picture.pixels==original);
    fail_read=true;assert(!load("/home/photo.jpg")&&picture.pixels==original);fail_read=false;
    handle_event({5,0,0,'1',0,0});assert(!fit&&zoom==100);
    for(int i=0;i<40;++i)zoom_by(1);assert(zoom==800);
    for(int i=0;i<40;++i)zoom_by(-1);assert(zoom==10);
    handle_event({5,0,0,'f',0,0});assert(fit);
    assert(load("/home/photo.jpg"));render();
    handle_event({5,0,0,'1',0,0});zoom=800;pan_x=999999;pan_y=-999999;auto r=view_rect();
    assert(r.w==(int)picture.width*8&&pan_x<999999&&pan_y>-999999);
    handle_event({1,200,200,1,0,0});assert(dragging);
    handle_event({3,180,190,0,0,0});handle_event({2,180,190,1,0,0});assert(!dragging);
    fit=true;pan_x=pan_y=0;render();
    capture(argv[3]);
    // Repeated replacement and failed decodes must return scratch to the arena.
    for(int i=0;i<100;++i){assert(load("/home/alpha.png"));assert(!load("/home/bad.png"));assert(load("/home/photo.jpg"));}
    image_free(&picture);
    assert(heap_head&&heap_head->free&&!heap_head->next&&heap_head->size==VIEWER_HEAP-sizeof(ViewerBlock));
    void* full=image_allocate(VIEWER_HEAP-sizeof(ViewerBlock));assert(full);assert(!image_allocate(16));image_deallocate(full);
    puts("Image Viewer load, failure preservation, zoom, pan and arena lifetime passed.");
}
