#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#define helpers_h
#define PMM_HOST_TEST
static void print(char*) {}
static void to_hex(uint64_t,char*) {}
static void to_str(uint64_t,char*) {}
static uint64_t read_cr3() { return 0; }
[[noreturn]] static void hcf() { std::abort(); }
// Model disabling preemption with a recursive lock: nested count==1 calls
// and callers that already hold the guard must preserve exclusion.
static std::recursive_mutex allocator_lock;
struct PmmGuard {
    PmmGuard(){allocator_lock.lock();}
    ~PmmGuard(){allocator_lock.unlock();}
};
#include "src/arch/pagingstuff.h"
alignas(4096) static uint8_t memory[64*1024*1024];
static std::atomic<bool> owned[sizeof(memory)/4096];

int main() {
    HHDM=(uintptr_t)memory;
    pmm_regions[0]={PAGE_SIZE,sizeof(memory)/2};
    pmm_regions[1]={sizeof(memory)/2+PAGE_SIZE,sizeof(memory)};
    pmm_region_count=2;
    // Preserve a hole when advancing between regions.
    memset(memory+sizeof(memory)/2,0xa5,PAGE_SIZE);
    std::vector<std::thread> workers;
    for(unsigned worker=0;worker<8;++worker)workers.emplace_back([worker]{
        for(unsigned step=0;step<1024;++step){
            unsigned count=(step+worker)%3+1;
            uint64_t physical=alloc_phys_pages(count);
            for(unsigned p=0;p<count;++p){
                unsigned index=physical/PAGE_SIZE+p;
                assert(!owned[index].exchange(true)&&"page allocated to two callers");
                auto* bytes=memory+index*PAGE_SIZE;
                for(unsigned i=0;i<PAGE_SIZE;++i)assert(bytes[i]==0);
                memset(bytes,0x37,PAGE_SIZE);
            }
            std::this_thread::yield();
            for(unsigned p=0;p<count;++p){
                unsigned index=physical/PAGE_SIZE+p;
                for(unsigned i=0;i<PAGE_SIZE;++i)assert(memory[index*PAGE_SIZE+i]==0x37);
                assert(owned[index].exchange(false));
                free_phys_page(index*PAGE_SIZE);
            }
        }
    });
    for(auto& worker:workers)worker.join();
    for(unsigned i=0;i<PAGE_SIZE;++i)assert(memory[sizeof(memory)/2+i]==0xa5);
    for(auto& page:owned)assert(!page);
    // One-page API must also zero recycled pages and advance a full region.
    uint64_t one=alloc_phys_page();
    for(unsigned i=0;i<PAGE_SIZE;++i)assert(memory[one+i]==0);
    free_phys_page(one);
    puts("Physical page allocation: concurrent unique ownership, recycling, zeroing and region gaps passed.");
}
