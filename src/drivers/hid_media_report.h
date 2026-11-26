#ifndef WYZAR_HID_MEDIA_REPORT_H
#define WYZAR_HID_MEDIA_REPORT_H
#include <stddef.h>
#include <stdint.h>
#include "audio_events.h"

// HID 1.11 short items, Consumer Control and Keyboard application collections.
// Track every Input bit (including padding/unrelated usages) per Report ID.
// Only absolute mute/up/down controls are exposed. Vendor and pointer data do
// not become keys. Array selectors follow logical-minimum/usage-list mapping.
struct HIDMediaField {
    uint16_t offset, count;
    uint32_t selector;
    uint8_t bits, mask;
    bool variable;
};
struct HIDMediaReport {
    HIDMediaField fields[24];
    uint16_t input_bytes;
    uint8_t id, count;
};
struct HIDMediaLayout {
    HIDMediaReport reports[8];
    uint8_t count;
    bool numbered, valid;
};
struct HIDMediaState {
    uint8_t reports[8]{}, held=0, repeating=0;
    uint64_t repeat_at=0;
};
static uint8_t hid_media_usage(uint32_t usage) {
    if(usage==0x000c00e2u || usage==0x0007007fu) return 1; // mute
    if(usage==0x000c00eau || usage==0x00070081u) return 2; // down
    if(usage==0x000c00e9u || usage==0x00070080u) return 4; // up
    return 0;
}
static uint8_t hid_boot_media_usage(uint8_t key) {
    return key==0x7f ? 1 : key==0x81 ? 2 : key==0x80 ? 4 : 0;
}
static bool hid_boot_media_decode(const uint8_t* packet,size_t bytes,uint8_t& mask) {
    if(!packet || bytes<8) return false;
    uint8_t next=0;
    for(unsigned i=2;i<8;++i){
        if(packet[i]>=1 && packet[i]<=3) return false; // rollover/error: state unknown
        next|=hid_boot_media_usage(packet[i]);
    }
    mask=next;return true;
}
static bool hid_media_parse(const volatile uint8_t* data,size_t length,HIDMediaLayout& layout) {
    for(size_t i=0;i<sizeof(layout);++i)((uint8_t*)&layout)[i]=0;
    if(!data || !length || length>4096) return false;
    struct Globals {uint32_t page,size,count;int32_t minimum;uint32_t maximum;uint8_t id;};
    Globals g{},stack[8]{};unsigned saved=0,depth=0;
    bool collections[16]{},allowed=false;
    uint16_t offsets[256]{};
    uint32_t usages[64]{},low=0,high=0;
    unsigned used=0;bool have_low=false,have_high=false;
    auto full_usage=[&](uint32_t v,unsigned bytes){return bytes==4?v:(g.page<<16)|(v&65535);};
    auto report=[&]()->HIDMediaReport*{
        for(unsigned i=0;i<layout.count;++i)if(layout.reports[i].id==g.id)return &layout.reports[i];
        if(layout.count==8)return nullptr;
        auto* r=&layout.reports[layout.count++];r->id=g.id;return r;
    };
    auto field=[&](uint16_t offset,uint16_t count,uint32_t selector,uint8_t mask,bool variable){
        auto* r=report();if(!r || r->count==24)return false;
        r->fields[r->count++]={offset,count,selector,(uint8_t)g.size,mask,variable};return true;
    };
    for(size_t cursor=0;cursor<length;){
        uint8_t prefix=data[cursor++];if(prefix==0xfe)return false;
        unsigned bytes=prefix&3;if(bytes==3)bytes=4;
        if(bytes>length-cursor)return false;
        uint32_t value=0;for(unsigned i=0;i<bytes;++i)value|=uint32_t(data[cursor++])<<(8*i);
        unsigned type=(prefix>>2)&3,tag=prefix>>4;
        if(type==1){
            if(tag==0){if(value>65535)return false;g.page=value;}
            else if(tag==1)g.minimum=bytes==1?(int8_t)value:bytes==2?(int16_t)value:(int32_t)value;
            else if(tag==2)g.maximum=value;
            else if(tag==7)g.size=value;
            else if(tag==8){if(!value || value>255)return false;g.id=value;layout.numbered=true;}
            else if(tag==9)g.count=value;
            else if(tag==10){if(saved==8)return false;stack[saved++]=g;}
            else if(tag==11){if(!saved)return false;g=stack[--saved];}
            continue;
        }
        if(type==2){
            if(tag==0){if(used==64)return false;usages[used++]=full_usage(value,bytes);}
            else if(tag==1){low=full_usage(value,bytes);have_low=true;}
            else if(tag==2){high=full_usage(value,bytes);have_high=true;}
            else if(tag==10)return false; // alternative usage sets unsupported
            continue;
        }
        if(type!=0)return false;
        if(tag==10){
            if(depth==16)return false;
            collections[depth++]=allowed;
            uint32_t usage=used?usages[0]:have_low?low:0;
            if(value==1)allowed=usage==0x000c0001u || usage==0x00010006u;
        }else if(tag==12){
            if(!depth)return false;allowed=collections[--depth];
        }else if(tag==8){
            if(have_low!=have_high || (have_low && (high<low || (low>>16)!=(high>>16))))return false;
            uint64_t end=uint64_t(offsets[g.id])+uint64_t(g.size)*g.count;
            if(!g.size || !g.count || end>8192)return false;
            if(allowed && !(value&5) && g.size<=32 && g.minimum>=0){
                if(value&2){
                    for(unsigned i=0;i<g.count;++i){
                        uint32_t usage=used?usages[i<used?i:used-1]:
                            have_low&&have_high&&high>=low&&i<=high-low?low+i:0;
                        uint8_t mask=hid_media_usage(usage);
                        if(mask && !field(offsets[g.id]+i*g.size,1,0,mask,true))return false;
                    }
                }else{
                    // Store only the three selectors we support, not every
                    // entry in a potentially enormous consumer usage range.
                    if(g.count>64)return false;
                    const uint32_t targets[]={0xc00e2,0xc00ea,0xc00e9,0x7007f,0x70081,0x70080};
                    for(auto target:targets){
                        uint32_t index=0;bool found=false;
                        if(used){for(unsigned i=0;i<used;++i)if(usages[i]==target){index=i;found=true;break;}}
                        else if(have_low&&have_high&&high>=low&&target>=low&&target<=high){index=target-low;found=true;}
                        uint64_t selector=uint64_t(index)+(uint32_t)g.minimum;
                        if(found && selector<=g.maximum && selector<(1ULL<<g.size) &&
                           !field(offsets[g.id],g.count,selector,hid_media_usage(target),false))return false;
                    }
                }
            }
            offsets[g.id]=end;
        }else if(tag!=9 && tag!=11)return false;
        used=0;have_low=have_high=false;
    }
    if(depth || saved || (layout.numbered&&offsets[0]))return false;
    for(unsigned i=0;i<layout.count;++i){
        auto& r=layout.reports[i];r.input_bytes=(offsets[r.id]+7)/8;
        if(r.count)layout.valid=true;
    }
    return layout.valid;
}
static uint32_t hid_media_value(const uint8_t* packet,unsigned offset,unsigned bits) {
    uint32_t value=0;
    for(unsigned i=0;i<bits;++i)value|=uint32_t((packet[(offset+i)/8]>>((offset+i)%8))&1)<<i;
    return value;
}
static bool hid_media_decode(const HIDMediaLayout& layout,const uint8_t* packet,size_t bytes,
                             HIDMediaState& state,uint8_t& combined) {
    if(!layout.valid || !packet || !bytes)return false;
    uint8_t id=layout.numbered?*packet++:0;if(layout.numbered)--bytes;
    unsigned index=0;for(;index<layout.count;++index)if(layout.reports[index].id==id)break;
    if(index==layout.count)return false;
    const auto& report=layout.reports[index];if(bytes<report.input_bytes)return false;
    uint8_t next=0;
    for(unsigned i=0;i<report.count;++i){
        const auto& f=report.fields[i];
        for(unsigned slot=0;slot<f.count;++slot){
            uint32_t value=hid_media_value(packet,f.offset+slot*f.bits,f.bits);
            if(f.variable?value!=0:value==f.selector){next|=f.mask;break;}
        }
    }
    state.reports[index]=next;combined=0;
    for(unsigned i=0;i<layout.count;++i)combined|=state.reports[i];
    return true;
}
static uint8_t hid_media_code(uint8_t bit){return bit==1?0x20:bit==2?0x2e:0x30;}
template<class Emit> static void hid_media_update(HIDMediaState& state,uint8_t next,uint64_t now,Emit emit){
    if(next==state.held)return;
    uint8_t pressed=next&~state.held;
    for(uint8_t bit=1;bit<=4;bit<<=1)if(pressed&bit){
        emit(hid_media_code(bit));
        if(bit!=1){state.repeating=bit;state.repeat_at=now+400;}
    }
    state.held=next;
    if(state.repeating && !(next&state.repeating)){
        state.repeating=(next&4)?4:(next&2)?2:0;state.repeat_at=now+400;
    }
    audio_events::notify();
}
template<class Emit> static void hid_media_repeat(HIDMediaState& state,uint64_t now,Emit emit){
    if(state.repeating && now>=state.repeat_at){emit(hid_media_code(state.repeating));state.repeat_at=now+80;}
}
static_assert(sizeof(HIDMediaLayout)<=4096);
#endif
