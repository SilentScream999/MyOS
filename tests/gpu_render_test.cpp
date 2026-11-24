#include <cassert>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include "src/drivers/gpu_render_commands.h"
#include "src/drivers/gpu_render_reset.h"
#include "src/drivers/gpu_gen9_workarounds.h"
#include "src/gui/gpu_compositor.h"
#include "src/gui/window_alpha.h"
#include "src/drivers/gpu_upload_cache.h"
#include "src/drivers/gpu_alpha_coverage.h"
#include "src/gui/gpu_deferred_alpha.h"

static float floating(uint32_t word) { float f;memcpy(&f,&word,4);return f; }
static void gen9_workaround_test() {
    struct IO {
        uint32_t regs[0xC000/4]{};
        unsigned writes=0;
        uint32_t reject=0;
        IO(){
            regs[0xFDC/4]=0xDFFFFFFF;
            regs[0xB004/4]=0x12340001;
            regs[0xB008/4]=0x12340001;
            regs[0xB118/4]=0x80400040;
            regs[0xB11C/4]=0x12340100;
            regs[0x20D4/4]=0x80;
        }
        uint32_t read(uint32_t reg){return regs[reg/4];}
        void write(uint32_t reg,uint32_t value){
            ++writes;
            if(reg==reject)return;
            if(reg==0x20D4){
                uint32_t mask=value>>16;
                regs[reg/4]=(regs[reg/4]&~mask)|(value&mask);
            }else regs[reg/4]=value;
        }
    } io;
    // Slice 1, subslice 2: steer away from fused-off instances.
    uint32_t fuse=(1u<<26)|(3u<<20);
    assert(gpu_apply_gen9_engine_workarounds(io,fuse));
    assert(io.read(0xFDC)==0xD6FFFFFF);
    assert(io.read(0xB004)==0x12340081);
    assert(io.read(0xB008)==0x12340000);
    assert(io.read(0xB118)==0x80200040);
    assert(io.read(0xB11C)==0x12340004);
    assert(io.read(0x20D4)==0x84);
    // Reapplying after context initialization preserves the same settings.
    assert(gpu_apply_gen9_engine_workarounds(io,fuse));
    IO missing;
    assert(!gpu_apply_gen9_engine_workarounds(missing,0));
    assert(!gpu_apply_gen9_engine_workarounds(missing,UINT32_MAX));
    assert(!gpu_apply_gen9_engine_workarounds(missing,(1u<<25)|(15u<<20)));
    assert(missing.writes==0);
    IO failed;failed.reject=0xB118;
    assert(!gpu_apply_gen9_engine_workarounds(failed,fuse));
    assert(failed.read(0x20D4)==0x80);
}
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
    assert(source[1]==0&&buffer.words[GPU_RENDER_DST_STATE/4+1]==0);
    draw.cache_source=true;
    assert(gpu_build_texture_batch(buffer.words,0x100000,draw));
    assert(buffer.words[GPU_RENDER_SRC_STATE/4+1]==(2u<<24));
    assert(buffer.words[GPU_RENDER_DST_STATE/4+1]==0); // No cached destination.
    draw.cache_source=false;
    assert(gpu_build_texture_batch(buffer.words,0x100000,draw));
    assert(buffer.words[18]==0x100001&&buffer.words[20]==0x100001&&buffer.words[24]==0x100001);
    assert(buffer.words[262]==0x100000+GPU_RENDER_VERTEX_OFFSET);
    assert(buffer.words[258]==(63u<<16|95));
    const auto* v=buffer.words+GPU_RENDER_VERTEX_OFFSET/4;
    assert(buffer.words[264]==72&&buffer.words[273]==4&&buffer.words[281]==6);
    assert((buffer.words[215]>>23)==15&&(buffer.words[215]&2));
    uint32_t dispatch_flags=buffer.words[215]&0x007fffffu;
    for(unsigned threads:{16u,32u,63u}){
        draw.threads=threads;assert(gpu_build_texture_batch(buffer.words,0x100000,draw));
        assert((buffer.words[215]>>23)==threads-1);
        assert((buffer.words[215]&0x007fffffu)==dispatch_flags);
        assert(buffer.words[GPU_RENDER_DST_STATE/4+1]==0);
    }
    for(unsigned threads:{0u,15u,64u}){
        draw.threads=threads;assert(!gpu_build_texture_batch(buffer.words,0x100000,draw));
    }
    draw.threads=16;assert(gpu_build_texture_batch(buffer.words,0x100000,draw));
    assert(gpu_render_dispatch_faster(800,700));
    assert(!gpu_render_dispatch_faster(800,701));
    assert(!gpu_render_dispatch_faster(0,100)&&!gpu_render_dispatch_faster(800,0));
    assert(gpu_render_dispatch_faster(UINT64_MAX,UINT64_MAX/2));
    assert(v[9]==v[0]&&v[12]==v[6]&&v[15]==20);
    assert(floating(v[16])==.75f&&floating(v[17])==.125f);
    assert(v[0]==(12u<<16|20)&&v[3]==(12u<<16)&&v[6]==0);
    assert(floating(v[1])==.75f&&floating(v[2])==.875f);
    assert(floating(v[4])==.125f&&floating(v[8])==.125f);
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
    assert(gpu_build_texture_batch(buffer.words,0x100000,draw,true));
    assert(buffer.words[210]==2816&&buffer.words[736]==0x05800031);
    assert(buffer.words[739]==0x90031000);
    assert(buffer.words[609]==0x06200000); // Solid code preserves blend state.
    draw.clip.x1=-1;assert(!gpu_build_texture_batch(buffer.words,0x100000,draw));
    draw.clip.x1=0;draw.source.pitch=60;assert(!gpu_build_texture_batch(buffer.words,0x100000,draw));
    draw.source.pitch=64;assert(!gpu_build_texture_batch(buffer.words,0x100001,draw));
    draw.client={INT32_MIN,0,INT32_MAX,14};
    assert(!gpu_build_texture_batch(buffer.words,0x100000,draw));
    draw={{0x10000000,480,320,1920},{0x2000000,128,128,512},
          {-768,-640,83,66},{0,0,128,128},false};
    assert(!gpu_build_texture_batch(buffer.words,0x100000,draw));
    draw.padded=true;
    assert(gpu_build_texture_batch(buffer.words,0x100000,draw));
    const auto* tile=buffer.words+GPU_RENDER_VERTEX_OFFSET/4;
    assert(tile[0]==(128u<<16|128)&&tile[6]==0);
    assert(floating(tile[7])==768.f/851&&floating(tile[8])==640.f/706);
    assert(gpu_build_texture_batch(buffer.words,0x100000,draw,true));
    assert(floating(tile[7])==0.f&&floating(tile[8])==0.f);
    assert(floating(tile[1])==1.f&&floating(tile[2])==1.f);
}
static void logical_context_test() {
    uint32_t eu_disable[3]={0xff000001,0,0},request=0;
    // Three subslices with 7/8/8 EUs. Ignore the disabled fourth subslice.
    assert(gpu_render_power_request((1u<<25)|(8u<<20),eu_disable,request));
    assert(request==0x80000088);
    uint32_t state[1024];gpu_init_render_context(state,0x2018000,0x123456000ULL,request);
    assert(state[1]==0x1100101B&&state[0x21]==0x11001011);
    assert(state[2]==0x2244&&state[3]==0x90009);
    assert(state[8]==0x2038&&state[9]==0x2018000);
    assert(state[0x30]==0x2274&&state[0x31]==1);
    assert(state[0x32]==0x2270&&state[0x33]==0x23456000);
    assert(state[0x42]==0x20C8&&state[0x43]==0x80000088);
    assert(gpu_render_power_request((3u<<25)|(8u<<20),eu_disable,request)&&request==0x80050088);
    assert(!gpu_render_power_request(0,eu_disable,request));
    assert(!gpu_render_power_request(UINT32_MAX,eu_disable,request));
    assert(!gpu_render_power_request(1u<<25,eu_disable,request));
    assert(state[11]==28673&&state[17]==32);
}
static void sustained_ring_test() {
    struct {uint32_t before;uint32_t words[GPU_RENDER_RING_BYTES/4];uint32_t after;} ring{};
    ring.before=0x76543210;ring.after=0xABCDEF01;
    constexpr unsigned slots=GPU_RENDER_RING_BYTES/GPU_RENDER_PACKET_BYTES-1;
    uint32_t latest[slots]{};
    uint32_t tail=0;
    for(uint32_t sequence=1;sequence<=2048;++sequence){
        uint32_t start=tail;
        assert(gpu_render_packet(ring.words,start,0x23021000,sequence,true,&tail));
        assert(tail==(((sequence-1)%slots)+1)*GPU_RENDER_PACKET_BYTES);
        if(start==GPU_RENDER_RING_BYTES-GPU_RENDER_PACKET_BYTES){
            for(unsigned i=start/4;i<GPU_RENDER_RING_BYTES/4;++i)assert(ring.words[i]==0);
            start=0;
        }
        latest[start/GPU_RENDER_PACKET_BYTES]=sequence;
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
        assert(packet[20]==0x23021008&&packet[21]==0&&packet[22]==sequence&&packet[23]==0);
        assert(packet[24]==0x7A000004&&packet[25]==(1u<<20));
        assert(packet[26]==0&&packet[28]==0);
        assert(packet[30]==0x18800101&&packet[31]==0x100000);
        assert(packet[33]==0&&packet[34]==0x10400002);
        assert(packet[35]==0x23021010&&packet[36]==0&&packet[37]==sequence);
        assert(packet[38]==0x7A000004);
        assert(packet[39]==((1u<<20)|(1u<<1)));
        assert(packet[40]==0&&packet[42]==0);
        assert(packet[44]==0x10400002&&packet[45]==0x23021018&&packet[47]==sequence);
        assert(packet[48]==0x7A000004&&
               packet[49]==((1u<<20)|(1u<<12)));
        // The cache drain has no post-sync write or address-space selector.
        // Its retirement stamp and completion QW follow as separate commands.
        assert(packet[50]==0&&packet[51]==0&&packet[52]==0&&packet[53]==0);
        assert(packet[54]==0x10400002&&packet[55]==0x23021020&&packet[57]==sequence);
        assert(packet[58]==0x7A000004&&packet[59]==((1u<<20)|(1u<<7)|(1u<<5)|1u));
        assert(packet[60]==0&&packet[62]==0&&packet[63]==0);
        assert(packet[64]==0x10400002&&packet[65]==0x23021014&&packet[67]==sequence);
        assert(packet[68]==0x7A000004&&packet[69]==((1u<<24)|(1u<<20)|(1u<<14)));
        assert(packet[70]==0x23021000&&packet[72]==sequence&&packet[73]==0);
        assert(ring.before==0x76543210&&ring.after==0xABCDEF01);
    }
    assert((GPU_RENDER_CONTEXT_FLAGS&7)==1); // Valid, without forced restores.
    assert(!gpu_render_packet(ring.words,1,0x23021000,1,true,&tail));
    assert(!gpu_render_packet(ring.words,32768,0x23021000,1,true,&tail));
    for(unsigned slot=0;slot<slots;++slot)
        assert(ring.words[slot*GPU_RENDER_PACKET_BYTES/4+72]==latest[slot]);
    assert(gpu_render_packet(ring.words,0,0x23021000,2049,true,&tail,false));
    assert(!(ring.words[19]&(1u<<18)));
    assert(ring.words[19]&(1u<<4)); // Vertex cache still invalidated.
    assert(gpu_render_packet(ring.words,0,0x23021000,2050,true,&tail,true,0x40012000));
    assert(ring.words[31]==0x40012000);
    assert(!gpu_render_packet(ring.words,0,0x23021000,2051,true,&tail,true,0x40012001));
    // Keep the entire reserved 40-byte checkpoint allocation addressable.
    assert(!gpu_render_packet(ring.words,0,0xFFFFFFE8u,2052,true,&tail));
    assert(!gpu_render_packet(ring.words,0,0xFFFFFFE0u,2052,true,&tail));
    assert(gpu_render_packet(ring.words,0,0xFFFFFFD8u,2052,true,&tail));
    assert(ring.words[50]==0);
    uint32_t baseline[GPU_RENDER_PACKET_BYTES/4]{},instrumented[GPU_RENDER_PACKET_BYTES/4]{};
    assert(gpu_render_packet(baseline,0,0x23021000,0x1234,true,&tail));
    assert(gpu_render_packet(instrumented,0,0x23021000,0x1234,true,&tail,true,0x100000,0x23021040));
    unsigned stamp_count=0,plain_index=0;
    for(unsigned i=0;i<GPU_RENDER_PACKET_BYTES/4;){
        if(instrumented[i]==0x12400002){
            assert(i+3<GPU_RENDER_PACKET_BYTES/4&&instrumented[i+1]==0x2358);
            assert(instrumented[i+2]==0x23021040+stamp_count*4&&instrumented[i+3]==0);
            ++stamp_count;i+=4;
        }else assert(instrumented[i++]==baseline[plain_index++]);
    }
    assert(stamp_count==5&&plain_index==GPU_RENDER_PACKET_BYTES/4-20);
    assert(!gpu_render_packet(instrumented,0,0x23021000,0x1234,true,&tail,true,0x100000,0x23021000));
    assert(!gpu_render_packet(instrumented,0,0x23021000,0x1234,true,&tail,true,0x100000,0x23021044));
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
    uint32_t batch[1024];
    const GpuRenderRect clips[]={{0,0,28,14},{1,2,19,11},{0,0,1,1},
                                 {27,0,28,14},{0,13,28,14},{7,3,8,12}};
    for(auto clip:clips){
        draw.clip=clip;assert(gpu_build_texture_batch(batch,0x100000,draw));
        const auto* vertices=batch+GPU_RENDER_VERTEX_OFFSET/4;
        int left=(int16_t)vertices[6],top=(int16_t)(vertices[6]>>16);
        int right=(int16_t)vertices[0],bottom=(int16_t)(vertices[0]>>16);
        assert(left==clip.x1&&top==clip.y1&&right==clip.x2&&bottom==clip.y2);
        assert(floating(vertices[7])==(left+4.f)/32);
        assert(floating(vertices[8])==(top+2.f)/16);
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
static void window_alpha_test() {
    // Every alpha and destination byte, including exact transparent/opaque
    // endpoints. Expected channels are evaluated independently in double.
    for(unsigned a=0;a<256;++a)for(unsigned d=0;d<256;++d){
        unsigned s=(a*73+127)/255;
        uint32_t source=(a<<24)|(s<<16)|(s<<8)|s;
        unsigned expected=(unsigned)(s+d*(1.0-a/255.0)+0.5);
        uint32_t background=0xff000000|(d<<16)|(d<<8)|d;
        assert(window_source_over(source,background)==(0xff000000|(expected<<16)|(expected<<8)|expected));
    }
    assert(window_source_over(0x80402010,0xff204080)==0xff504050);
    assert(window_source_over(0,0xff123456)==0xff123456);
    assert(window_source_over(0xffabcdef,0xff123456)==0xffabcdef);
    // Reconstruct the background for each repaint; repeated blending onto
    // yesterday's output would accumulate opacity and produce a different pixel.
    uint32_t background=0xff2468ac;
    auto expected=window_source_over(0x80402010,background);
    assert(window_source_over(0x80402010,expected)!=expected);

    alignas(64) uint32_t pixels[64*64]{};
    GpuSceneClient client{{-12,-8,116,136},{-12,8,116,136},pixels,64,64,256,true,false,true,true};
    GpuScenePlan alpha{};
    assert(gpu_alpha_scene_plan(&alpha,client,{0,20,100,136}));
    assert(alpha.count==1&&alpha.clients[0].premultiplied);
    auto clip=alpha.copies[0].destination;
    assert(clip.x1==0&&clip.x2==100&&clip.y1==20&&clip.y2==124);
    client.resize_grip=false;
    assert(gpu_alpha_scene_plan(&alpha,client,{0,20,100,136}));
    assert(alpha.copies[0].destination.y2==136);
    client.client={0,8,64,72}; // 1:1 alpha must still use the render engine
    assert(gpu_alpha_scene_plan(&alpha,client,{0,8,64,72}));
    client.pixels=pixels+1;assert(!gpu_alpha_scene_plan(&alpha,client,{0,8,64,72}));
    client.pixels=pixels;client.premultiplied=false;
    assert(!gpu_alpha_scene_plan(&alpha,client,{0,8,64,72}));
    client.premultiplied=true;
    assert(!gpu_alpha_scene_plan(&alpha,client,{64,8,65,72}));

    GpuScenePlan bcs{},rcs{};bcs.client_count=2;
    bcs.clients[0]={{0,0,64,80},{0,16,64,80},pixels,64,64,256,true,true,false,false};
    bcs.clients[1]=client;
    VisibleRect damage{0,20,64,64};
    gpu_scene_plan(&bcs,damage);assert(!bcs.count&&!bcs.accelerated);
    rcs=bcs;gpu_scaled_scene_plan(&rcs,damage);assert(!rcs.count&&!rcs.accelerated);
    assert(!gpu_scaled_damage_plan(&rcs,damage));
    assert(!gpu_rain_damage_plan(&bcs,damage));
    VisibleRegion gaps{};assert(!gpu_retained_damage(bcs,rcs,damage,&gaps));
    bcs.clients[1].visible=false;
    gpu_scene_plan(&bcs,damage);assert(bcs.count&&bcs.accelerated==1);
    // Transparent/shadow scenes retain opaque blits and the resident rainbow
    // palette instead of regenerating those pixels on the CPU.
    GpuScenePlan ordered{};client.visible=true;client.premultiplied=false;
    assert(gpu_ordered_scene_plan(&ordered,client,{0,8,64,72})==1);
    client.rainbow=true;
    assert(gpu_ordered_scene_plan(&ordered,client,{0,8,64,72})==1);
    client.rainbow=false;client.client.x2=128;
    assert(gpu_ordered_scene_plan(&ordered,client,{0,8,128,72})==2);
    client.premultiplied=true;
    assert(gpu_ordered_scene_plan(&ordered,client,{0,8,128,72})==2);
    // Keep only visible opaque-client pixels, while preserving source offsets
    // and leaving the background beneath higher shadows/alpha available.
    client.premultiplied=false;client.client={10,8,74,72};
    assert(gpu_ordered_scene_plan(&ordered,client,{20,18,70,62})==1);
    VisibleRect covers[]={{35,18,50,62},{20,30,70,40}};
    gpu_ordered_occlude(&ordered,covers,2);
    assert(ordered.count==4);
    unsigned area=0;
    for(unsigned i=0;i<ordered.count;++i){
        const auto& copy=ordered.copies[i];auto r=copy.destination;
        assert(copy.source_x==(unsigned)(r.x1-10)&&copy.source_y==(unsigned)(r.y1-8));
        area+=(r.x2-r.x1)*(r.y2-r.y1);
        for(auto cover:covers)assert(!visibility_valid(visibility_intersect(r,cover)));
    }
    assert(area==35*34);
    VisibleRect full{0,0,128,100};
    assert(gpu_ordered_scene_plan(&ordered,client,{20,18,70,62})==1);
    gpu_ordered_occlude(&ordered,&full,1);assert(ordered.count==0);
}
static void immutable_upload_test() {
    GpuUploadCache cache;
    uint32_t source=0xff112233,staged=0;
    unsigned uploads=0;
    auto upload=[&](uint64_t revision,uint64_t epoch){
        if(!cache.matches(&source,sizeof(source),revision,epoch)){
            staged=source;++uploads;cache.record(&source,sizeof(source),revision,epoch);
        }
    };
    upload(1,0);assert(staged==source&&uploads==1);
    for(unsigned frame=0;frame<10000;++frame)upload(1,0);
    assert(uploads==1);
    // The same publication slot may be overwritten later at the same address.
    source=0xff445566;upload(3,0);assert(staged==source&&uploads==2);
    upload(3,1);assert(uploads==3); // retired backing invalidates reuse
    source=0xff778899;upload(0,1);assert(staged==source&&uploads==4);
    source=0xffaabbcc;upload(0,1);assert(staged==source&&uploads==5);
    assert(!cache.matches(&staged,sizeof(source),3,1));
    assert(!cache.matches(&source,sizeof(source)+4,3,1));
}
static void animation_batch_test() {
    VisibleRegion exposed{};VisibleRect bounds;
    assert(!gpu_animation_damage_bounds(exposed,&bounds));
    exposed.count=3;
    exposed.rects[0]={10,30,200,50};exposed.rects[1]={10,50,60,160};exposed.rects[2]={150,50,200,160};
    assert(gpu_animation_damage_bounds(exposed,&bounds));
    assert(bounds.x1==10&&bounds.y1==30&&bounds.x2==200&&bounds.y2==160);
    // The bounding scene does not reintroduce opaque covered GPU pixels.
    alignas(64) uint32_t pixels[190*130]{};
    GpuSceneClient rain{{10,10,200,160},{10,30,200,160},pixels,190,130,760,true,true,false};
    GpuScenePlan plan{};assert(gpu_ordered_scene_plan(&plan,rain,bounds)==1);
    VisibleRect cover{60,50,150,160};gpu_ordered_occlude(&plan,&cover,1);
    unsigned area=0;for(unsigned i=0;i<plan.count;++i){auto r=plan.copies[i].destination;
        assert(!visibility_valid(visibility_intersect(r,cover)));area+=(r.x2-r.x1)*(r.y2-r.y1);}
    assert(area==190*20+100*110);
    exposed.count=VisibleRegion::capacity+1;assert(!gpu_animation_damage_bounds(exposed,&bounds));
}
static void alpha_coverage_test() {
    constexpr unsigned width=64,height=64,pitch=68;
    uint32_t pixels[height][pitch]{};
    GpuAlphaCoverage coverage;
    GpuTextureDraw draw{{0x10000000,width,height,pitch*4},{0x2000000,192,128,192*4},
                        {-3,-6,181,121},{0,0,171,111},true};
    GpuRenderRect clips[GPU_RENDER_MAX_RECTS];
    auto scan=[&]{coverage.scan(reinterpret_cast<const uint8_t*>(pixels),width,height,pitch*4);};
    scan();
    assert(coverage.clips(draw,clips)==0);
    for(unsigned y=0;y<height;++y)
        for(unsigned x=y<44?21:0;x<width;++x)pixels[y][x]=0x80402010;
    for(unsigned y=0;y<height;++y)pixels[y][1]=0xff000000; // opaque border encloses the gap
    pixels[3][2]=1; // malformed zero alpha still affects the GPU blend
    scan();
    unsigned n=coverage.clips(draw,clips);
    assert(n>1&&n<=GPU_RENDER_MAX_RECTS);
    unsigned skipped=0;
    for(int y=draw.clip.y1;y<draw.clip.y2;++y)for(int x=draw.clip.x1;x<draw.clip.x2;++x){
        unsigned sx=(2*(x-draw.client.x1)+1)*width/(2*(draw.client.x2-draw.client.x1));
        unsigned sy=(2*(y-draw.client.y1)+1)*height/(2*(draw.client.y2-draw.client.y1));
        unsigned covered=0;
        for(unsigned i=0;i<n;++i)
            covered+=x>=clips[i].x1&&x<clips[i].x2&&y>=clips[i].y1&&y<clips[i].y2;
        assert(covered<=1);
        if(pixels[sy][sx])assert(covered==1);
        if(!covered)++skipped;
    }
    assert(skipped>2000);
    uint32_t batch[1024];
    assert(gpu_build_texture_batch(batch,0x100000,draw,false,clips,n));
    assert(batch[264]==n*72&&batch[281]==n*6);
    for(unsigned i=0;i<n;++i)
        assert(batch[GPU_RENDER_VERTEX_OFFSET/4+18*i]==((unsigned)clips[i].x2|((unsigned)clips[i].y2<<16)));
    clips[1]=clips[0];
    assert(!gpu_build_texture_batch(batch,0x100000,draw,false,clips,n));
    for(auto& row:pixels)for(auto& pixel:row)pixel=0x80402010;
    scan();
    auto upper=draw,lower=draw;
    upper.clip.y2=55;lower.clip.y1=55;
    n=0;
    assert(coverage.append_clips(upper,clips,n));
    assert(coverage.append_clips(lower,clips,n));
    assert(gpu_build_texture_batch(batch,0x100000,draw,false,clips,n));
    for(int y=0;y<111;++y)for(int x=0;x<171;++x){
        unsigned hits=0;
        for(unsigned i=0;i<n;++i)
            hits+=x>=clips[i].x1&&x<clips[i].x2&&y>=clips[i].y1&&y<clips[i].y2;
        assert(hits==1); // merged fragments blend each destination once
    }
    unsigned full=GPU_RENDER_MAX_RECTS;
    auto first=clips[0];
    assert(!coverage.append_clips(draw,clips,full));
    assert(full==GPU_RENDER_MAX_RECTS && clips[0].x1==first.x1 && clips[0].y2==first.y2);
}
static void alpha_merge_test(){
    // A ring-shaped union must retain its hole; a partial shared edge cannot
    // be expanded into a bounding box. Check every pixel, including overlap.
    GpuRenderRect original[]={{2,2,18,5},{2,5,5,15},{15,5,18,15},{2,15,18,18},
                             {22,2,25,5},{25,2,28,5},{28,2,31,5}};
    GpuRenderRect merged[32];memcpy(merged,original,sizeof(original));
    unsigned n=gpu_alpha_merge_rects(merged,7);assert(n==5);
    for(int y=0;y<32;++y)for(int x=0;x<32;++x){
        unsigned before=0,after=0;
        for(auto r:original)before+=x>=r.x1&&x<r.x2&&y>=r.y1&&y<r.y2;
        for(unsigned i=0;i<n;++i){auto r=merged[i];after+=x>=r.x1&&x<r.x2&&y>=r.y1&&y<r.y2;}
        assert(before==after&&after<=1);
    }
    GpuRenderRect bridge[]={{0,0,5,5},{10,0,15,5},{5,0,10,5},{0,5,15,10}};
    assert(gpu_alpha_merge_rects(bridge,4)==1);
    assert(bridge[0].x1==0&&bridge[0].y1==0&&bridge[0].x2==15&&bridge[0].y2==10);

    constexpr unsigned W=192,H=128;
    uint32_t pixels[64*64];for(auto& pixel:pixels)pixel=0x80402010;
    GpuAlphaCoverage coverage;coverage.scan((const uint8_t*)pixels,64,64,256);
    GpuTextureDraw draw{{0x10000000,64,64,256},{0x2000000,W,H,W*4},
                         {0,0,W,H},{0,0,W,H},true};
    GpuRenderRect full[16];for(unsigned i=0;i<16;++i)full[i]={(int)i*10,0,(int)i*10+5,10};
    auto next=draw;next.clip={5,0,10,10};n=16;
    assert(coverage.append_clips(next,full,n,true)&&n==15);
    uint32_t batch[1024];assert(gpu_build_texture_batch(batch,0x100000,draw,false,full,n));
    for(unsigned i=0;i<16;++i)full[i]={(int)i*10,0,(int)i*10+5,10};
    GpuRenderRect saved[16];memcpy(saved,full,sizeof(saved));n=16;
    next.clip={180,30,185,40};
    assert(!coverage.append_clips(next,full,n,true));
    assert(n==16&&!memcmp(saved,full,sizeof(saved)));

    // Model opaque retention inside the mixed-alpha test. The legacy band
    // clipping must generate exactly the same pixels after coalescing, with
    // fewer context submissions and no extra vertex/batch storage.
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)
        pixels[y*64+x]=x<16?0:x<42?0x80402010:0xff123456;
    coverage.scan((const uint8_t*)pixels,64,64,256);
    GpuRenderRect fragments[]={{0,0,192,4},{0,124,192,128},{0,4,130,124},{188,4,192,124}};
    uint8_t hits[2][W*H]{};unsigned batches[2]{},rectangles[2]{};
    for(unsigned mode=0;mode<2;++mode){
        GpuRenderRect group[16];unsigned count=0;
        auto flush=[&](){if(!count)return;
            assert(gpu_build_texture_batch(batch,0x100000,draw,false,group,count));
            ++batches[mode];rectangles[mode]+=count;
            for(unsigned i=0;i<count;++i){auto r=group[i];
                const auto* v=batch+GPU_RENDER_VERTEX_OFFSET/4+i*18;
                assert(floating(v[1])==(float)r.x2/W&&floating(v[2])==(float)r.y2/H);
                for(int y=r.y1;y<r.y2;++y)for(int x=r.x1;x<r.x2;++x)++hits[mode][y*W+x];
            }
            count=0;
        };
        for(auto r:fragments){auto part=draw;part.clip=r;
            if(!coverage.append_clips(part,group,count,mode!=0)){
                flush();assert(coverage.append_clips(part,group,count,mode!=0));
            }
        }
        flush();
    }
    assert(!memcmp(hits[0],hits[1],W*H));
    for(auto hit:hits[1])assert(hit<=1);
    assert(batches[1]<batches[0]&&rectangles[1]<rectangles[0]);
    printf("Alpha merge fixture: batches %u -> %u, rectangles %u -> %u, identical pixel coverage.\n",
           batches[0],batches[1],rectangles[0],rectangles[1]);
}
static void deferred_alpha_test(){
    alignas(64) uint32_t pixels[64*64];for(auto& p:pixels)p=0x80402010;
    GpuSceneClient client{{0,0,64,64},{0,0,64,64},pixels,64,64,256,true,false,false,true,17};
    GpuScenePlan animation{};animation.count=1;animation.copies[0]={0,{0,20,64,64},0,0};
    GpuScenePlan repair{};repair.client_count=1;repair.clients[0]=client;
    repair.count=1;repair.copies[0]={0,{0,0,64,20},0,0};
    GpuDeferredAlpha deferred;deferred.begin(client,true);
    assert(deferred.append(repair,animation));deferred.active=false;
    assert(deferred.ready(client,animation));
    GpuScenePlan alpha{};assert(gpu_retained_alpha_plan(&alpha,client,animation));
    assert(deferred.combine(&alpha)&&alpha.count==2);
    GpuAlphaCoverage coverage;coverage.scan((const uint8_t*)pixels,64,64,256);
    GpuTextureDraw draw{{0x10000000,64,64,256},{0x2000000,64,64,256},
                        {0,0,64,64},{0,0,64,64},true};
    GpuRenderRect rects[16];unsigned count=0;
    for(unsigned i=0;i<alpha.count;++i){auto part=draw;auto r=alpha.copies[i].destination;
        part.clip={r.x1,r.y1,r.x2,r.y2};assert(coverage.append_clips(part,rects,count,true));}
    assert(count==1&&rects[0].y1==0&&rects[0].y2==64);
    uint32_t batch[1024];assert(gpu_build_texture_batch(batch,0x100000,draw,false,rects,count));
    auto changed=client;++changed.revision;
    assert(!deferred.ready(changed,animation));
    changed=client;++changed.client.x1;assert(!deferred.ready(changed,animation));
    auto collision=animation;collision.copies[0].destination={0,10,64,64};
    assert(!deferred.ready(client,collision));
    deferred.begin(client,true);assert(deferred.append(repair,animation));
    assert(!deferred.append(repair,animation)&&deferred.failed&&deferred.count==1);
    assert(!deferred.ready(client,animation));
    deferred.begin(client,false);assert(!deferred.active&&!deferred.append(repair,animation));
    changed=client;changed.revision=0;deferred.begin(changed,true);assert(!deferred.active);
    changed=client;changed.resize_grip=true;changed.client.y1=changed.client.y2-8;
    deferred.begin(changed,true);assert(!deferred.active);
    deferred.begin(client,true);repair.copies[0].destination={0,10,64,30};
    assert(!deferred.append(repair,animation)&&!deferred.count);
    changed=client;changed.resize_grip=true;deferred.begin(changed,true);
    repair.clients[0]=changed;repair.copies[0].destination={0,50,64,60};animation.count=0;
    assert(!deferred.append(repair,animation));
    deferred.begin(client,true);repair.clients[0]=client;repair.count=64;
    for(unsigned i=0;i<64;++i)repair.copies[i]={0,{(int)(i%8)*4,(int)(i/8)*4,(int)(i%8)*4+1,(int)(i/8)*4+1},0,0};
    assert(deferred.append(repair,animation)&&deferred.count==64);
    repair.count=1;repair.copies[0]={0,{40,40,41,41},0,0};
    assert(!deferred.append(repair,animation)&&deferred.failed&&deferred.count==64);
    assert(!deferred.ready(client,animation));
    deferred.clear();assert(!deferred.active&&!deferred.failed&&!deferred.count);
    VisibleRect opaque{10,10,40,40};
    deferred.begin(client,true);animation.count=0;
    assert(!deferred.append_opaque({9,12,20,20},&opaque,1,animation));
    assert(deferred.append_opaque({10,10,40,40},&opaque,1,animation)&&deferred.count==1);
    assert(!deferred.ready(changed,animation));
    deferred.begin(client,true);animation.count=1;animation.copies[0].destination={20,20,30,30};
    assert(!deferred.append_opaque(opaque,&opaque,1,animation)&&!deferred.count);
    puts("Deferred alpha: repair + animation fit one existing batch; source/layout/overlap/grip guards passed.");
}
int main(){gen9_workaround_test();reset_handshake_test();texture_state_test();logical_context_test();sustained_ring_test();readback_sampling_test();scissor_geometry_test();scaled_occlusion_test();resized_shape_test();retained_mixed_scene_test();window_alpha_test();immutable_upload_test();animation_batch_test();alpha_coverage_test();alpha_merge_test();deferred_alpha_test();
    puts("Gen9 texture/scaling/alpha state, logical contexts and occlusion tests passed.");}
