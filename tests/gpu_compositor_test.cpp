#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <map>
#include "src/drivers/gpu_scene_commands.h"
#include "src/drivers/gpu_address_space.h"
#include "src/drivers/gpu_mapping_cache.h"
#include "src/drivers/gpu_display.h"
#include "src/gui/gpu_presentation.h"
#include "src/gui/gpu_deferred_alpha.h"
#include "src/gui/window_shadow.cpp"
#include "src/gui/alpha_renderer.cpp"

static void scene_repair_batch_test(){
    VisibleRegion input{},region{};VisibleRect bounds{};
    input.count=3;
    input.rects[0]={1,60,5,64};input.rects[1]={3,62,7,66};input.rects[2]={9,60,11,66};
    assert(gpu_scene_repair_region(&region,input,{0,60,12,70},&bounds));
    assert(bounds.x1==1&&bounds.y1==60&&bounds.x2==11&&bounds.y2==66);
    unsigned hits[12*10]{};
    for(unsigned i=0;i<region.count;++i){const auto r=region.rects[i];
        for(int y=r.y1;y<r.y2;++y)for(int x=r.x1;x<r.x2;++x)++hits[(y-60)*12+x];
    }
    for(int y=60;y<70;++y)for(int x=0;x<12;++x){
        bool covered=false;
        for(unsigned i=0;i<input.count;++i){auto r=input.rects[i];
            covered|=x>=r.x1&&x<r.x2&&y>=r.y1&&y<r.y2;
        }
        assert(hits[(y-60)*12+x]==unsigned(covered));
    }
    alignas(64) uint32_t pixels[12*10]{};
    GpuSceneClient client{{0,60,12,70},{0,60,12,70},pixels,12,10,48,true,false,false,true,123};
    GpuScenePlan batch{},part{};
    for(unsigned i=0;i<region.count;++i){
        assert(gpu_alpha_scene_plan(&part,client,region.rects[i]));
        assert(gpu_scene_append_clips(&batch,part));
    }
    assert(batch.count==region.count&&batch.client_count==1);
    // Compare a partial-alpha repair with the union, including untouched holes.
    for(int y=60;y<70;++y)for(int x=0;x<12;++x){
        uint32_t actual=0xff506070,expected=actual;
        if(hits[(y-60)*12+x])expected=window_source_over(0x80402010,expected);
        for(unsigned i=0;i<batch.count;++i){auto r=batch.copies[i].destination;
            if(x>=r.x1&&x<r.x2&&y>=r.y1&&y<r.y2)actual=window_source_over(0x80402010,actual);
        }
        assert(actual==expected);
    }
    auto saved=batch.count;part.copies[0].source=1;
    assert(!gpu_scene_append_clips(&batch,part)&&batch.count==saved);
    part.copies[0].source=0;batch.count=GpuScenePlan::capacity;
    assert(!gpu_scene_append_clips(&batch,part)&&batch.count==GpuScenePlan::capacity);
    input.rects[0].y1=59;
    assert(!gpu_scene_repair_region(&region,input,{0,60,12,70},&bounds));
    input.count=VisibleRegion::capacity+1;
    assert(!gpu_scene_repair_region(&region,input,{0,60,12,70},&bounds));
}

// Interpret the actual emitted command stream, rather than a second high-level
// renderer. This checks command addresses, pitches, clipping, overlap, palette
// wrapping, replication barriers, NOOP padding and ring wrap on host memory.
struct Engine {
    alignas(64) uint32_t commands[8192]{},registers[128]{},fence[16]{};
    GpuRing ring;
    struct Mapping { uint64_t base; uint32_t* pixels; size_t bytes; };
    std::vector<Mapping> mappings;
    uint32_t timestamp=100;
    Engine() {
        ring.cpu=commands;ring.regs=registers;ring.fence_cpu=fence;
        ring.fence_gfx=0x1000;ring.size=sizeof(commands);ring.ready=true;
    }
    uint32_t* at(uint64_t address,size_t bytes) {
        for(auto m:mappings)if(address>=m.base && address-m.base<=m.bytes && bytes<=m.bytes-(address-m.base))
            return (uint32_t*)((uint8_t*)m.pixels+address-m.base);
        assert(false && "BLT accessed unmapped memory");return nullptr;
    }
    void execute() {
        unsigned head=registers[GPU_RING_HEAD/4];
        while(head!=ring.tail){
            timestamp+=7;
            auto* c=commands+head/4;unsigned length=1;
            if(!c[0]){}
            else if((c[0]&0xffc00000)==GPU_XY_SRC_COPY_BLT_CMD){
                length=10;
                assert((c[0]&255)==8);
                unsigned dx=c[2]&65535,dy=c[2]>>16,x2=c[3]&65535,y2=c[3]>>16;
                unsigned sx=c[6]&65535,sy=c[6]>>16;
                unsigned dp=c[1]&65535,sp=c[7]&65535;
                uint64_t dst=c[4]|((uint64_t)c[5]<<32),src=c[8]|((uint64_t)c[9]<<32);
                assert(dx<x2 && dy<y2);
                // The rainbow's doubling copies must never rely on undefined
                // traversal order for overlapping source/destination pixels.
                if(dst==src)assert(y2<=sy || sy+(y2-dy)<=dy || x2<=sx || sx+(x2-dx)<=dx);
                for(unsigned y=0;y<y2-dy;++y){
                    auto* from=at(src+(uint64_t)(sy+y)*sp+sx*4,(x2-dx)*4);
                    auto* to=at(dst+(uint64_t)(dy+y)*dp+dx*4,(x2-dx)*4);
                    std::memcpy(to,from,(x2-dx)*4);
                }
            }else if(c[0]==GPU_MI_STORE_REGISTER_MEM){
                length=4;assert(c[1]==GPU_BCS_BASE+GPU_RING_TIMESTAMP);
                *at(c[2]|((uint64_t)c[3]<<32),4)=timestamp;
            }else if((c[0]&0xff800000)==(GPU_MI_FLUSH_DW&0xff800000)){
                length=4;assert((c[0]&255)==2);assert((c[1]&~7u)==ring.fence_gfx);
                *ring.fence_cpu=c[3];
            }else {std::printf("Unknown command %08x\n",c[0]);assert(false);}
            assert(head+length*4<=ring.size);
            head=(head+length*4)&(ring.size-1);
        }
        registers[GPU_RING_HEAD/4]=head;
        ring.flush_from=ring.tail;
    }
};

