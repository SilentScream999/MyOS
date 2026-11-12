#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "../src/gui/image.h"

static int fail_after=-1, allocations=0, live=0;
void* image_allocate(uint64_t size) {
    if (fail_after>=0 && allocations++==fail_after) return nullptr;
    void* p=std::malloc(size); if(p) ++live; return p;
}
void image_deallocate(void* p) { if(p) { --live; std::free(p); } }
static std::vector<uint8_t> read(const std::string& path) {
    std::ifstream file(path,std::ios::binary); assert(file.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),{});
}
int main(int argc,char** argv) {
    assert(argc==3);
    std::ifstream cases(std::string(argv[1])+"/cases.txt");
    std::string name; unsigned width,height,count=0;
    while(cases>>name>>width>>height) {
        auto bytes=read(std::string(argv[1])+"/"+name+".png");
        auto expected=read(std::string(argv[1])+"/"+name+".rgba");
        Image image{}; assert(image_decode(bytes.data(),bytes.size(),&image));
        assert(image.width==width && image.height==height);
        for(unsigned i=0;i<width*height;++i) {
            uint32_t r=expected[i*4],g=expected[i*4+1],b=expected[i*4+2],a=expected[i*4+3];
            uint32_t pixel=a<<24 | ((r*a+127)/255)<<16 | ((g*a+127)/255)<<8 | (b*a+127)/255;
            assert(image.pixels[i]==pixel);
        }
        Image scaled{}; assert(image_resize(image,17,13,&scaled));
        std::vector<uint32_t> full(29*25,0xff123456),tiled=full;
        image_draw(scaled,full.data(),29,25,29,-3,2,0,0,29,25);
        for(int y=0;y<25;y+=3) for(int x=0;x<29;x+=5)
            image_draw(scaled,tiled.data(),29,25,29,-3,2,x,y,x+5,y+3);
        assert(full==tiled);
        image_draw(scaled,full.data(),29,25,29,INT32_MAX,INT32_MIN,0,0,29,25);
        assert(full==tiled);
        image_free(&scaled); image_free(&image); assert(live==0);
        // Every prefix and single-byte corruption of these small PNGs fails.
        for(size_t n=0;n<bytes.size();++n) { Image bad{}; assert(!image_decode(bytes.data(),n,&bad)); assert(live==0); }
        for(size_t i=0;i<bytes.size();++i) {
            bytes[i]^=1; Image bad{}; assert(!image_decode(bytes.data(),bytes.size(),&bad)); bytes[i]^=1; assert(live==0);
        }
        for(int failure=0;failure<20;++failure) {
            fail_after=failure; allocations=0;
            Image trial{}; image_decode(bytes.data(),bytes.size(),&trial); image_free(&trial);
            if(live) std::fprintf(stderr,"PNG allocation failure: %s step=%d live=%d\n",name.c_str(),failure,live);
            assert(live==0);
        }
        fail_after=-1;
        Image limited{};
        if(width*height>1) assert(!image_decode(bytes.data(),bytes.size(),&limited,width*height-1));
        ++count;
    }
    assert(count>=12);
    auto jpeg=read(argv[2]); Image photo{};
    assert(image_decode(jpeg.data(),jpeg.size(),&photo));
    assert(photo.width>0 && photo.height>0);
    for(uint64_t i=0;i<(uint64_t)photo.width*photo.height;++i) assert(photo.pixels[i]>>24==255);
    image_free(&photo);
    for(int failure=0;failure<40;++failure) {
        fail_after=failure; allocations=0;
        Image trial{}; image_decode(jpeg.data(),jpeg.size(),&trial); image_free(&trial); assert(live==0);
    }
    fail_after=-1;
    for(size_t n=0;n<jpeg.size();n+=17) { Image bad{}; assert(!image_decode(jpeg.data(),n,&bad)); assert(live==0); }
    assert(image_over(0xff0000ff,0x80800000)==0xff80007f);
    assert(image_over(0xff123456,0)==0xff123456);
    // Resizing a transparent colored edge must not produce a colored fringe.
    uint32_t edge_pixels[]={0,0xff0000ff}; Image edge{2,1,edge_pixels},resized{};
    assert(image_resize(edge,3,1,&resized));
    assert((resized.pixels[1]&0x00ff0000)==0);
    assert(image_over(0xffffffff,resized.pixels[1])==0xff7f7fff);
    image_free(&resized); assert(live==0);
    // Invalid inputs and allocation failure leave caller state unchanged.
    Image empty{}; assert(!image_decode(nullptr,5,&empty));
    assert(!image_decode(jpeg.data(),IMAGE_MAX_FILE+1,&empty));
    assert(!image_resize(empty,1,1,&resized));
    for(unsigned i=0;i<2000;++i) {
        std::vector<uint8_t> random(64); uint32_t state=i+1;
        for(auto& b:random) { state=state*1664525+1013904223; b=state>>24; }
        Image bad{}; assert(!image_decode(random.data(),random.size(),&bad)); assert(live==0);
    }
}
