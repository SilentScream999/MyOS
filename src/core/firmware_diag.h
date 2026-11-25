#ifndef WYZAR_FIRMWARE_DIAG_H
#define WYZAR_FIRMWARE_DIAG_H
// Read-only target-machine evidence for firmware hotkey bring-up. Does not
// interpret AML or enter ACPI mode. Export only FADT/ECDT/DSDT/SSDT, not MSDM.
namespace firmware_diag {
static pid_t owner=0;
static bool starting=false;
static bool writable(uint64_t address,uint64_t length){
    if(!address||!length||length>65536||address>=USERSPACE_END||length>USERSPACE_END-address)return false;
    for(uint64_t page=address&~4095ULL;page<=((address+length-1)&~4095ULL);page+=4096){
        uint64_t table=read_cr3()&PTE_ADDR_MASK;
        for(int shift=39;shift>=12;shift-=9){
            uint64_t entry=((uint64_t*)(HHDM+table))[(page>>shift)&511];
            if((entry&(PRESENT|USER|WRITABLE))!=(PRESENT|USER|WRITABLE))return false;
            if(shift==12||((shift==30||shift==21)&&(entry&HUGE)))break;
            table=entry&PTE_ADDR_MASK;
        }
    }
    return true;
}
static bool acpi_backing(uint64_t physical,uint64_t length){
    if(!memmap_req.response||!physical||!length||physical+length<physical)return false;
    for(uint64_t i=0;i<memmap_req.response->entry_count;++i){
        auto* e=memmap_req.response->entries[i];
        if((e->type==LIMINE_MEMMAP_ACPI_RECLAIMABLE||e->type==LIMINE_MEMMAP_ACPI_NVS)&&physical>=e->base&&
           physical-e->base<=e->length&&length<=e->length-(physical-e->base))return true;
    }
    return false;
}
static bool map_acpi(uint64_t physical,uint64_t length){
    if(!acpi_backing(physical,length))return false;
    TtyIrqGuard guard;
    for(uint64_t p=physical&~4095ULL;p<physical+length;p+=4096)map_page(HHDM+p,p,WRITABLE|NX);
    return true;
}
static const ACPISDTHeader* table(unsigned index){
    static const ACPISDTHeader* cached[35]{};static bool tried[35]{};
    if(index>=35)return nullptr;
    if(tried[index])return cached[index];
    tried[index]=true;
    auto* header=index==0?find_acpi_table("FACP"):index==1?find_acpi_table("ECDT"):
        index>=3?find_acpi_table("SSDT",index-3):nullptr;
    if(index==2){
        auto* f=table(0);if(!f||f->length<44)return nullptr;
        uint64_t phys=thinkpad_audio::integer_at((const uint8_t*)f+40,4);
        if(f->length>=148){uint64_t extended=thinkpad_audio::integer_at((const uint8_t*)f+140,8);if(extended)phys=extended;}
        if(!map_acpi(phys,sizeof(ACPISDTHeader)))return nullptr;
        header=(ACPISDTHeader*)(HHDM+phys);
    }
    if(!header||(uint64_t)header<HHDM)return nullptr;
    uint64_t physical=(uint64_t)header-HHDM;
    if(!map_acpi(physical,sizeof(ACPISDTHeader))||header->length<36||header->length>1024*1024)return nullptr;
    const char* signature=index==0?"FACP":index==1?"ECDT":index==2?"DSDT":"SSDT";
    if(strncmp(header->signature,signature,4)||!map_acpi(physical,header->length))return nullptr;
    uint8_t checksum=0;for(unsigned i=0;i<header->length;++i)checksum+=((const uint8_t*)header)[i];
    if(checksum)return nullptr;
    return cached[index]=header;
}
static int64_t control(uint64_t action,uint64_t arg,uint64_t buffer,uint64_t capacity,uint64_t offset){
    if(!g_current_task)return -1;
    if(action==0){
        if(arg<1000||arg>45000)return -4;
        {TtyIrqGuard guard;if(starting||g_fn_trace.active(g_tick_count))return -2;starting=true;}
        hda::control(0,0);
        owner=g_current_task->pid;g_fn_trace_owner=owner;
        fn_probe::begin(hda::ready&&hda::firmware_identity(),arg);
        {TtyIrqGuard guard;starting=false;}return 0;
    }
    if(action==2){
        auto* data=arg<35?table(arg):nullptr;if(!data)return -1;
        if(!buffer&&!capacity)return data->length;
        if(offset>data->length||!writable(buffer,capacity))return -4;
        unsigned count=data->length-offset;if(count>capacity)count=capacity;
        memcpy((void*)buffer,(const uint8_t*)data+offset,count);return count;
    }
    TtyIrqGuard owner_guard;
    if(owner!=g_current_task->pid)return -2;
    if(action==1){
        TtyIrqGuard guard;unsigned size=g_fn_trace.bytes();
        if(!buffer&&!capacity)return size;
        if(arg>size||!writable(buffer,capacity))return -4;
        unsigned count=size-arg;if(count>capacity)count=capacity;
        memcpy((void*)buffer,(uint8_t*)&g_fn_trace+arg,count);return count;
    }
    if(action==3){if(arg>15)return -4;TtyIrqGuard guard;g_fn_trace.stage=arg;g_fn_trace.append(g_tick_count,4,arg);return 0;}
    if(action==4){TtyIrqGuard guard;g_fn_trace.deadline=0;return 0;}
    return -4;
}
}
#endif
