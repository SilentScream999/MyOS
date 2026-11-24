#ifndef WYZAR_WINDOW_ALPHA_H
#define WYZAR_WINDOW_ALPHA_H
#include <stdint.h>

// Opt-in client pixel contract. Legacy surfaces ignore their alpha byte.
enum : uint32_t { WIN_ALPHA_OPAQUE=0, WIN_ALPHA_PREMULTIPLIED=1 };

// Premultiplied source-over onto the opaque desktop. Round to nearest just as
// the UNORM render target does. Input RGB components must be at most alpha.
static inline uint32_t window_source_over(uint32_t source,uint32_t destination) {
    unsigned alpha=source>>24;
    if(!alpha)return destination;
    if(alpha==255)return source;
    uint32_t result=0xff000000;
    for(unsigned shift=0;shift<24;shift+=8){
        unsigned value=((source>>shift)&255)+(((destination>>shift)&255)*(255-alpha)+127)/255;
        result|=(value>255?255:value)<<shift;
    }
    return result;
}
#endif
