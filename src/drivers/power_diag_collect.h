#ifndef WYZAR_POWER_DIAG_COLLECT_H
#define WYZAR_POWER_DIAG_COLLECT_H
static void collect_power_sample(power_diag::Sample& s){
    using namespace power_diag;
    // All scheduler tasks currently execute on the BSP. Snapshot there with
    // short IRQ exclusion; never sleep, render or print inside this section.
    TtyIrqGuard guard;
    s.version=1;s.bytes=sizeof(s);
    uint32_t a,b,c,d;timer_cpuid(0,a,b,c,d);uint32_t maximum=a;
    memcpy(s.vendor,&b,4);memcpy(s.vendor+4,&d,4);memcpy(s.vendor+8,&c,4);
    bool intel=!strncmp(s.vendor,"GenuineIntel",12);
    if(maximum>=1){timer_cpuid(1,a,b,c,d);s.signature=a;s.hypervisor=c>>31;}
    if(maximum>=6){timer_cpuid(6,a,b,c,d);s.features6a=a;s.features6c=c;}
    if(maximum>=0x16){timer_cpuid(0x16,a,b,c,d);s.nominal_mhz=a&65535;}
    bool model=client_model(s.signature);
    for(unsigned i=0;i<Count;++i){
        if(!allowed(i,intel,model,s.features6a,s.features6c))continue;
        // Requests/capabilities are defined only while HWP is enabled.
        if(i>=HwpCapabilities&&(!(s.valid&(1u<<HwpEnable))||!(s.msr[HwpEnable]&1)))continue;
        if(power_read_msr(msrs[i],&s.msr[i]))s.valid|=1u<<i;
    }
    if(!s.nominal_mhz&&intel&&model&&(s.valid&(1u<<Platform)))s.nominal_mhz=((s.msr[Platform]>>8)&255)*100;
    // Read Gen9 status only on our supported client family, while the existing
    // driver already holds render forcewake. Diagnostics never acquire it.
    if(g_gpu_ready&&g_render_ready&&g_igpu.mmio_virt&&g_igpu.mmio_size>0xa020&&
       ((g_igpu.devid&0xff00)==0x1900||(g_igpu.devid&0xff00)==0x5900)&&
       (*(volatile uint32_t*)(g_igpu.mmio_virt+0xd84)&1)){
        s.gpu_valid=1;s.gpu_device=g_igpu.devid;
        s.rpstat=*(volatile uint32_t*)(g_igpu.mmio_virt+0xa01c);
        s.rp_request=*(volatile uint32_t*)(g_igpu.mmio_virt+0xa008);
    }
    uint32_t lo,hi;__asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
    s.ticks=g_tick_count;s.tsc=((uint64_t)hi<<32)|lo;s.tsc_hz=tsc_hz;
    s.scene_count=g_scene_compose_count;s.scene_pixels=g_scene_compose_pixels;s.gpu_flips=g_gpu_flips;
    s.bcs_batches=g_gpu_perf.bcs_batches;s.rcs_draws=g_gpu_perf.rcs_draws;s.repair_pixels=g_gpu_perf.repair_pixels;
}
#endif