static bool contains(VisibleRect r,int x,int y){return x>=r.x1&&x<r.x2&&y>=r.y1&&y<r.y2;}
static uint32_t seed=1234;
static uint32_t random_value(){return seed=seed*1664525+1013904223;}

static void scene_test(){
    constexpr int W=192,H=128;
    alignas(64) uint32_t surfaces[8][128*128];
    uint32_t colors[720];for(unsigned x=0;x<720;++x)colors[x]=0xff000000|x*9017;
    Engine engine; // Keep the ring alive across frames to exercise wrapping.
    unsigned wraps=0;
    for(unsigned frame=0;frame<300;++frame){
        GpuScenePlan plan{};plan.client_count=8;plan.scroll=frame==0?UINT_MAX:random_value();
        for(unsigned i=0;i<8;++i){
            int x=(int)(random_value()%200)-40,y=(int)(random_value()%130)-30;
            int w=32+random_value()%90,h=16+random_value()%100;
            bool rainbow=i==0,visible=(random_value()%5)!=0;
            for(int p=0;p<128*128;++p)surfaces[i][p]=0xff000000|(i<<20)|p;
            plan.clients[i]={{x,y-9,x+w,y+h},{x,y,x+w,y+h},surfaces[i],
                             (unsigned)w,(unsigned)h,512,visible,rainbow};
            if(i==1)plan.clients[i].width=w/2; // scaled clients use CPU fallback
            if(i==2)plan.clients[i].pixels=surfaces[i]+1; // unaligned source fallback
        }
        VisibleRect damage{0,(int)(random_value()%64),W,64+(int)(random_value()%65)};
        gpu_scene_plan(&plan,damage);
        std::vector<uint32_t> reference(W*H,0xff123456),staged=reference;
        auto pixel=[&](const GpuSceneClient& c,int x,int y){
            unsigned sx=(uint64_t)(x-c.client.x1)*c.width/(c.client.x2-c.client.x1);
            unsigned sy=(uint64_t)(y-c.client.y1)*c.height/(c.client.y2-c.client.y1);
            return c.rainbow?colors[((uint64_t)plan.scroll+sx)%720]:c.pixels[sy*(c.pitch/4)+sx];
        };
        for(unsigned i=0;i<8;++i){
            const auto& c=plan.clients[i];if(!c.visible)continue;
            for(int y=damage.y1;y<damage.y2;++y)for(int x=0;x<W;++x){
                if(!contains(c.outer,x,y))continue;
                if(!contains(c.client,x,y))reference[y*W+x]=staged[y*W+x]=0xffabcdef+i;
                else {
                    reference[y*W+x]=pixel(c,x,y);
                    if(!(plan.accelerated&(1u<<i)) || y>=c.client.y2-12)
                        staged[y*W+x]=pixel(c,x,y);
                }
            }
        }
        engine.mappings.clear();
        constexpr uint64_t destination=0x1000000,palette=0x2000000;
        engine.mappings.push_back({destination,staged.data(),staged.size()*4});
        engine.mappings.push_back({palette,colors,sizeof(colors)});
        for(unsigned i=0;i<8;++i)engine.mappings.push_back({0x3000000+i*0x10000, surfaces[i],sizeof(surfaces[i])});
        unsigned previous=engine.ring.tail;
        for(unsigned i=0;i<plan.count;++i){
            auto copy=plan.copies[i];const auto& c=plan.clients[copy.source];auto r=copy.destination;
            assert(contains(c.client,r.x1,r.y1));
            for(unsigned j=copy.source+1;j<8;++j)
                assert(!plan.clients[j].visible || !visibility_valid(visibility_intersect(r,plan.clients[j].outer)));
            bool ok=c.rainbow?gpu_emit_rainbow(&engine.ring,palette,destination,W*4,copy,plan.scroll)
                :gpu_emit_copy(&engine.ring,0x3000000+copy.source*0x10000,c.pitch,copy.source_x,copy.source_y,
                               destination,W*4,r.x1,r.y1,r.x2-r.x1,r.y2-r.y1);
            assert(ok);engine.execute();
            if(engine.ring.tail<previous)++wraps;previous=engine.ring.tail;
        }
        assert(reference==staged);
    }
    assert(wraps>0);
}

static void rainbow_test(){
    uint32_t colors[720];for(unsigned i=0;i<720;++i)colors[i]=0xff000000|i;
    std::vector<uint32_t> tile(720*GPU_RAIN_TILE_ROWS);
    for(unsigned i=0;i<tile.size();++i)tile[i]=colors[i%720];
    for(unsigned mode:{0u,1u,2u})
    for(unsigned width:{1u,15u,16u,719u,720u,721u,953u,967u,1441u,1920u,4096u})
        for(unsigned height:{1u,2u,3u,17u,257u,978u,1025u})
            for(unsigned scroll:{0u,719u,720u,UINT_MAX}){
                Engine engine;unsigned pitch=(width+31)&~15u;
                std::vector<uint32_t> pixels(pitch*(height+4),0xdeadbeef);
                engine.mappings={{0x1000000,tile.data(),tile.size()*4},{0x2000000,pixels.data(),pixels.size()*4}};
                GpuSceneCopy copy{0,{3,2,(int)width+3,(int)height+2},701,19};
                assert(gpu_emit_rainbow(&engine.ring,0x1000000,0x2000000,pitch*4,copy,scroll,
                    mode?GPU_RAIN_TILE_ROWS:1,mode==2));
                if(mode==2){
                    unsigned start=((uint64_t)scroll+copy.source_x)%720;
                    unsigned old_ops=((start+width+719)/720)*((height+255)/256);
                    assert(engine.ring.tail<=old_ops*40);
                    if(width==1920&&height==978)assert(engine.ring.tail<=3*40);
                    if(width==1441&&height==17)assert(engine.ring.tail==40);
                }
                engine.execute();
                for(unsigned y=0;y<height+4;++y)for(unsigned x=0;x<pitch;++x){
                    uint32_t expected=(y>=2&&y<height+2&&x>=3&&x<width+3)?
                        colors[((uint64_t)scroll+701+x-3)%720]:0xdeadbeef;
                    assert(pixels[y*pitch+x]==expected);
                }
            }
}

