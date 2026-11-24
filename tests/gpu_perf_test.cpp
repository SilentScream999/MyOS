#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <array>
#include "src/drivers/gpu_perf.h"
#include "src/drivers/gpu_frame_gap.h"
#include "src/gui/gpu_presentation.h"

static void retained_scanout_test(){
    constexpr unsigned width=96,height=90;
    std::array<unsigned,width*height> scene{},buffers[2]{};
    DamageList pending[2]{};
    unsigned old_cursor[2]={0,0};
    for(unsigned frame=1;frame<=80;++frame){
        unsigned back=frame%2;
        DamageList changed{};
        // Include independent animated/app damage, shell rows and enough
        // fragments to exercise conservative merging at the list capacity.
        for(unsigned update=0;update<12;++update){
            unsigned x=(frame*7+update*13)%88,y=(frame*11+update*7)%82;
            for(unsigned row=y;row<y+8;++row)for(unsigned col=x;col<x+8;++col)
                scene[row*width+col]=frame*100+update;
            damage_add(&changed,x,x+7,y,y+7);
        }
        for(auto& buffer:pending)gpu_frame_damage(buffer,changed,width,height,10,false);
        DamageList overlay{};
        unsigned cursor=old_cursor[back];
        damage_add(&overlay,cursor%width,cursor%width,cursor/width,cursor/width);
        gpu_frame_damage(pending[back],overlay,width,height,10,false);
        DamageList repair{};
        gpu_frame_damage(repair,pending[back],width,height,10,false);
        for(unsigned i=0;i<repair.n;++i){
            auto r=repair.r[i];
            for(unsigned y=r.y1;y<=r.y2;++y)for(unsigned x=r.x1;x<=r.x2;++x)
                buffers[back][y*width+x]=scene[y*width+x];
        }
        assert(buffers[back]==scene);
        damage_clear(&pending[back]);
        old_cursor[back]=(frame*137)%(width*height);
        buffers[back][old_cursor[back]]=0xffffffff; // presentation overlay
    }
}

