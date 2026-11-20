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

// Interpret the actual emitted command stream, rather than a second high-level
// renderer. This checks command addresses, pitches, clipping, overlap, palette
// wrapping, replication barriers, NOOP padding and ring wrap on host memory.
struct Engine {
    alignas(64) uint32_t commands[8192]{},registers[128]{},fence[16]{};
    GpuRing ring;
    struct Mapping { uint64_t base; uint32_t* pixels; size_t bytes; };
    std::vector<Mapping> mappings;
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
    for(unsigned width:{1u,15u,16u,719u,720u,721u,1441u,4096u})
        for(unsigned height:{1u,2u,3u,17u,257u})
            for(unsigned scroll:{0u,719u,720u,UINT_MAX}){
                Engine engine;unsigned pitch=(width+31)&~15u;
                std::vector<uint32_t> pixels(pitch*(height+4),0xdeadbeef);
                engine.mappings={{0x10000,colors,sizeof(colors)},{0x20000,pixels.data(),pixels.size()*4}};
                GpuSceneCopy copy{0,{3,2,(int)width+3,(int)height+2},701,19};
                assert(gpu_emit_rainbow(&engine.ring,0x10000,0x20000,pitch*4,copy,scroll));
                engine.execute();
                for(unsigned y=0;y<height+4;++y)for(unsigned x=0;x<pitch;++x){
                    uint32_t expected=(y>=2&&y<height+2&&x>=3&&x<width+3)?
                        colors[((uint64_t)scroll+701+x-3)%720]:0xdeadbeef;
                    assert(pixels[y*pitch+x]==expected);
                }
            }
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

int main(){scene_test();rainbow_test();invalid_test();memory_and_capacity_test();firmware_layout_test();mapping_cache_test();display_plane_test();complete_frame_flip_test();fullscreen_damage_test();puts("GPU composition, fullscreen damage, display guards, complete frame/overlay flips and retirement tests passed.");}