static void cursor_save_test(){
    constexpr int w=47,h=38,pitch=52;
    std::array<uint32_t,pitch*h> buffer[2],original[2];
    ScanoutCursorSave save[2]{};
    uint32_t sprite[32*32]{};
    for(unsigned i=0;i<32*32;++i)if(i%3)sprite[i]=0xff000000+i;
    for(int frame=0;frame<100;++frame){
        int b=frame%2;
        auto* target=(uint8_t*)buffer[b].data();
        if(frame>=2){
            assert(save[b].restore(target,w,h,pitch*4));
            assert(buffer[b]==original[b]);
            assert(!save[b].restore(target,w,h,pitch*4));
        }
        for(auto& pixel:buffer[b])pixel=random_value();
        original[b]=buffer[b];
        int x=(frame*17)%90-22,y=(frame*11)%79-20;
        save[b].paint(target,w,h,pitch*4,{x,y,0,0},sprite);
        for(int row=0;row<h;++row)for(int col=0;col<pitch;++col){
            int sx=col-x+12,sy=row-y+12;
            uint32_t ink=(col<w&&sx>=0&&sx<32&&sy>=0&&sy<32)?sprite[sy*32+sx]:0;
            assert(buffer[b][row*pitch+col]==(ink?ink:original[b][row*pitch+col]));
        }
    }
    auto other=buffer[1];
    assert(!save[0].restore((uint8_t*)buffer[1].data(),w,h,pitch*4));
    assert(buffer[1]==other);
}

static void invalid_test(){
    Engine e;unsigned tail=e.ring.tail;
    assert(!gpu_emit_copy(&e.ring,1,128,0,0,0x2000,128,0,0,1,1));
    assert(!gpu_emit_copy(&e.ring,0x1000,128,31,0,0x2000,128,0,0,2,1));
    assert(!gpu_emit_copy(&e.ring,0x1000,128,0,UINT_MAX,0x2000,128,0,0,2,1));
    assert(!gpu_emit_copy(&e.ring,0x1000,128,0,0,0x2000,128,UINT_MAX,0,2,1));
    assert(!gpu_emit_fill(&e.ring,0x1000,65536,0,0,2,2,0));
    assert(!gpu_emit_fill(&e.ring,0x1000,128,0,0,0,2,0));
    assert(tail==e.ring.tail);
    e.ring.seqno=UINT_MAX;uint32_t sequence;
    assert(gpu_emit_fence(&e.ring,&sequence,false));e.execute();
    assert(sequence==0 && *e.ring.fence_cpu==0);
    GpuScenePlan plan{};plan.client_count=33;gpu_scene_plan(&plan,{0,0,100,100});
    assert(!plan.count && !plan.accelerated);
}

static void memory_and_capacity_test(){
    auto occupied=[](uint64_t address){return address==0x2000||address==0x5000;};
    assert(gpu_find_gtt_hole(0x1000,0x9000,0x3000,occupied)==0x6000);
    assert(!gpu_find_gtt_hole(0x1000,0x8000,0x3000,occupied));
    assert(!gpu_find_gtt_hole(0x1001,0x9000,0x3000,occupied));
    assert(!gpu_find_gtt_hole(0x9000,0x1000,0x1000,occupied));
    assert(!gpu_find_gtt_hole(0x1000,0x9000,0,occupied));
    GpuScenePlan plan{};plan.client_count=32;
    auto* pixels=(uint32_t*)(uintptr_t)0x1000;
    // Sixteen horizontal clients crossed by sixteen foreground vertical
    // clients exceed the operation budget. Overflow must retain CPU ownership
    // of a whole client, rather than partially skipping its surface copies.
    for(int i=0;i<16;++i){
        VisibleRect r{0,i*24,1000,i*24+20};
        plan.clients[i]={r,r,pixels,1000,20,4000,true,false};
    }
    for(int i=16;i<32;++i){
        VisibleRect r{(i-16)*50+10,0,(i-16)*50+14,800};
        plan.clients[i]={r,r,pixels,4,800,64,true,false};
    }
    gpu_scene_plan(&plan,{0,0,1000,800});
    assert(plan.count<=GpuScenePlan::capacity);
    assert(!(plan.accelerated&(1u<<15)));
    for(unsigned i=0;i<plan.count;++i)assert(plan.copies[i].source!=15);
}

