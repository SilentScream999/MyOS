#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

static uint32_t config[2][64];
static std::vector<std::string> lines;
static void print(char* s) { lines.emplace_back(s); }
static void gpu_log_hex(const char*,uint64_t) {}
static uint32_t pci_cfg_read32(uint64_t,uint8_t,uint8_t bus,uint8_t,uint8_t,unsigned off) {
    assert(bus < 2 && !(off & 3) && off < 256);
    return config[bus][off/4];
}
static uint16_t pci_cfg_read16(uint64_t base,uint8_t start,uint8_t bus,uint8_t dev,uint8_t fn,unsigned off) {
    return pci_cfg_read32(base,start,bus,dev,fn,off);
}
#include "../src/drivers/gpu_pci_power_trace.h"

int main() {
    trace_nvidia_power("boot");
    assert(lines.back()=="PCI function not identified during boot scan");
    config[0][0]=0x12348086;
    config[0][6]=0x010100;
    trace_pci_discover({1,config[0][0],0,0,0,1,0},0x06040000);
    config[1][0]=0x134d10de;
    trace_pci_discover({1,config[1][0],0,0,1,0,0},0x03020000);
    assert(trace_upstream.id==config[0][0]);
    config[1][1]=1u<<20;
    config[1][13]=0x40;
    config[1][16]=0x5010;
    config[1][20]=1;
    trace_pci_power(trace_nvidia,"boot");
    assert(lines.back()=="PCI state D0");
    config[1][21]=3;
    trace_pci_power(trace_nvidia,"timeout");
    assert(lines.back()=="PCI state D3hot");
    config[1][21]=0xffff;
    trace_pci_power(trace_nvidia,"timeout");
    assert(lines.back().find("unknown")!=std::string::npos);
    config[1][20]=0x4010; // Cyclic list.
    trace_pci_power(trace_nvidia,"timeout");
    assert(lines.back().find("unknown")!=std::string::npos);
    config[1][0]=UINT32_MAX;
    trace_pci_power(trace_nvidia,"timeout");
    assert(lines.back()=="PCI identity unavailable or changed; power state unknown");
}
