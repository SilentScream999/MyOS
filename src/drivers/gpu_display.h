#ifndef WYZAR_GPU_DISPLAY_H
#define WYZAR_GPU_DISPLAY_H
#include <stdint.h>

struct GpuDisplayPlane {
    uint32_t control=0,surface=0,live=0,frame_count=0,original_surface=0;
    unsigned pipe=0,plane=0;
    uint32_t control_value=0;
};

// Take over only a single existing, linear XRGB8888 scanout with exactly the
// firmware framebuffer's geometry. Preserve its timings/scaler/watermarks and
// write only SURF after a complete hidden frame. Unknown/power-saving layouts
// retain the current presenter instead of attempting an unverified modeset.
template<class Read>
static bool gpu_find_display_plane(Read read,uint64_t framebuffer,uint32_t width,
                                   uint32_t height,uint32_t pitch,GpuDisplayPlane* output) {
    if(!width || !height || width>8191 || height>8192 || (pitch&63) ||
       pitch<width*4 || pitch>32767 || framebuffer>UINT32_MAX || (framebuffer&4095))return false;
    const uint32_t power_regs[]={0x43208u,0x6F800u,0x6F900u};
    for(uint32_t reg:power_regs){
        uint32_t value=read(reg); // FBC, eDP PSR1/PSR2
        if(value==UINT32_MAX || (value&(1u<<31)))return false;
    }
    unsigned matches=0;
    for(unsigned pipe=0;pipe<3;++pipe)for(unsigned plane=0;plane<3;++plane){
        uint32_t offset=pipe*0x1000+plane*0x100;
        uint32_t ctl=read(0x70180+offset);
        if(ctl==UINT32_MAX)return false;
        if(!(ctl&(1u<<31)))continue;
        uint32_t surface=read(0x7019C+offset),live=read(0x701AC+offset);
        if((surface&0xFFFFF000u)!=framebuffer && (live&0xFFFFF000u)!=framebuffer)continue;
        // SURFLIVE's address field is bits 31:12. Firmware on the 0x5916
        // laptop returns 0x20 for surface zero. Compare only the address,
        // matching the runtime latch check, while rejecting failed MMIO reads.
        if(surface!=framebuffer || live==UINT32_MAX ||
           (live&0xFFFFF000u)!=framebuffer || (ctl&(1u<<15)) ||
           (ctl&(7u<<10)) || (ctl&3) || (ctl&(3u<<4)) || (ctl&(1u<<9)) ||
           (ctl&(15u<<24))!=(4u<<24))return false;
        uint32_t stride=read(0x70188+offset),size=read(0x70190+offset);
        if(stride==UINT32_MAX || (stride&0xFFF)*64!=pitch ||
           (size&0xFFFF)+1!=width || (size>>16)+1!=height ||
           read(0x7018C+offset)!=0 || read(0x701A4+offset)!=0)return false;
        uint32_t cursor=read(0x70080+pipe*0x1000),count=read(0x70040+pipe*0x1000);
        if(cursor==UINT32_MAX || (cursor&0x27) || count==UINT32_MAX)return false;
        *output={0x70180+offset,0x7019C+offset,0x701AC+offset,0x70040+pipe*0x1000,
                 surface,pipe,plane,ctl};
        ++matches;
    }
    return matches==1;
}

struct GpuFlipState {
    uint32_t surfaces[2]{};
    int front=-1,pending=-1;
    uint32_t armed_frame=0;
    int writable() const {return pending>=0?-1:front==0?1:0;}
    bool arm(int index,uint32_t frame) {
        if(index<0 || index>1 || pending>=0 || index==front)return false;
        pending=index;armed_frame=frame;return true;
    }
    bool observe(uint32_t live,uint32_t frame) {
        if(pending<0 || live!=surfaces[pending] || frame==armed_frame)return false;
        front=pending;pending=-1;return true;
    }
};
#endif