static void firmware_layout_test(){
    constexpr uint64_t maximum=64ULL<<20, fixed=0x5805000-maximum;
    constexpr uint64_t minimum=1920*1080*4;
    assert(gpu_source_capacity(0x3829000,fixed,minimum,maximum)==0x2024000);
    assert(gpu_source_capacity(fixed-1,fixed,minimum,maximum)==0);
    assert(gpu_source_capacity(fixed+minimum-1,fixed,minimum,maximum)==0);
    assert(gpu_source_capacity(fixed+minimum,fixed,minimum,maximum)==minimum);
    assert(gpu_source_capacity(fixed+maximum+4096,fixed,minimum,maximum)==maximum);
    assert(gpu_takeover_base(0x20000000,0xFE000000,0x57C5000)==0x20001000);
    assert(!gpu_takeover_base(0x20000000,0x20002000,4096));
    assert(!gpu_takeover_base(0x20000001,0xFE000000,4096));
    assert(!gpu_takeover_base(0x20000000,0xFE000000,UINT64_MAX));
    // Reproduce a fully present firmware GGTT with a shared dummy page. The
    // original non-present-only allocator must fail on this layout.
    constexpr uint64_t begin=0x20000000,end=0x40000000;
    constexpr uint64_t scratch=0x12345001;
    unsigned inspected=0;
    auto read=[&](uint64_t address){assert(address>=begin&&address<end);++inspected;return scratch;};
    auto absent=gpu_gtt_largest_run(begin,end,read,[](uint64_t pte){return !(pte&1);},false);
    assert(!absent.bytes && inspected==(end-begin)/4096);
    auto alias=gpu_gtt_largest_run(begin,end,read,[](uint64_t pte){return gpu_firmware_scratch_pte(pte,0xBD800000,0xBDA00000);},true);
    assert(alias.base==begin && alias.bytes==end-begin && alias.pte==scratch);
    // A unique page in the middle breaks the run; sampling its ends would
    // miss this occupied mapping. Invalid MMIO reads are never scratch space.
    auto fragmented=gpu_gtt_largest_run(begin,end,[&](uint64_t address){return address==0x30000000?scratch+4096:scratch;},
        [](uint64_t pte){return gpu_firmware_scratch_pte(pte,0xBD800000,0xBDA00000);},true);
    assert(fragmented.base==begin && fragmented.bytes==0x10000000);
    assert(!gpu_firmware_scratch_pte(UINT64_MAX,0xBD800000,0xBDA00000));
    assert(!gpu_firmware_scratch_pte(0xBD800001,0xBD800000,0xBDA00000));
    assert(!gpu_firmware_scratch_pte(0xBDA00001,0xBD800000,0xBDA00000));
    assert(!gpu_firmware_scratch_pte(0x12345000,0xBD800000,0xBDA00000));
    assert(!gpu_firmware_scratch_pte(0x8000000012345001,0xBD800000,0xBDA00000));
    std::map<uint32_t,uint32_t> regs;
    auto display=[&](uint32_t offset){return regs[offset];};
    auto enable=[&](unsigned pipe,unsigned plane,uint32_t surface,uint32_t live){
        uint32_t offset=pipe*0x1000+plane*0x100;
        regs[0x70180+offset]=0x84000000;
        regs[0x70188+offset]=64; // 4096-byte linear stride
        regs[0x70190+offset]=(767u<<16)|1023;
        regs[0x7019c+offset]=surface;regs[0x701ac+offset]=live;
    };
    uint64_t prefix=0,limit=0xFF000000ULL;
    assert(!gpu_firmware_display_prefix(display,0x10000000,0x400000,limit,&prefix));
    enable(0,0,0,0);
    assert(gpu_firmware_display_prefix(display,0x10000000,0x400000,limit,&prefix) && prefix==begin);
    enable(2,2,0x40000000,0x50000000); // protect the programmed and live base
    assert(gpu_firmware_display_prefix(display,0x10000000,0x400000,limit,&prefix) && prefix==0x70000000);
    regs[0x71080]=0x27;regs[0x71084]=0x90000000;
    assert(gpu_firmware_display_prefix(display,0x10000000,0x400000,limit,&prefix) && prefix==0x90200000);
    for(uint32_t unsupported:{0x84008000u,0x84000400u,0x81000000u,0x84000001u,UINT32_MAX}){
        regs[0x70180]=unsupported;
        assert(!gpu_firmware_display_prefix(display,0x10000000,0x400000,limit,&prefix));
    }
    regs[0x70180]=0x84000000;regs[0x701ac]=0xFE000000;
    assert(!gpu_firmware_display_prefix(display,0x10000000,0x400000,limit,&prefix));
    regs[0x701ac]=0;regs[0x70188]=0;
    assert(!gpu_firmware_display_prefix(display,0x10000000,0x400000,limit,&prefix));
}

static void mapping_cache_test(){
    GpuMappingCache cache;
    GpuMappingKey keys[]={{0x100000,0x2000},{0x200000,0x4000}};
    assert(!cache.matches(keys,2,0));
    cache.record(keys,2,0);
    for(unsigned frame=0;frame<10000;++frame)assert(cache.matches(keys,2,0));
    // Palette-only animation does not evict unchanged window mappings.
    assert(cache.matches(nullptr,0,0));cache.record(nullptr,0,0);
    assert(cache.matches(keys,2,0));
    assert(!cache.matches(keys,2,1)); // backing was retired/reused
    assert(!cache.matches(keys,1,0));
    keys[1].address+=4096;assert(!cache.matches(keys,2,0));keys[1].address-=4096;
    keys[1].bytes+=4096;assert(!cache.matches(keys,2,0));keys[1].bytes-=4096;
    auto first=keys[0];keys[0]=keys[1];keys[1]=first;
    assert(!cache.matches(keys,2,0));
    cache.record(keys,2,1);assert(cache.matches(keys,2,1));
    assert(!cache.matches(keys,33,1));
    for(uint32_t value:{0u,0xFFFFFFFFu,0x12345678u,0xA5A5A5A5u}){
        uint32_t updated=gpu_pat0_wb_llc(value);
        assert((updated&255)==7 && (updated&~255u)==(value&~255u));
        updated=gpu_pat0_uncached(value);
        assert(!(updated&255) && (updated&~255u)==(value&~255u));
    }
}

static void display_plane_test(){
    std::map<uint32_t,uint32_t> regs;
    auto read=[&](uint32_t reg){return regs[reg];};
    auto enable=[&](unsigned pipe,unsigned plane){
        unsigned offset=pipe*0x1000+plane*0x100;
        regs[0x70180+offset]=0x84000000;regs[0x70188+offset]=6;
        regs[0x70190+offset]=(63u<<16)|95;
    };
    GpuDisplayPlane plane;
    assert(!gpu_find_display_plane(read,0,96,64,384,&plane));
    enable(0,0);
    assert(gpu_find_display_plane(read,0,96,64,384,&plane));
    assert(plane.surface==0x7019C && plane.live==0x701AC && plane.frame_count==0x70040);
    regs[0x701AC]=0x20; // physical 0x5916 boot: address zero with low readback bits
    assert(gpu_find_display_plane(read,0,96,64,384,&plane));
    regs[0x701AC]=UINT_MAX;
    assert(!gpu_find_display_plane(read,0,96,64,384,&plane));
    regs[0x701AC]=0;
    const std::pair<uint32_t,uint32_t> invalid[]={
        {0x70180,0x84008000},{0x70180,0x84000400},{0x70180,0x84000200},
        {0x70180,0x84000010},{0x70180,0x84000001},{0x70180,0x81000000},
        {0x70188,5},{0x70190,(62u<<16)|95},{0x7018C,1},{0x701A4,1},
        {0x7019C,1},{0x701AC,0x1000},{0x70080,0x27},{0x70040,UINT_MAX},
        {0x43208,0x80000000},{0x6F800,0x80000000},{0x6F900,0x80000000}};
    for(auto [reg,value]:invalid){
        uint32_t before=regs[reg];regs[reg]=value;
        assert(!gpu_find_display_plane(read,0,96,64,384,&plane));regs[reg]=before;
    }
    enable(2,2);assert(!gpu_find_display_plane(read,0,96,64,384,&plane));
    regs[0x70180]=0;
    assert(gpu_find_display_plane(read,0,96,64,384,&plane) && plane.pipe==2 && plane.plane==2);
    assert(!gpu_find_display_plane(read,0x100000000ULL,96,64,384,&plane));
    assert(!gpu_find_display_plane(read,0,96,64,383,&plane));
}

