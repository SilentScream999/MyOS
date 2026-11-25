#ifndef WYZAR_THINKPAD_SMI_H
#define WYZAR_THINKPAD_SMI_H
#include <stdint.h>
// Exact captured UCMS(0x1c): SMI(2,0x1c,0,0,0). Caller validates/maps the
// 21-byte MNVS+0xfc0 mailbox and serializes access. Never hold the EC/FACS
// lock across SMI: this is a separate firmware interface, not an EC command.
// Return 0=completed, -1=bounded timeout, -2=already busy, -3=firmware error.
template<class Read,class Write,class Trigger,class Pause>
static int thinkpad_linux_hotkey_init(Read read,Write write,Trigger trigger,Pause pause,
                                      uint32_t& status,uint32_t& result) {
    status=read(1,4);result=0;
    if(status==1) return -2;
    write(0,1,2); // CMD
    write(1,4,1); // ERR: pending
    write(5,4,0x1c); // PAR0
    write(9,4,0);write(13,4,0);write(17,4,0);
    trigger();
    for(unsigned i=0;i<50;++i){
        status=read(1,4);
        if(status!=1){result=read(5,4);return status ? -3 : 0;}
        if(i==49 || !pause()) break;
        trigger();
    }
    return -1;
}
#endif