static void exact_scanout_test(){
    constexpr unsigned W=160,H=120,T=12;
    DamageList crossing{};damage_add(&crossing,50,79,20,80);
    VisibleRegion exact{};assert(gpu_exact_frame_damage(exact,crossing,W,H,T,false));
    uint64_t area=0;for(unsigned i=0;i<exact.count;++i){auto r=exact.rects[i];area+=(r.x2-r.x1)*(r.y2-r.y1);}
    assert(area==W*40+30*21);
    VisibleRegion hidden_stats{};
    assert(gpu_exact_frame_damage(hidden_stats,crossing,W,H,T,false,false));
    assert(hidden_stats.count==1);
    const auto bounded=hidden_stats.rects[0];
    assert(bounded.x1==50&&bounded.x2==80&&bounded.y1==20&&bounded.y2==81);
    assert(gpu_exact_frame_rect(hidden_stats,{50,110,80,112},W,H,T,false,false));
    assert(hidden_stats.count==2);
    const auto taskbar=hidden_stats.rects[1];
    assert(taskbar.x1==0&&taskbar.x2==W&&taskbar.y1==110&&taskbar.y2==112);
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){unsigned hits=0;
        for(unsigned i=0;i<exact.count;++i){auto r=exact.rects[i];hits+=x>=(unsigned)r.x1&&x<(unsigned)r.x2&&y>=(unsigned)r.y1&&y<(unsigned)r.y2;}
        assert(hits==((y>=20&&y<60)||(y>=60&&y<=80&&x>=50&&x<=79)));
    }
    std::array<unsigned,W*H> scene{},buffers[2]{};
    VisibleRegion pending[2]{};unsigned cursor[2]={0,0};bool drawn[2]{};
    for(unsigned frame=1;frame<=80;++frame){
        DamageList changed{};
        for(unsigned i=0;i<8;++i){unsigned x=2+((frame*13+i*17)%150),y=64+((frame*3+i*7)%40);
            scene[y*W+x]=frame*100+i;damage_add(&changed,x,x,y,y);}
        for(auto& p:pending)assert(gpu_exact_frame_damage(p,changed,W,H,T,false));
        unsigned back=frame%2;
        if(drawn[back])assert(gpu_exact_frame_rect(pending[back],{(int)(cursor[back]%W),(int)(cursor[back]/W),
            (int)(cursor[back]%W)+1,(int)(cursor[back]/W)+1},W,H,T,false));
        for(unsigned i=0;i<pending[back].count;++i){auto r=pending[back].rects[i];
            for(int y=r.y1;y<r.y2;++y)for(int x=r.x1;x<r.x2;++x)buffers[back][y*W+x]=scene[y*W+x];}
        assert(buffers[back]==scene);pending[back].count=0;
        cursor[back]=(frame*137)%(W*H);buffers[back][cursor[back]]=0xffffffff;drawn[back]=true;
    }
    exact.count=0;
    for(unsigned i=0;i<64;++i)assert(visibility_add_exact(&exact,{(int)i*2,70,(int)i*2+1,71}));
    assert(exact.count==64);
    assert(!gpu_exact_frame_rect(exact,{140,80,141,81},W,H,T,false));
    assert(exact.count==1&&exact.rects[0].x1==0&&exact.rects[0].x2==W&&exact.rects[0].y2==H);
}
static uint64_t fake_wall=0,fake_cpu=0;
static unsigned cpu_reads=0,wall_reads=0;
static uint64_t test_wall(){++wall_reads;return fake_wall;}
static uint64_t test_cpu(){++cpu_reads;return fake_cpu;}
static void frame_diagnostics_test(){
    GpuFrameTimeline::Cost cost{};
    fake_wall=100;fake_cpu=10;
    {GpuCpuPhaseTimer timer(&cost,test_cpu,test_wall);fake_wall=400;fake_cpu=50;}
    assert(cost.wall==300&&cost.cpu==40&&!cost.invalid);
    {GpuCpuPhaseTimer timer(&cost,test_cpu,test_wall);fake_wall=1400;}
    assert(cost.wall==1300&&cost.cpu==40); // Off-core time is wall, not CPU.
    {GpuCpuPhaseTimer timer(&cost,test_cpu,test_wall);fake_cpu=1;}
    assert(cost.wall==1300&&cost.cpu==40&&cost.invalid==1);
    cpu_reads=wall_reads=0;
    {GpuCpuPhaseTimer timer(nullptr,test_cpu,test_wall);}
    assert(!cpu_reads&&!wall_reads);
    unsigned reads=0;uint32_t ending_frame=17,line=0x80000437u;
    uint64_t stamp=100;
    auto read=[&](uint32_t reg){++reads;
        if(reg==0x70040)return reads==1?17u:ending_frame;
        if(reg==0x70000)return line;
        assert(reg==0x701ac);return 0x1000020u;
    };
    auto clock=[&](){return stamp++;};
    auto p=gpu_read_display_phase(read,clock,0x70040,0x70000,0x701ac);
    assert(reads==4&&p.valid&&p.frame==17&&p.line()==1079&&p.live==0x1000020);
    assert(p.first==100&&p.last==101);
    reads=0;ending_frame=18;
    p=gpu_read_display_phase(read,clock,0x70040,0x70000,0x701ac);
    assert(!p.valid&&reads==4); // A refresh crossing the observation is flagged.
    reads=0;ending_frame=17;line=UINT32_MAX;
    assert(!gpu_read_display_phase(read,clock,0x70040,0x70000,0x701ac).valid);
    g_gpu_timeline={};g_gpu_timeline_slowest={};g_gpu_execution_slowest={};
    g_gpu_timeline.begin=10;g_gpu_timeline.previous_latch=1;g_gpu_timeline.latch=20;
    g_gpu_timeline.measure=true;g_gpu_timeline.bcs_gpu.samples=1;
    g_gpu_timeline.planning[GpuFrameTimeline::Merge].cpu=77;
    g_gpu_timeline.display_after.frame=19;
    gpu_timeline_finish(21,0,{});
    assert(g_gpu_timeline_slowest.planning[GpuFrameTimeline::Merge].cpu==77);
    assert(g_gpu_execution_slowest.display_after.frame==19);
    g_gpu_timeline={};g_gpu_timeline_slowest={};g_gpu_execution_slowest={};
}
static void frame_gap_test(){
    GpuGapHistory history;
    auto load=history.begin(100,GpuGapActivity::Launch,7,"viewer");
    auto& event=history.events[GpuGapHistory::slot(load)];
    assert(event.overlaps(120,140)&&!event.overlaps(60,99));
    history.finish(load,130);
    assert(event.overlaps(120,140)&&!event.overlaps(131,150));
    for(unsigned i=0;i<GpuGapHistory::capacity;++i)
        history.begin(150+i,GpuGapActivity::StartOpen,1,nullptr);
    assert(event.serial!=load&&event.detail[0]==0);
    history.finish(load,500);
    assert(event.end==0);
    assert(!gpu_frame_gap_due(17000,1,1000000));
    assert(gpu_frame_gap_due(32999,2,1000000));
    assert(gpu_frame_gap_due(34000,0,1000000));
    assert(!gpu_frame_gap_due(17000,0xffffffff,1000000));
    assert(!gpu_frame_gap_due(0,3,1000000));
}