static void complete_frame_flip_test(){
    constexpr unsigned W=96,H=64;
    constexpr uint64_t source_address=0x10000;
    GpuFlipState flip;flip.surfaces[0]=0x20000;flip.surfaces[1]=0x30000;
    std::vector<uint32_t> master(W*H,0xFF112233),buffers[2]={master,master},visible=master;
    Engine engine;
    engine.mappings={{source_address,master.data(),master.size()*4},
                     {flip.surfaces[0],buffers[0].data(),buffers[0].size()*4},
                     {flip.surfaces[1],buffers[1].data(),buffers[1].size()*4}};
    const OverlayFrame empty{{-999,-999,0,0},{},0};
    for(unsigned iteration=0;iteration<40;++iteration){
        // Partial scene changes must survive alternation without stale rows.
        for(unsigned x=0;x<W;++x)master[(iteration%H)*W+x]=0xFF000000|(iteration<<12)|x;
        auto clean=master,old_visible=visible;
        int back=flip.writable();assert(back>=0 && back!=flip.front);
        assert(gpu_emit_copy(&engine.ring,source_address,W*4,0,0,flip.surfaces[back],W*4,0,0,W,H));
        engine.execute();
        OverlayFrame overlay{{(int)(iteration*5%110)-10,(int)(iteration*3%72)-4,iteration%3,0},{},1};
        overlay.outlines[0]={8,9,42,29};
        auto color=[](const OverlayCursor&,int x,int y)->uint32_t{
            return x>=0&&x<5&&y>=0&&y<8?0xFFBBCCDD:0;
        };
        overlay_transition(empty,overlay,H,[&](int left,int right,int y){
            if(left<0)left=0;if(right>(int)W)right=W;
            if(left<right)overlay_present_row(buffers[back].data()+y*W,master.data()+y*W,
                left,right,y,overlay,color,[](uint32_t* d,const uint32_t* s,int bytes){std::memcpy(d,s,bytes);});
        },true);
        auto expected=master;
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
            const auto r=overlay.outlines[0];
            if((int)x>=r.x && (int)x<r.x+r.w && (int)y>=r.y && (int)y<r.y+r.h &&
               ((int)x<r.x+2||(int)x>=r.x+r.w-2||(int)y<r.y+2||(int)y>=r.y+r.h-2))expected[y*W+x]=0xFFFFFFFF;
            if(uint32_t ink=color(overlay.cursor,x-overlay.cursor.x,y-overlay.cursor.y))expected[y*W+x]=ink;
        }
        assert(buffers[back]==expected && master==clean && visible==old_visible);
        uint32_t frame=iteration==0?UINT_MAX:iteration;
        assert(flip.arm(back,frame) && flip.writable()==-1);
        assert(!flip.arm(1-back,frame));
        assert(!flip.observe(flip.surfaces[1-back],frame+1));
        assert(!flip.observe(flip.surfaces[back],frame));
        assert(visible==old_visible); // completed GPU copy is not a visible frame
        assert(flip.observe(flip.surfaces[back],frame+1)); // includes counter wrap
        visible=buffers[flip.front];assert(visible==expected);
        assert(gpu_overlay_equal(overlay,overlay));
        auto moved=overlay;++moved.cursor.x;assert(!gpu_overlay_equal(overlay,moved));
    }
    int back=flip.writable();assert(flip.arm(back,99));
    for(unsigned i=0;i<100;++i)assert(!flip.observe(flip.surfaces[1-back],100+i));
    assert(flip.writable()==-1); // timed-out/pending surfaces are never recycled
}

static void fullscreen_damage_test(){
    GpuScenePlan plan{};plan.client_count=2;
    auto* pixels=(uint32_t*)(uintptr_t)0x1000;
    plan.clients[0]={{0,0,800,560},{0,28,800,560},pixels,800,532,3200,true,false,true};
    plan.clients[1]={{0,0,800,560},{0,28,800,560},pixels,800,532,3200,true,true,false};
    VisibleRect damage{13,42,601,560};
    assert(gpu_rain_damage_plan(&plan,damage));
    assert(plan.count==1 && plan.copies[0].source==1 && plan.copies[0].source_x==13 && plan.copies[0].source_y==14);
    assert(plan.copies[0].destination.y2==560); // maximized client has no grip strip
    assert(!gpu_rain_damage_plan(&plan,{0,0,800,560})); // includes title chrome
    assert(!gpu_rain_damage_plan(&plan,{0,28,801,560}));
    plan.clients[1].resize_grip=true;assert(!gpu_rain_damage_plan(&plan,damage));
    plan.clients[1].resize_grip=false;plan.clients[1].rainbow=false;assert(!gpu_rain_damage_plan(&plan,damage));
    plan.clients[1].rainbow=true;plan.clients[1].width=799;assert(!gpu_rain_damage_plan(&plan,damage));
    plan.clients[1].width=800;plan.clients[1].visible=false;assert(!gpu_rain_damage_plan(&plan,damage));
    plan.clients[1].visible=true;plan.client_count=3;
    plan.clients[2]={{500,300,700,500},{500,328,700,500},pixels,200,172,800,true,false,true};
    assert(gpu_rain_damage_plan(&plan,{0,28,400,560})); // exposed beside Task Manager
    assert(!gpu_rain_damage_plan(&plan,{550,350,650,450})); // foreground app owns these pixels
}

