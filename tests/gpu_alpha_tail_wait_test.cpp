#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "src/drivers/gpu_alpha_tail_wait.h"

static GpuExecutionTiming sample(){
    GpuExecutionTiming value{};value.samples=1;
    value.queue_us=100;value.setup_us=20;value.parse_us=30;
    value.rt_us=6739;value.drain_us=11;value.tail_us=940;value.observed_us=7840;
    return value;
}
static GpuAlphaWaitProfile trained(){
    GpuAlphaWaitProfile profile{};profile.select(123);
    profile.observe(sample());profile.observe(sample());return profile;
}
static void profile_test(){
    auto profile=trained();assert(profile.predicted_us==6900&&profile.samples==2);
    assert(!profile.near_completion(6774,1000000));
    assert(profile.near_completion(6775,1000000));
    assert(profile.near_completion(7150,1000000));
    assert(!profile.near_completion(7151,1000000));
    assert(!profile.near_completion(6900,0));
    assert(!profile.near_completion(UINT64_MAX,1000000));
    assert(!profile.near_completion(6900,10000000001ULL));
    assert(profile.near_completion(6900ULL*3000,3000000000ULL));
    assert(!profile.near_completion(6774ULL*3000,3000000000ULL));
    profile.select(124);assert(!profile.samples&&!profile.predicted_us);
    profile.observe(sample());assert(!profile.near_completion(6900,1000000));
    profile.observe(sample());assert(profile.near_completion(6900,1000000));
    auto invalid=sample();invalid.invalid=1;profile.observe(invalid);
    assert(!profile.samples&&!profile.predicted_us);
    for(unsigned kind=0;kind<5;++kind){
        profile=trained();invalid=sample();
        if(kind==0)invalid.samples=0;
        if(kind==1)invalid.samples=2;
        if(kind==2)invalid.observed_us=100;
        if(kind==3)invalid.rt_us=100001;
        if(kind==4)invalid.queue_us=invalid.setup_us=invalid.parse_us=invalid.rt_us=invalid.drain_us=0;
        profile.observe(invalid);assert(!profile.samples&&!profile.predicted_us);
    }
}
static void budget_test(){
    GpuAlphaPollBudget budget{};
    assert(!budget.poll(1,1000,1000000,false));
    assert(budget.poll(1,1775,1000000,true));
    budget.finish(2000);assert(budget.used==225&&budget.total_ticks==225&&!budget.active);
    // A second submission in the same frame gets only the remainder.
    assert(budget.poll(1,2000,1000000,true));budget.finish(2025);
    assert(budget.used==250&&!budget.poll(1,2025,1000000,true));
    assert(!budget.poll(1,100000,1000000,true));
    assert(budget.total_ticks==250); // sleeping intervals were not charged
    assert(budget.poll(2,100000,1000000,true));budget.finish(100010);
    assert(budget.used==10&&budget.total_ticks==260);
    assert(!budget.poll(2,100010,2000000,true)); // never renew a frame's allowance by changing its clock
    assert(!budget.poll(2,100010,1000000,true));
    assert(!budget.poll(3,100010,0,true));
    assert(!budget.poll(3,100010,1000000,true));
    assert(!budget.poll(0,100010,1000000,true));
    assert(!budget.poll(4,100010,10000000001ULL,true));
    assert(budget.poll(5,100010,1000000,true));budget.finish(100000);
    assert(!budget.poll(5,100011,1000000,true)); // clock regression spends the allowance
    assert(budget.poll(6,100010,1000000,true));budget.finish(110010);
    assert(budget.used==250&&!budget.poll(6,110010,1000000,true));
    GpuAlphaPollBudget scaled{};
    assert(scaled.poll(7,1,3000000000ULL,true));
    scaled.finish(750001);assert(scaled.used==750000);
    assert(!scaled.poll(7,750001,3000000000ULL,true));
}
struct Result {uint64_t observed,poll_ticks,sleeps;bool completed;};
static Result wait_simulation(bool adaptive,uint64_t fence_at,uint64_t retired_at,
                              GpuAlphaWaitProfile profile=trained()){
    GpuAlphaPollBudget budget{};uint64_t now=50,sleeps=0;
    while(now<200000){
        // The prediction is never part of the completion predicate.
        if(now>=fence_at&&now>=retired_at){budget.finish(now);return {now,budget.used,sleeps,true};}
        if(adaptive&&budget.poll(1,now,1000000,profile.near_completion(now,1000000))){now+=10;continue;}
        // A 1-kHz timer whose phase matches the captured RCS wait.
        now=((now+160)/1000+1)*1000-160;++sleeps;
    }
    budget.finish(now);return {now,budget.used,sleeps,false};
}
int main(){
    profile_test();budget_test();
    auto old=wait_simulation(false,6930,6970),tail=wait_simulation(true,6930,6970);
    assert(old.completed&&tail.completed&&old.observed==7840&&tail.observed==6970);
    assert(tail.poll_ticks==130&&tail.sleeps==old.sleeps-1);
    auto slower=wait_simulation(true,9030,9070);
    assert(slower.completed&&slower.observed>=9070&&slower.poll_ticks==250);
    for(bool missing_fence:{false,true}){
        auto hung=wait_simulation(true,missing_fence?UINT64_MAX:6930,missing_fence?6970:UINT64_MAX);
        assert(!hung.completed&&hung.poll_ticks==250&&hung.sleeps>190);
    }
    GpuAlphaWaitProfile cold{};cold.select(1);cold.observe(sample());
    auto untrained=wait_simulation(true,6930,6970,cold);
    assert(untrained.completed&&untrained.observed==old.observed&&!untrained.poll_ticks);
    puts("Alpha wait: validated warmup, workload invalidation, shared 250us allowance, unchanged fence/retirement timeout, and 870us earlier completion observation passed.");
}