int main() {
    frame_gap_test();
    frame_diagnostics_test();
    exact_scanout_test();
    retained_scanout_test();
    assert(gpu_timestamp_frequency(100,38500,6000000,3000000000ULL)==19200000);
    assert(gpu_timestamp_frequency(0xfffffff0u,(uint32_t)(0xfffffff0u+38400u),6000000,3000000000ULL)==19200000);
    assert(!gpu_timestamp_frequency(100,100,6000000,3000000000ULL));
    assert(!gpu_timestamp_frequency(1,2,0,3000000000ULL));
    assert(!gpu_timestamp_frequency(0,UINT32_MAX,1,10000000000ULL));
    assert(gpu_timestamp_clock_agreement(19200000,20000000)==19600000);
    assert(!gpu_timestamp_clock_agreement(19200000,22000000));
    GpuExecutionTiming execution{};
    uint32_t stamps[]={110,120,180,210,230};
    assert(gpu_execution_record(execution,1000000,100,stamps,250,150,1000000));
    assert(execution.samples==1&&execution.queue_us==10&&execution.setup_us==10&&execution.parse_us==60);
    assert(execution.rt_us==30&&execution.drain_us==20&&execution.tail_us==20&&execution.observed_us==150);
    uint32_t wrapped[]={0xfffffff5u,0xfffffffau,10,15,20};
    assert(gpu_execution_record(execution,1000000,0xfffffff0u,wrapped,40,56,1000000));
    auto before=execution;
    stamps[3]=90; // stale/out-of-order memory must never become a huge GPU time.
    assert(!gpu_execution_record(execution,1000000,100,stamps,250,150,1000000));
    assert(execution.invalid==1&&execution.samples==before.samples&&execution.queue_us==before.queue_us);
    stamps[3]=210;
    assert(!gpu_execution_record(execution,1000000,100,stamps,250,100000,1000000));
    assert(!gpu_execution_record(execution,0,100,stamps,250,150,1000000));
    // Two submissions, CPU work and sleeping belong to the same completed
    // frame. Keep the first submission and last observed completion, then
    // retain the frame with the largest presentation interval in the sample.
    GpuPerfCounters baseline{};
    baseline.bcs_wait=10;baseline.rcs_wait=20;baseline.bcs_batches=3;
    baseline.bcs_sleep.ticks=7;
    g_gpu_timeline={};g_gpu_timeline_slowest={};
    g_gpu_timeline.begin=110;g_gpu_timeline.previous_latch=100;
    g_gpu_timeline.cpu=50;g_gpu_timeline.bcs=10;g_gpu_timeline.rcs=20;
    g_gpu_timeline.sleep=7;g_gpu_timeline.bcs_batches=3;
    gpu_timeline_submit(120);gpu_timeline_ready(130);
    gpu_timeline_submit(140);gpu_timeline_ready(160);
    baseline.bcs_wait+=15;baseline.rcs_wait+=25;baseline.bcs_batches+=2;
    baseline.rcs_draws=1;baseline.flip_sleep.ticks=40;
    g_gpu_timeline.latch=180;
    gpu_timeline_finish(182,60,baseline);
    auto timeline=g_gpu_timeline_slowest;
    assert(timeline.first_submit==120&&timeline.last_ready==160);
    assert(timeline.interval()==80&&timeline.finish==182&&timeline.cpu==10);
    assert(timeline.bcs==15&&timeline.rcs==25&&timeline.sleep==40);
    assert(timeline.bcs_batches==2&&timeline.rcs_draws==1);
    assert(g_gpu_timeline_last_latch==180&&!g_gpu_timeline.begin);
    gpu_timeline_submit(200);gpu_timeline_ready(210);
    assert(g_gpu_timeline.last_ready==160); // Boot/outside-frame calls excluded.
    g_gpu_timeline={};g_gpu_timeline.begin=190;
    gpu_timeline_finish(200,0,{});
    assert(!g_gpu_timeline_last_latch&&g_gpu_timeline_slowest.interval()==80);
    g_gpu_timeline={};g_gpu_timeline.begin=210;
    g_gpu_timeline.previous_latch=200;g_gpu_timeline.latch=230;
    gpu_timeline_finish(231,0,{});
    assert(g_gpu_timeline_slowest.interval()==80);
    assert(gpu_rainbow_phase(0,60000)==0);
    assert(gpu_rainbow_phase(30000,60000)==120);
    assert(gpu_rainbow_phase(60000,60000)==240);
    assert(gpu_rainbow_phase(180000,60000)==0);
    for(unsigned frame=0;frame<90;++frame)
        assert(gpu_rainbow_phase(frame*2000,60000)==frame*8%720);
    assert(gpu_rainbow_phase(1000,0)==0);
    g_rain_trace.update=1;
    {
        RainbowWindowTimer timer(2,17,"Scaled window",true);
        timer.next(&RainbowWindowTrace::chrome);
        timer.next(&RainbowWindowTrace::gpu);
        g_gpu_perf.bcs_wait+=10;g_gpu_perf.rcs_wait+=20;
        timer.next(&RainbowWindowTrace::client);
    }
    const auto& window=g_rain_trace.windows[2];
    assert(window.id==17 && window.alpha && window.passes==1);
    assert(window.shadow+window.chrome+window.gpu+window.client==window.end-window.start);
    assert(window.bcs==10 && window.rcs==20);
    assert(window.title[0]=='S' && window.title[13]==0);
    g_rain_trace={};
    assert(gpu_scanout_use_streaming_copy(true,4,8));
    assert(!gpu_scanout_use_streaming_copy(true,5,9));
    assert(!gpu_scanout_use_streaming_copy(false,1,10));
    assert(!gpu_scanout_use_streaming_copy(true,0,10));
    GpuPerfSleep sleep{};
    constexpr uint64_t hz=1000000;
    for(auto ticks:{0ULL,999ULL,1000ULL,1999ULL,2000ULL,9999ULL,10000ULL,15000ULL})
        gpu_perf_record_sleep(sleep,ticks,hz);
    assert(sleep.calls==8&&sleep.ticks==40997);
    assert(sleep.at_least_2ms==4&&sleep.at_least_10ms==2);
    auto old=sleep;
    gpu_perf_record_sleep(sleep,1000,hz);
    assert(sleep.calls-old.calls==1&&sleep.ticks-old.ticks==1000);
    assert(sleep.at_least_2ms==old.at_least_2ms&&sleep.at_least_10ms==old.at_least_10ms);
    GpuPerfSleep uncalibrated{};
    gpu_perf_record_sleep(uncalibrated,15000,0);
    assert(uncalibrated.calls==1&&uncalibrated.ticks==15000);
    assert(!uncalibrated.at_least_2ms&&!uncalibrated.at_least_10ms);
    unsigned calls=0;
    gpu_perf_sleep_one(uncalibrated,0,[&](uint64_t ms){assert(ms==1);++calls;});
    assert(calls==1&&uncalibrated.calls==2);
    GpuFlipCadence cadence{};
    cadence.record(100,10,0xfffffffeu);
    cadence.record(133,11,0);
    cadence.record(184,12,3);
    cadence.record(1000,30,50); // idle period
    assert(cadence.intervals==2&&cadence.total_ticks==84&&cadence.max_ticks==51);
    assert(cadence.intervening_refreshes==3&&cadence.over_two_refreshes==1&&!cadence.last_interval);
    cadence.reset_sample();
    cadence.record(1033,31,52);
    assert(cadence.intervals==1&&cadence.max_ticks==33&&cadence.over_two_refreshes==0);
    puts("GPU sleep diagnostics: threshold boundaries, sample deltas, uncalibrated clock and unchanged wait request passed.");
}