static void retained_animation_test(){
    constexpr int W=192,H=128;
    std::vector<uint32_t> palette(720*GPU_RAIN_TILE_ROWS);
    for(unsigned y=0;y<GPU_RAIN_TILE_ROWS;++y)for(unsigned x=0;x<720;++x)palette[y*720+x]=0xff000000|x*9017;
    auto* pixels=(const uint32_t*)(uintptr_t)0x1000;
    const VisibleRect screen{0,0,W,H};
    VisibleRect covers[]={{48,32,109,99},{75,52,137,115},{110,20,174,70}};
    GpuSceneClient client{{-8,-10,188,120},{-8,4,188,120},pixels,196,116,784,true,true,false};
    GpuScenePlan plan{};
    assert(gpu_retained_animation_plan(&plan,client,screen,covers,3));
    assert(plan.count>1&&plan.client_count==1);
    for(unsigned i=0;i<plan.count;++i)for(auto cover:covers)
        assert(!visibility_valid(visibility_intersect(plan.copies[i].destination,cover)));
    // Build complete reference scenes in stack order. Retained buffers must
    // match them without repainting any foreground window, including shadows
    // cast by a higher window onto another static foreground window.
    auto reference=[&](unsigned phase){
        std::vector<uint32_t> result(W*H,0xff293645);
        for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(contains(client.client,x,y))
            result[y*W+x]=palette[(phase+x-client.client.x1)%720];
        for(unsigned s=0;s<3;++s){
            window_shadow_draw({(uint8_t*)result.data(),W,H,W*4},covers[s],screen);
            for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(contains(covers[s],x,y))
                result[y*W+x]=0xff806040+s*0x101010;
        }
        return result;
    };
    std::array<std::vector<uint32_t>,2> buffers{reference(0),reference(0)};
    Engine engine;
    for(unsigned frame=1;frame<=90;++frame){
        auto& target=buffers[frame%2];unsigned phase=frame*17%720;
        engine.mappings={{0x1000000,palette.data(),palette.size()*4},{0x2000000,target.data(),target.size()*4}};
        for(unsigned i=0;i<plan.count;++i)
            assert(gpu_emit_rainbow(&engine.ring,0x1000000,0x2000000,W*4,plan.copies[i],phase,GPU_RAIN_TILE_ROWS));
        uint32_t sequence;assert(gpu_emit_fence(&engine.ring,&sequence,false));engine.execute();
        assert(*engine.ring.fence_cpu==sequence);
        uint32_t background_row[W];
        for(int x=0;x<W;++x)background_row[x]=palette[(phase+x-client.client.x1)%720];
        for(unsigned i=0;i<plan.count;++i)for(unsigned s=0;s<3;++s)
            window_shadow_draw({(uint8_t*)target.data(),W,H,W*4,s==0?background_row:nullptr},covers[s],plan.copies[i].destination);
        assert(target==reference(phase));
    }
    client.premultiplied=true;
    assert(!gpu_retained_animation_plan(&plan,client,screen,covers,3)&&!plan.count);
    client.premultiplied=false;
    assert(!gpu_retained_animation_plan(&plan,client,screen,&screen,1)&&!plan.count);
    // Many tiny holes exceed the bounded visibility storage. Reject the fast
    // path completely instead of overwriting retained opaque pixels.
    std::array<VisibleRect,80> fragmented{};
    for(unsigned i=0;i<fragmented.size();++i)fragmented[i]={(int)i*2,10,(int)i*2+1,12};
    assert(!gpu_retained_animation_plan(&plan,client,screen,fragmented.data(),fragmented.size())&&!plan.count);
}
static void timestamp_command_test(){
    Engine engine;
    uint32_t source[16],destination[16]{},stamps[16]{};
    for(unsigned i=0;i<16;++i)source[i]=0xff120000+i;
    engine.mappings={{0x10000,source,sizeof(source)},{0x20000,destination,sizeof(destination)},
                     {0x3000,stamps,sizeof(stamps)}};
    assert(gpu_emit_timestamp(&engine.ring,GPU_BCS_BASE+GPU_RING_TIMESTAMP,0x3000));
    assert(gpu_emit_copy(&engine.ring,0x10000,64,0,0,0x20000,64,0,0,16,1));
    uint32_t sequence;
    assert(gpu_emit_fence(&engine.ring,&sequence,false));
    assert(gpu_emit_timestamp(&engine.ring,GPU_BCS_BASE+GPU_RING_TIMESTAMP,0x3004));
    assert(gpu_emit_fence(&engine.ring,&sequence,false));
    engine.execute();
    assert(stamps[0]>0&&stamps[1]>stamps[0]&&*engine.ring.fence_cpu==sequence);
    for(unsigned i=0;i<16;++i)assert(destination[i]==source[i]);
    assert(!gpu_emit_timestamp(&engine.ring,3,0x3000));
    assert(!gpu_emit_timestamp(&engine.ring,GPU_BCS_BASE+GPU_RING_TIMESTAMP,0x3001));
    assert(!gpu_emit_timestamp(&engine.ring,GPU_BCS_BASE+GPU_RING_TIMESTAMP,1ULL<<32));
}

static void opaque_coverage_test(){
    constexpr unsigned W=64,H=48,P=68;
    alignas(64) uint32_t pixels[P*H];
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<P;++x)
        pixels[y*P+x]=x<18?0:x<40?0x80402010:0xff123456;
    GpuOpaqueCoverage coverage;coverage.update(pixels,W,H,P*4,1);
    assert(coverage.count==1);
    for(int w:{17,64,133,317})for(int h:{13,48,101})for(int ox:{-71,0,23}){
        VisibleRect client{ox,-9,ox+w,-9+h},rects[8];
        unsigned count=coverage.mapped(client,3,rects);uint64_t area=0;
        for(unsigned i=0;i<count;++i){auto r=rects[i];
            assert(r.y2<=client.y2-3);
            for(int y=r.y1;y<r.y2;++y)for(int x=r.x1;x<r.x2;++x){
                unsigned sx=(2ull*(x-client.x1)+1)*W/(2*w);
                unsigned sy=(2ull*(y-client.y1)+1)*H/(2*h);
                assert((pixels[sy*P+sx]>>24)==255);
                unsigned gpu_x=(unsigned)(((float)(x-client.x1)+.5f)/w*W);
                unsigned gpu_y=(unsigned)(((float)(y-client.y1)+.5f)/h*H);
                assert((pixels[gpu_y*P+gpu_x]>>24)==255);
                ++area;
            }
        }
        if(w>64&&h>48)assert(area>0);
    }
    // A same-address new revision with holes must revoke the previous proof.
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)pixels[y*P+x]=0x80402010;
    coverage.update(pixels,W,H,P*4,2);assert(!coverage.count);
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)pixels[y*P+x]=0xffffffff;
    coverage.update(pixels,W,H,P*4,0);assert(!coverage.count);
    assert(coverage.update(pixels,W,H,P*4,3));assert(coverage.count==1);
    assert(!coverage.update(pixels,W,H,P*4,3)); // Same immutable snapshot reuses proof.
    coverage.update(pixels,W,H,8,4);assert(!coverage.count);
    auto* fake=(const uint32_t*)(uintptr_t)0x1000;
    GpuSceneClient animation{{0,0,512,512},{0,0,512,512},fake,512,512,2048,true,true,false};
    GpuScenePlan plan{};assert(gpu_retained_animation_plan(&plan,animation,{0,0,512,512},nullptr,0));
    auto original=plan.copies[0];
    VisibleRect cuts[70];for(int i=0;i<70;++i)cuts[i]={i*6+1,0,i*6+2,512};
    assert(!gpu_retained_opaque_cutouts(&plan,cuts,70));
    assert(plan.count==1&&!memcmp(&plan.copies[0],&original,sizeof(original)));
}

