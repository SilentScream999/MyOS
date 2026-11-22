#pragma once

// Called from the existing ECAM scan in diagnostic builds. Remember the first
// NVIDIA display function (including class 03:02) and its immediate bridge.
struct GpuTracePciFunction {
    uint64_t base;
    uint32_t id;
    uint16_t segment;
    uint8_t start, bus, dev, fn;

    uint32_t read(unsigned offset) const {
        return pci_cfg_read32(base,start,bus,dev,fn,offset);
    }
};
static GpuTracePciFunction trace_nvidia, trace_upstream;
static GpuTracePciFunction trace_bus_parent[256];

static void trace_pci_discover(GpuTracePciFunction f,uint32_t class_reg) {
    if ((class_reg >> 16) == 0x0604) {
        uint32_t buses = f.read(0x18);
        uint8_t secondary = buses >> 8;
        if ((uint8_t)buses == f.bus && secondary > f.bus && secondary <= (uint8_t)(buses >> 16))
            trace_bus_parent[secondary] = f;
    }
    if (!trace_nvidia.base && (uint16_t)f.id == 0x10de && (class_reg >> 24) == 3) {
        trace_nvidia = f;
        trace_upstream = trace_bus_parent[f.bus];
    }
}

static void trace_pci_power(const GpuTracePciFunction& f,const char* label) {
    print((char*)label);
    if (!f.base) {
        print((char*)"PCI function not identified during boot scan");
        return;
    }
    gpu_log_hex("PCI segment:bus:device:function (packed bytes below segment):",
                ((uint64_t)f.segment<<24)|((uint32_t)f.bus<<16)|(f.dev<<8)|f.fn);
    gpu_log_hex("PCI device:vendor ID at discovery:",f.id);
    if (f.read(0) != f.id) {
        print((char*)"PCI identity unavailable or changed; power state unknown");
        return;
    }
    uint32_t status_command = f.read(4);
    if (status_command != UINT32_MAX && (status_command & (1u<<20))) {
        unsigned offset = f.read(0x34) & 0xfc;
        uint64_t seen = 0;
        while (offset >= 0x40) {
            uint64_t bit = 1ULL << (offset/4);
            if (seen & bit) break;
            seen |= bit;
            uint32_t cap = f.read(offset);
            if (cap == UINT32_MAX) break;
            if ((cap & 255) == 1) {
                if (offset > 0xf8) break;
                uint16_t pmcsr = pci_cfg_read16(f.base,f.start,f.bus,f.dev,f.fn,offset+4);
                if (pmcsr == 0xffff) break;
                gpu_log_hex("PCI PMCSR:",pmcsr);
                static const char* const states[]={"PCI state D0","PCI state D1","PCI state D2","PCI state D3hot"};
                print((char*)states[pmcsr & 3]);
                return;
            }
            offset = (cap >> 8) & 0xfc;
        }
    }
    print((char*)"PCI PM capability unavailable; power state unknown");
}

static void trace_nvidia_power(const char* phase) {
    print((char*)phase);
    trace_pci_power(trace_nvidia,"GPU PCI trace: NVIDIA display");
    if (trace_nvidia.base) trace_pci_power(trace_upstream,"GPU PCI trace: upstream bridge");
}
