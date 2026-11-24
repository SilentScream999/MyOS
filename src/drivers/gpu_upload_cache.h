#ifndef WYZAR_GPU_UPLOAD_CACHE_H
#define WYZAR_GPU_UPLOAD_CACHE_H
#include <stdint.h>

// GPU staging contains one complete source. Revision zero denotes mutable
// backing and deliberately never reuses bytes, even at the same address.
struct GpuUploadCache {
    const void* source=nullptr;
    uint64_t bytes=0,revision=0,epoch=0;
    bool matches(const void* next,uint64_t size,uint64_t publication,uint64_t retirement) const {
        return publication&&source==next&&bytes==size&&revision==publication&&epoch==retirement;
    }
    void record(const void* next,uint64_t size,uint64_t publication,uint64_t retirement) {
        source=next;bytes=size;revision=publication;epoch=retirement;
    }
};
#endif