static void retained_translucency_test(bool opaque_cutouts,bool defer_repairs=false,bool menu_replay=false,bool transition_rebase=false,bool picker=false){
    constexpr int W=192,H=128;
    alignas(64) uint32_t textures[2][64*48];
    for(unsigned s=0;s<2;++s)for(unsigned y=0;y<48;++y)for(unsigned x=0;x<64;++x){
        unsigned a=x<16?0:x<48?128:255;
        textures[s][y*64+x]=(a<<24)|((a*x/63)<<16)|((a*y/47)<<8)|(a/(s+2));
    }
    auto* pixels=(const uint32_t*)(uintptr_t)0x1000;
    GpuSceneClient animation{{-8,-10,188,120},{-8,4,188,120},pixels,196,116,784,true,true,false};
    GpuSceneClient layers[]={
        {{48,32,109,99},{48,42,109,99},pixels,61,57,244,true,false,false},
        {{30,18,162,118},{30,28,162,118},textures[0],64,48,256,true,false,true,true,1},
        {{110,20,174,70},{110,30,174,70},pixels,64,40,256,true,false,false},
        {{65,40,180,125},{65,50,180,125},textures[1],64,48,256,true,false,false,true,2}};
    VisibleRect screen{0,0,W,H},covers[5];
    VisibleRect picker_panel{12,80,72,116};
    covers[4]=picker_panel;
    for(unsigned s=0;s<4;++s){
        covers[s]=layers[s].outer;
        if(layers[s].premultiplied)covers[s].y2=layers[s].client.y1;
    }
    GpuScenePlan plan{};assert(gpu_retained_animation_plan(&plan,animation,screen,covers,4));
    auto prepare=[&](){
        for(unsigned s=0;s<4;++s){covers[s]=layers[s].outer;
            if(layers[s].premultiplied)covers[s].y2=layers[s].client.y1;}
        assert(gpu_retained_animation_plan(&plan,animation,screen,covers,picker?5:4));
        uint64_t before=0,after=0;
        for(unsigned i=0;i<plan.count;++i){auto r=plan.copies[i].destination;before+=(r.x2-r.x1)*(r.y2-r.y1);}
        if(opaque_cutouts){
            VisibleRect cuts[32];unsigned count=0;
            for(const auto& c:layers)if(c.premultiplied){GpuOpaqueCoverage coverage;
                coverage.update(c.pixels,c.width,c.height,c.pitch,c.revision);
                count+=coverage.mapped(c.client,c.resize_grip?12:0,cuts+count);}
            assert(gpu_retained_opaque_cutouts(&plan,cuts,count));
        }
        for(unsigned i=0;i<plan.count;++i){auto r=plan.copies[i].destination;after+=(r.x2-r.x1)*(r.y2-r.y1);}
        if(opaque_cutouts)assert(after<before);
    };
    prepare();
    for(unsigned variant=0;variant<3;++variant){
        DamageList dirty{};
        if(variant==0)damage_add(&dirty,0,W-1,0,H-1);
        if(variant==1)damage_add(&dirty,35,150,30,110);
        if(variant==2){damage_add(&dirty,45,100,39,75);damage_add(&dirty,120,181,55,126);}
        VisibleRegion repairs{};assert(gpu_retained_repair_region(&repairs,dirty,plan));
        for(int y=0;y<H;++y)for(int x=0;x<W;++x){
            bool damaged=false,animated=false,repaired=false;
            for(unsigned i=0;i<dirty.n;++i){auto r=dirty.r[i];damaged|=x>=(int)r.x1&&x<=(int)r.x2&&y>=(int)r.y1&&y<=(int)r.y2;}
            for(unsigned i=0;i<plan.count;++i)animated|=contains(plan.copies[i].destination,x,y);
            for(unsigned i=0;i<repairs.count;++i)repaired|=contains(repairs.rects[i],x,y);
            assert(repaired==(damaged&&!animated));
        }
    }
    std::vector<uint32_t> palette(720*GPU_RAIN_TILE_ROWS);
    for(unsigned y=0;y<GPU_RAIN_TILE_ROWS;++y)for(unsigned x=0;x<720;++x)palette[y*720+x]=0xff000000|x*9017;
    unsigned decoration=0,repair_ink=0;
    bool menu_open=menu_replay;VisibleRect panel{5,80,80,126};unsigned menu_hover=0;
    std::vector<uint32_t> menu_cache(W*H);
    auto build_menu=[&](){for(int y=0;y<H;++y)for(int x=0;x<W;++x)
        menu_cache[y*W+x]=0xff203040+menu_hover*0x10203+((x^y)&7)*0x010101;};
    build_menu();
    auto reference=[&](unsigned phase,unsigned limit=4){
        std::vector<uint32_t> result(W*H,0xff293645);
        for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(contains(animation.client,x,y))
            result[y*W+x]=palette[(phase+x-animation.client.x1)%720];
        for(unsigned s=0;s<limit;++s){
            const auto& c=layers[s];
            window_shadow_draw({(uint8_t*)result.data(),W,H,W*4},c.outer,screen);
            for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(contains(c.outer,x,y)&&
                (!c.premultiplied||y<c.client.y1))result[y*W+x]=0xff806040+s*0x101010+(s==0?decoration:0)+
                    (s==0&&x>=95&&x<105&&y>=60&&y<68?repair_ink:0);
            if(c.premultiplied)alpha_render({result.data(),W,H,W*4},
                {c.pixels,(int)c.width,(int)c.height,(int)c.pitch},c.client,screen);
        }
        if(menu_open)for(int y=panel.y1;y<panel.y2;++y)for(int x=panel.x1;x<panel.x2;++x)
            if(y>=0&&y<H&&x>=0&&x<W)result[y*W+x]=menu_cache[y*W+x];
        if(picker)for(int y=picker_panel.y1;y<picker_panel.y2;++y)for(int x=picker_panel.x1;x<picker_panel.x2;++x)
            result[y*W+x]=0xff151b2e;
        return result;
    };
    std::array<std::vector<uint32_t>,2> buffers{reference(0),reference(0)};
    Engine engine;
    bool pending_repair[2]{};
    unsigned deferred_frames=0;
    bool full_repair_pending[2]{};
    auto rebuild=[&](unsigned phase){
        if(transition_rebase)full_repair_pending[0]=full_repair_pending[1]=true;
        else for(auto& buffer:buffers)buffer=reference(phase);
    };
    for(unsigned frame=1;frame<=90;++frame){
        unsigned phase=frame*19%720;
        if(menu_replay&&(frame==20||frame==35||frame==45||frame==55||frame==80)){
            if(frame==20||frame==80)menu_open=false;
            if(frame==35)menu_open=true;
            if(frame==45)panel={10,75,85,120};
            if(frame==55){++menu_hover;build_menu();}
            // Cache/layout changes use ordinary complete composition before
            // allowing retained replay again, on both pending scanouts.
            rebuild(phase);
        }
        if(frame==40||frame==50||frame==60||frame==70){
            if(frame==40){layers[1].client.x1-=5;layers[1].outer.x1-=5;}
            if(frame==50){layers[3].client.y1+=3;layers[3].outer.y1+=3;}
            if(frame==60){
                for(unsigned y=0;y<48;++y)textures[0][y*64+55]=0x80402010;
                ++layers[1].revision;
            }
            if(frame==70){auto tmp=layers[1];layers[1]=layers[3];layers[3]=tmp;}
            prepare();
            // Model the normal scene repair required for source/layout changes
            // on both alternating scanouts before retaining the new coverage.
            rebuild(phase);
        }
        // Simulate a genuine foreground update repaired on both retained
        // buffers, including the cached translucent pixels above that update.
        if(frame==30){
            decoration=0x030303;auto updated=reference(phase);
            for(auto& buffer:buffers)for(int y=32;y<99;++y)for(int x=48;x<109;++x)buffer[y*W+x]=updated[y*W+x];
        }
        auto& target=buffers[frame%2];
        if(full_repair_pending[frame%2]){
            VisibleRegion all{},repairs{};all.count=1;all.rects[0]={0,0,W,H};
            assert(gpu_retained_repair_region(&repairs,all,plan));
            auto current=reference(phase);
            for(unsigned i=0;i<repairs.count;++i){auto r=repairs.rects[i];
                for(int y=r.y1;y<r.y2;++y)for(int x=r.x1;x<r.x2;++x)target[y*W+x]=current[y*W+x];
            }
            full_repair_pending[frame%2]=false;
        }
        GpuDeferredAlpha deferred;
        if(defer_repairs&&frame%7==0){repair_ink+=0x100;pending_repair[0]=pending_repair[1]=true;}
        if(defer_repairs&&pending_repair[frame%2]){
            const auto& top=layers[3];deferred.begin(top,true);
            GpuScenePlan repair{};repair.client_count=1;repair.clients[0]=top;
            repair.count=1;repair.copies[0]={0,{95,60,105,68},0,0};
            assert(deferred.append(repair,plan));
            auto underneath=reference(phase,3);
            for(int y=60;y<68;++y)for(int x=95;x<105;++x)target[y*W+x]=underneath[y*W+x];
            pending_repair[frame%2]=false;deferred.active=false;
            assert(deferred.ready(top,plan));++deferred_frames;
        }
        engine.mappings={{0x1000000,palette.data(),palette.size()*4},{0x2000000,target.data(),target.size()*4}};
        for(unsigned i=0;i<plan.count;++i)
            assert(gpu_emit_rainbow(&engine.ring,0x1000000,0x2000000,W*4,plan.copies[i],phase,GPU_RAIN_TILE_ROWS));
        uint32_t sequence;assert(gpu_emit_fence(&engine.ring,&sequence,false));engine.execute();
        uint32_t background_row[W];
        for(int x=0;x<W;++x)background_row[x]=palette[(phase+x-animation.client.x1)%720];
        unsigned layer_index=0;
        for(const auto& c:layers){
            for(unsigned i=0;i<plan.count;++i)window_shadow_draw(
                {(uint8_t*)target.data(),W,H,W*4,layer_index==0?background_row:nullptr},c.outer,plan.copies[i].destination);
            ++layer_index;
            if(!c.premultiplied)continue;
            GpuScenePlan alpha{};assert(gpu_retained_alpha_plan(&alpha,c,plan));
            if(defer_repairs&& &c==&layers[3])assert(deferred.combine(&alpha));
            for(unsigned i=0;i<alpha.count;++i){
                const auto& r=alpha.copies[i].destination;
                for(int y=r.y1;y<r.y2;++y)for(int x=r.x1;x<r.x2;++x){
                    unsigned sx=((2*(x-c.client.x1)+1)*c.width)/(2*(c.client.x2-c.client.x1));
                    unsigned sy=((2*(y-c.client.y1)+1)*c.height)/(2*(c.client.y2-c.client.y1));
                    auto* row=(const uint32_t*)((const uint8_t*)c.pixels+sy*c.pitch);
                    target[y*W+x]=window_source_over(row[sx],target[y*W+x]);
                }
            }

        }
        if(menu_open)gpu_retained_overlay_replay(plan,panel,[&](VisibleRect r){
            for(int y=r.y1;y<r.y2;++y)for(int x=r.x1;x<r.x2;++x)target[y*W+x]=menu_cache[y*W+x];
        });
        auto expected=reference(phase);
        if(target!=expected)for(unsigned p=0;p<target.size();++p)if(target[p]!=expected[p]){
            std::fprintf(stderr,"Retained alpha frame %u pixel %u,%u actual %08x expected %08x\n",frame,p%W,p/W,target[p],expected[p]);break;
        }
        assert(target==expected);
    }
    if(defer_repairs)assert(deferred_frames>=20);
}

int main(){scene_repair_batch_test();cursor_save_test();scene_test();rainbow_test();invalid_test();memory_and_capacity_test();firmware_layout_test();mapping_cache_test();display_plane_test();complete_frame_flip_test();fullscreen_damage_test();retained_animation_test();opaque_coverage_test();retained_translucency_test(false);retained_translucency_test(true);retained_translucency_test(true,true);retained_translucency_test(true,true,true);retained_translucency_test(true,true,true,true);retained_translucency_test(true,false,false,true,true);timestamp_command_test();puts("GPU composition, timestamp packets, retained animation/alpha/shadows, opaque cutouts, deferred repairs, menu replay, complete flips and retirement tests passed.");}
