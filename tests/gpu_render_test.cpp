#include <cassert>
#include <cstdio>
#include <cstring>
#include "src/drivers/gpu_render_commands.h"
#include "src/drivers/gpu_render_reset.h"
#include "src/gui/gpu_compositor.h"

static float floating(uint32_t word) { float f;memcpy(&f,&word,4);return f; }
static void reset_handshake_test() {
    struct IO {
        bool ack=true,finish=true,requested=false,ready=false,resetting=false;
        unsigned preparations=0,resets=0,cancellations=0,ready_reads=0,reset_reads=0;
        uint64_t clock=0;
        uint64_t now(){return clock++;}
        void pause(){++clock;}
        uint32_t read(uint32_t reg){
            if(reg==0x20D0){
                assert(requested);
                if(ack&&++ready_reads>=3)ready=true;
                return ready?2:0;
            }
            assert(reg==0x941C&&resetting);
            if(finish&&++reset_reads>=3){resetting=false;return 0;}
            return 2;
        }
        void write(uint32_t reg,uint32_t value){
            if(reg==0x20D0){
                if(value==0x10001){
                    assert(!requested);requested=true;ready=false;
                    ready_reads=0;++preparations;
                }else{
                    assert(value==0x10000&&requested);
                    requested=false;ready=false;++cancellations;
                }
            }else{
                assert(reg==0x941C&&value==2&&requested&&ready);
                resetting=true;reset_reads=0;++resets;
            }
        }
    };
    IO success;
    assert(gpu_render_domain_reset(success,20));
    assert(success.preparations==2&&success.resets==2&&success.cancellations==2);
    assert(!success.requested);
    for(unsigned i=0;i<1024;++i)assert(gpu_render_domain_reset(success,20));
    assert(success.preparations==2050&&success.resets==2050&&success.cancellations==2050);
    IO not_ready;not_ready.ack=false;
    assert(!gpu_render_domain_reset(not_ready,20));
    assert(not_ready.preparations==1&&not_ready.resets==0&&not_ready.cancellations==1);
    assert(!not_ready.requested);
    IO stuck;stuck.finish=false;
    assert(!gpu_render_domain_reset(stuck,20));
    assert(stuck.preparations==1&&stuck.resets==1&&stuck.cancellations==1);
    assert(!stuck.requested);
}
static void texture_state_test() {
    struct { uint32_t before;uint32_t words[1024];uint32_t after; } buffer{};
    buffer.before=0x12345678;buffer.after=0x87654321;
    GpuTextureDraw draw{{0x10000000,16,8,64},{0x2000000,96,64,384},
                        {-4,-2,28,14},{0,0,20,12},true};
    assert(gpu_build_texture_batch(buffer.words,0x100000,draw));
    assert(buffer.before==0x12345678&&buffer.after==0x87654321);
    const auto* source=buffer.words+GPU_RENDER_SRC_STATE/4;
    assert(((source[0]>>18)&511)==0xC0); // BGRA8 UNORM
    assert(source[2]==(7u<<16|15)&&source[3]==63);
    assert(source[8]==draw.source.address&&source[9]==0);
    assert(source[7]==(4u<<25|5u<<22|6u<<19|7u<<16));
    assert(buffer.words[18]==0x100001&&buffer.words[20]==0x100001&&buffer.words[24]==0x100001);
    assert(buffer.words[262]==0x100000+GPU_RENDER_VERTEX_OFFSET);
    assert(buffer.words[258]==(63u<<16|95));
    const auto* v=buffer.words+GPU_RENDER_VERTEX_OFFSET/4;
    assert(v[0]==(14u<<16|28)&&v[3]==(14u<<16|0xFFFCu)&&v[6]==0xFFFEFFFCu);
    assert(floating(v[1])==1.f&&floating(v[2])==1.f);
    assert(floating(v[4])==0.f&&floating(v[8])==0.f);
    assert(buffer.words[GPU_RENDER_RASTER_DWORD]&(1u<<1));
    assert(buffer.words[230]==GPU_RENDER_SCISSOR_STATE);
    assert(buffer.words[GPU_RENDER_SCISSOR_STATE/4]==0);
    assert(buffer.words[GPU_RENDER_SCISSOR_STATE/4+1]==(11u<<16|19));
    // Independently decode the hardware blend fields, including PS_BLEND.
    uint32_t blend=buffer.words[2432/4+1],ps=buffer.words[222];
    assert((blend>>31)==1&&((blend>>26)&31)==1&&((blend>>21)&31)==0x13);
    assert(((blend>>13)&31)==1&&((blend>>8)&31)==0x13);
    assert((ps&(1u<<29))&&((ps>>14)&31)==1&&((ps>>9)&31)==0x13);
    assert(((ps>>24)&31)==1&&((ps>>19)&31)==0x13);
    draw.blend=false;assert(gpu_build_texture_batch(buffer.words,0xFFF000,draw));
    assert(!(buffer.words[2432/4+1]&(1u<<31)));
    assert(buffer.words[GPU_RENDER_COMMAND_BYTES/4]==0x05000000);
    draw.clip.x1=-1;assert(!gpu_build_texture_batch(buffer.words,0x100000,draw));
    draw.clip.x1=0;draw.source.pitch=60;assert(!gpu_build_texture_batch(buffer.words,0x100000,draw));
    draw.source.pitch=64;assert(!gpu_build_texture_batch(buffer.words,0x100001,draw));
    draw.client={INT32_MIN,0,INT32_MAX,14};
    assert(!gpu_build_texture_batch(buffer.words,0x100000,draw));
}
static void logical_context_test() {
    uint32_t state[1024];gpu_init_render_context(state,0x2018000,0x123456000ULL);
    assert(state[1]==0x1100101B&&state[0x21]==0x11001011);
    assert(state[2]==0x2244&&state[3]==0x90009);
    assert(state[8]==0x2038&&state[9]==0x2018000);
    assert(state[0x30]==0x2274&&state[0x31]==1);
    assert(state[0x32]==0x2270&&state[0x33]==0x23456000);
    assert(state[0x42]==0x20C8&&state[0x43]==0);
    assert(state[11]==28673&&state[17]==32);
}
static void sustained_ring_test() {
    struct {uint32_t before;uint32_t words[GPU_RENDER_RING_BYTES/4];uint32_t after;} ring{};
    ring.before=0x76543210;ring.after=0xABCDEF01;
    uint32_t tail=0;
    for(uint32_t sequence=1;sequence<=2048;++sequence){
        uint32_t start=tail;
        assert(gpu_render_packet(ring.words,start,0x23021000,sequence,true,&tail));
        assert(tail==(sequence*256)%32768);
        const auto* packet=ring.words+start/4;
        assert(packet[0]==0&&packet[1]==0x11001003);
        assert(packet[2]==0x7300&&packet[3]==0x80308030);
        assert(packet[4]==0x7004&&packet[5]==0x00420042);
        assert(packet[6]==0x7A000004&&packet[7]==0);
        // Gen9 VF null-PC and KBL GAM-hang workaround are ordered around
        // invalidation: null -> DC flush -> invalidate/post-sync -> CS stall.
        // A stall before invalidation leaves the trailing workaround absent.
        assert(packet[12]==0x7A000004&&packet[13]==(1u<<5));
        assert(packet[18]==0x7A000004);
        assert(packet[19]==((1u<<24)|(1u<<20)|(1u<<18)|(1u<<14)|
                            (1u<<11)|(1u<<10)|(1u<<4)|(1u<<3)|(1u<<2)));
        assert(packet[20]==0x23021008&&packet[22]==0);
        assert(packet[24]==0x7A000004&&packet[25]==(1u<<20));
        assert(packet[26]==0&&packet[28]==0);
        assert(packet[30]==0x18800101&&packet[31]==0x100000);
        assert(packet[33]==0&&packet[34]==0x7A000004);
        assert(packet[36]==0x23021000&&packet[38]==sequence);
        for(unsigned i=40;i<64;++i)assert(packet[i]==0);
        assert(ring.before==0x76543210&&ring.after==0xABCDEF01);
    }
    assert(GPU_RENDER_CONTEXT_FLAGS&(1u<<2));
    assert(!gpu_render_packet(ring.words,1,0x23021000,1,true,&tail));
    assert(!gpu_render_packet(ring.words,32768,0x23021000,1,true,&tail));
    // Production deliberately recycles the idle engine before a tail of zero
    // can be submitted. Exercise many cycles without using hardware wrapping.
    tail=0;unsigned recycles=0;
    for(uint32_t sequence=1;sequence<=2048;++sequence){
        if(gpu_render_needs_recycle(tail)){tail=0;++recycles;}
        assert(gpu_render_packet(ring.words,tail,0x23021000,sequence,true,&tail));
        assert(tail>0&&tail<=GPU_RENDER_RING_BYTES-GPU_RENDER_PACKET_BYTES);
        assert(ring.before==0x76543210&&ring.after==0xABCDEF01);
    }
    assert(recycles==16&&tail==4096);
}
static void readback_sampling_test() {
    uint32_t batch[1024];
    for(unsigned test=0;test<3;++test){
        auto draw=gpu_render_probe_draw(test,0x10000000,0x2000000);
        assert(gpu_build_texture_batch(batch,0x100000,draw));
        const auto* vertices=batch+GPU_RENDER_VERTEX_OFFSET/4;
        float left=floating(vertices[7]),right=floating(vertices[1]);
        float top=floating(vertices[8]),bottom=floating(vertices[2]);
        int x1=(int16_t)vertices[6],y1=(int16_t)(vertices[6]>>16);
        int x2=(int16_t)vertices[0],y2=(int16_t)(vertices[0]>>16);
        for(int y=draw.clip.y1;y<draw.clip.y2;++y)
            for(int x=draw.clip.x1;x<draw.clip.x2;++x){
                float u=left+((x+.5f-x1)/(x2-x1))*(right-left);
                float v=top+((y+.5f-y1)/(y2-y1))*(bottom-top);
                float sx=u*draw.source.width,sy=v*draw.source.height;
                unsigned expected_x=(2*(x-2)+1)*draw.source.width/32;
                unsigned expected_y=(2*(y-2)+1)*draw.source.height/32;
                assert((unsigned)sx==expected_x&&(unsigned)sy==expected_y);
                assert(sx-expected_x>=.25f&&sx-expected_x<=.75f);
                assert(sy-expected_y>=.25f&&sy-expected_y<=.75f);
            }
    }
    // The previous 16-to-12 fixture sampled y=3 exactly at texel edge 2.
    assert((3-2+.5f)*16/12==2.f);
}
static void scissor_geometry_test() {
    GpuTextureDraw draw{{0x10000000,16,8,64},{0x2000000,96,64,384},
                        {-4,-2,28,14},{0,0,28,14},false};
    uint32_t original[1024],batch[1024];
    assert(gpu_build_texture_batch(original,0x100000,draw));
    const GpuRenderRect clips[]={{0,0,28,14},{1,2,19,11},{0,0,1,1},
                                 {27,0,28,14},{0,13,28,14},{7,3,8,12}};
    for(auto clip:clips){
        draw.clip=clip;assert(gpu_build_texture_batch(batch,0x100000,draw));
        const auto* vertices=batch+GPU_RENDER_VERTEX_OFFSET/4;
        assert(!memcmp(vertices,original+GPU_RENDER_VERTEX_OFFSET/4,9*4));
        int left=(int16_t)vertices[6],top=(int16_t)(vertices[6]>>16);
        int right=(int16_t)vertices[0],bottom=(int16_t)(vertices[0]>>16);
        assert(left==-4&&top==-2&&right==28&&bottom==14);
        const auto* scissor=batch+GPU_RENDER_SCISSOR_STATE/4;
        unsigned x1=scissor[0]&65535,y1=scissor[0]>>16;
        unsigned x2=(scissor[1]&65535)+1,y2=(scissor[1]>>16)+1;
        for(int y=0;y<64;++y)for(int x=0;x<96;++x){
            bool written=x>=left&&x<right&&y>=top&&y<bottom&&
                         (unsigned)x>=x1&&(unsigned)x<x2&&(unsigned)y>=y1&&(unsigned)y<y2;
            bool expected=x>=clip.x1&&x<clip.x2&&y>=clip.y1&&y<clip.y2;
            assert(written==expected);
            if(written){
                unsigned sx=(2*(x-left)+1)*16/(2*(right-left));
                unsigned sy=(2*(y-top)+1)*8/(2*(bottom-top));
                assert(sx<16&&sy<8);
            }
        }
    }
    // A cropped visible area cannot authorize unrepresentable full vertices.
    draw.destination={0x2000000,16384,16,65536};
    draw.client={16300,0,32768,16};draw.clip={16300,0,16301,1};
    assert(!gpu_build_texture_batch(batch,0x100000,draw));
}
static void scaled_occlusion_test() {
    GpuScenePlan plan{};
    plan.client_count=2;
    plan.clients[0]={{0,0,200,150},{0,20,200,150},(uint32_t*)0x1000,100,65,400,true,false,false};
    plan.clients[1]={{80,30,140,100},{80,50,140,100},(uint32_t*)0x2000,60,50,240,true,false,true};
    gpu_scaled_scene_plan(&plan,{0,0,200,150});
    assert(plan.accelerated==1&&plan.count>1);
    for(unsigned i=0;i<plan.count;++i){
        auto r=plan.copies[i].destination;
        assert(!visibility_valid(visibility_intersect(r,plan.clients[1].outer)));
        assert(r.y1>=20&&r.y2<=150);
    }
    plan.clients[0].resize_grip=true;gpu_scaled_scene_plan(&plan,{0,0,200,150});
    assert(plan.count>0&&plan.accelerated==1);
    for(unsigned i=0;i<plan.count;++i)assert(plan.copies[i].destination.y2<=138);
    plan.clients[0].rainbow=true;gpu_scaled_scene_plan(&plan,{0,0,200,150});
    assert(!plan.count&&!plan.accelerated);
    plan.clients[0].rainbow=false;plan.clients[0].resize_grip=false;
    assert(gpu_scaled_damage_plan(&plan,{0,20,70,80}));
    assert(!gpu_scaled_damage_plan(&plan,{0,0,70,80}));
    assert(!gpu_scaled_damage_plan(&plan,{90,40,120,80}));
    plan.clients[0].resize_grip=true;
    assert(!gpu_scaled_damage_plan(&plan,{0,20,70,80}));
}
static void resized_shape_test() {
    GpuScenePlan plan{};plan.client_count=1;
    // Odd sizes and mixed upscale/downscale are eligible again. The generic
    // path preserves CPU grip rows; retained client damage includes them.
    const unsigned widths[]={32,33,127,479,480,481,639,1023,1919};
    const unsigned heights[]={32,33,63,319,320,321,767,1001};
    for(unsigned w:widths)for(unsigned h:heights){
        plan.clients[0]={{0,0,(int)w,(int)h+20},{0,20,(int)w,(int)h+20},
                          (uint32_t*)0x1000,480,320,1920,true,false,true};
        gpu_scaled_scene_plan(&plan,{0,0,1920,1080});
        bool scaled=w!=480||h!=320;
        assert(plan.count==(scaled?1u:0u));
        assert(plan.accelerated==(scaled?1u:0u));
        if(scaled)assert(plan.copies[0].destination.y2==(int)h+20-12);
        gpu_scaled_scene_plan(&plan,{0,0,1920,1080},true);
        assert(plan.count==(scaled?1u:0u));
        if(scaled){
            assert(plan.copies[0].destination.y2==(int)h+20);
            auto r=plan.copies[0].destination;
            uint32_t batch[1024];
            GpuTextureDraw draw{{0x10000000,480,320,1920},{0x2000000,1920,1080,7680},
                                {0,20,(int)w,(int)h+20},{r.x1,r.y1,r.x2,r.y2},false};
            assert(gpu_build_texture_batch(batch,0x100000,draw));
            const auto* vertices=batch+GPU_RENDER_VERTEX_OFFSET/4;
            assert(vertices[0]==(w|((h+20)<<16)));
            assert(floating(vertices[1])==1.f&&floating(vertices[2])==1.f);
            assert(floating(vertices[7])==0.f&&floating(vertices[8])==0.f);
        }
        assert(!gpu_scaled_damage_plan(&plan,{0,20,(int)w,(int)h+20}));
    }
    plan.clients[0]={{0,0,1920,1040},{0,20,1920,1040},
                      (uint32_t*)0x1000,480,320,1920,true,false,false};
    assert(gpu_scaled_damage_plan(&plan,{0,20,1920,122}));
    assert(plan.count==1&&plan.accelerated==1);
}
static void retained_mixed_scene_test() {
    GpuScenePlan bcs{},rcs{};VisibleRegion background{};
    bcs.client_count=rcs.client_count=2;
    bcs.clients[0]={{0,0,100,100},{0,20,100,100},(uint32_t*)0x1000,100,80,400,true,true,true};
    bcs.clients[1]={{100,0,200,100},{100,20,200,100},(uint32_t*)0x2000,50,40,200,true,false,true};
    auto build=[&](VisibleRect damage,bool grips){
        for(unsigned i=0;i<bcs.client_count;++i)rcs.clients[i]=bcs.clients[i];
        rcs.client_count=bcs.client_count;
        gpu_scene_plan(&bcs,damage,grips);gpu_scaled_scene_plan(&rcs,damage,grips);
    };
    VisibleRect damage{0,20,200,100};
    build(damage,false);assert(!gpu_retained_damage(bcs,rcs,damage,&background));
    build(damage,true);assert(bcs.count==1&&rcs.count==1);
    assert(gpu_retained_damage(bcs,rcs,damage,&background)&&!background.count);
    // A desktop gap only costs the gap's pixels, not both windows' chrome.
    bcs.clients[1].outer.x1=110;bcs.clients[1].outer.x2=210;
    bcs.clients[1].client.x1=110;bcs.clients[1].client.x2=210;
    damage.x2=210;build(damage,true);
    assert(gpu_retained_damage(bcs,rcs,damage,&background)&&background.count==1);
    auto gap=background.rects[0];assert(gap.x1==100&&gap.x2==110&&gap.y1==20&&gap.y2==100);
    // Chrome and unsupported foreground content must still repaint on CPU.
    damage.y1=0;build(damage,true);
    assert(!gpu_retained_damage(bcs,rcs,damage,&background));
    damage.y1=20;bcs.client_count=3;
    bcs.clients[2]={{40,30,60,80},{40,40,60,80},(uint32_t*)0x3004,20,40,80,true,false,true};
    build(damage,true);assert(!gpu_retained_damage(bcs,rcs,damage,&background));
    assert(!gpu_scene_pixel_exposed(bcs,0,50,50));
    assert(gpu_scene_pixel_exposed(bcs,0,98,98));
    // Invalid copy bounds cannot authorize a retained update.
    bcs.client_count=2;build(damage,true);rcs.copies[0].destination.x2=211;
    assert(!gpu_retained_damage(bcs,rcs,damage,&background));
}
int main(){reset_handshake_test();texture_state_test();logical_context_test();sustained_ring_test();readback_sampling_test();scissor_geometry_test();scaled_occlusion_test();resized_shape_test();retained_mixed_scene_test();
    puts("Gen9 texture/scaling/alpha state, logical contexts and occlusion tests passed.");}
