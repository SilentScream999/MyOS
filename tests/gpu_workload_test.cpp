#include <cassert>
#include <cstdio>
#include <cstring>
#include "src/drivers/gpu_scene_commands.h"
#include "src/drivers/gpu_render_commands.h"
#include "src/drivers/gpu_perf.h"
#include "src/gui/gpu_occlusion_metrics.h"
static void occlusion_metrics_test(){
    VisibleRect screen{0,0,200,120};
    GpuOcclusionLayer layers[]={
        {{120,10,200,100},{120,20,200,100},true},
        {{10,30,130,110},{10,40,130,110},false},
        {{20,50,80,120},{20,60,80,120},false}};
    GpuOcclusionMetric metrics[3];
    assert(gpu_occlusion_metrics(screen,screen,layers,3,metrics));
    assert(metrics[0].cover_pixels==800&&metrics[0].newly_hidden_pixels==800);
    // A narrow overlap with the alpha client can still cover a large part
    // of Rainbow elsewhere. These areas have distinct meanings.
    assert(metrics[1].cover_pixels==9600&&metrics[1].newly_hidden_pixels==9600);
    assert(metrics[1].lower_alpha_client_pixels==700);
    assert(metrics[2].cover_pixels==4200&&metrics[2].newly_hidden_pixels==600);
    // Independently paint visibility to check clipping, union and accounting.
    uint8_t hidden[200*120]{};
    for(unsigned i=0;i<3;++i){auto cover=layers[i].outer;if(layers[i].alpha)cover.y2=layers[i].client.y1;
        uint64_t newly=0;
        for(int y=0;y<120;++y)for(int x=0;x<200;++x)
            if(x>=cover.x1&&x<cover.x2&&y>=cover.y1&&y<cover.y2&&!hidden[y*200+x]){
                hidden[y*200+x]=1;++newly;
            }
        assert(newly==metrics[i].newly_hidden_pixels);
    }
    GpuOcclusionLayer offscreen{{-50,-20,50,20},{-50,-10,50,20},false};
    assert(gpu_occlusion_metrics(screen,screen,&offscreen,1,metrics));
    assert(metrics[0].cover_pixels==1000&&metrics[0].newly_hidden_pixels==1000);
}
struct Fixture {
    uint32_t commands[8192]{},regs[128]{};
    GpuRing ring;
    Fixture(){ring.cpu=commands;ring.regs=regs;ring.size=sizeof(commands);ring.ready=true;ring.fence_gfx=0x1000;}
};
static void encode(Fixture& f){
    auto* r=&f.ring;
    assert(gpu_emit_fill(r,0x10000,4096,3,4,103,24,0xff123456));
    assert(gpu_emit_copy(r,0x10000,4096,0,0,0x20000,4096,10,10,80,30));
    {
        GpuCopyScope client(r,GpuCopyKind::Client);
        assert(gpu_emit_copy(r,0x10000,4096,0,0,0x20000,4096,0,0,60,20));
        {
            GpuCopyScope scanout(r,GpuCopyKind::Scanout);
            assert(gpu_emit_copy(r,0x10000,4096,0,0,0x20000,4096,0,0,1024,600));
        }
        assert(r->copy_kind==GpuCopyKind::Client);
        // Invalid commands must not contribute to encoded payload counters.
        assert(!gpu_emit_copy(r,1,4096,0,0,0x20000,4096,0,0,20,20));
        assert(!gpu_emit_fill(r,0x10000,4096,20,20,10,10,0));
    }
    assert(r->copy_kind==GpuCopyKind::Other);
    {
        GpuCopyScope rainbow(r,GpuCopyKind::Rainbow);
        GpuSceneCopy copy{};copy.destination={13,17,1013,617};copy.source_x=700;
        assert(gpu_emit_rainbow(r,0x30000,0x20000,4096,copy,17,GPU_RAIN_TILE_ROWS));
    }
    uint32_t seq;assert(gpu_emit_fence(r,&seq,false));
}
int main(){
    occlusion_metrics_test();
    Fixture baseline,measured;GpuWorkload work{};measured.ring.workload=&work;
    encode(baseline);encode(measured);
    assert(baseline.ring.tail==measured.ring.tail&&baseline.ring.seqno==measured.ring.seqno);
    assert(!memcmp(baseline.commands,measured.commands,sizeof(baseline.commands)));
    assert(work.fill_ops==1&&work.fill_bytes==100*20*4);
    assert(work.copies[(unsigned)GpuCopyKind::Other].bytes==80*30*4);
    assert(work.copies[(unsigned)GpuCopyKind::Client].bytes==60*20*4);
    assert(work.copies[(unsigned)GpuCopyKind::Scanout].bytes==1024*600*4);
    assert(work.copies[(unsigned)GpuCopyKind::Rainbow].bytes==1000*600*4);
    // Independently decode every actual BLT to check emitted operations/bytes,
    // rather than assuming one high-level Rainbow rectangle means one copy.
    uint64_t decoded_bytes=0,decoded_ops=0;
    for(unsigned i=0;i<measured.ring.tail/4;){
        uint32_t cmd=measured.commands[i];
        if((cmd&0xffc00000)==GPU_XY_SRC_COPY_BLT_CMD){
            auto* p=measured.commands+i;
            decoded_bytes+=(uint64_t)((p[3]&65535)-(p[2]&65535))*((p[3]>>16)-(p[2]>>16))*4;
            ++decoded_ops;i+=10;
        }else if((cmd&0xffc00000)==GPU_XY_COLOR_BLT_CMD)i+=8;
        else if((cmd&0xff800000)==(GPU_MI_FLUSH_DW&0xff800000))i+=4;
        else {assert(cmd==0);++i;}
    }
    uint64_t total_bytes=0,total_ops=0;
    for(auto& copy:work.copies){total_bytes+=copy.bytes;total_ops+=copy.ops;}
    assert(decoded_bytes==total_bytes&&decoded_ops==total_ops&&work.fences==1);
    GpuRenderRect rects[]={{3,4,23,14},{40,4,50,24}};
    gpu_workload_render(work,rects,2,true);
    gpu_workload_render(work,rects,1,false);
    assert(work.rcs_draws==2&&work.rcs_rects==3&&work.blend_pixels==400&&work.opaque_pixels==200);
    g_gpu_timeline={};g_gpu_execution_slowest={};
    g_gpu_timeline.begin=1;g_gpu_timeline.previous_latch=2;g_gpu_timeline.latch=20;
    g_gpu_timeline.measure=true;g_gpu_timeline.bcs_gpu.samples=1;g_gpu_timeline.workload=work;
    gpu_timeline_finish(21,0,{});
    assert(g_gpu_execution_slowest.interval()==18);
    assert(g_gpu_execution_slowest.workload.blend_pixels==400);
    assert(g_gpu_execution_slowest.workload.copies[(unsigned)GpuCopyKind::Rainbow].bytes==2400000);
    measured.ring.workload=nullptr;encode(measured);
    assert(work.fences==1); // Detached/boot commands cannot contaminate a frame.
    puts("GPU workload: byte-identical commands, decoded BLT payloads, scoped categories, alpha geometry and same-frame timing snapshot passed.");
}
