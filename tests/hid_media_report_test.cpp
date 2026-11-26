#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "src/drivers/hid_media_report.h"
static unsigned wake_calls=0;
static void wake_test(){++wake_calls;}

static const uint8_t bitmap[]={
    5,0x0c,9,1,0xa1,1,0x85,3,0x15,0,0x25,1,
    9,0xe2,9,0xea,9,0xe9,0x75,1,0x95,3,0x81,2,
    0x75,5,0x95,1,0x81,3,0xc0
};
static const uint8_t selector[]={
    5,0x0c,9,1,0xa1,1,0x85,4,0x15,0,0x26,0xff,3,
    0x19,0,0x2a,0xff,3,0x75,0x10,0x95,2,0x81,0,0xc0
};
int main(){
    HIDMediaLayout layout{};HIDMediaState state{};uint8_t held=99;
    assert(hid_media_parse(bitmap,sizeof(bitmap),layout));
    assert(layout.numbered&&layout.count==1&&layout.reports[0].input_bytes==1);
    for(unsigned bits=0;bits<8;++bits){
        uint8_t packet[]={3,(uint8_t)bits};
        assert(hid_media_decode(layout,packet,2,state,held)&&held==bits);
    }
    // Other Report IDs and incomplete transfers do not synthesize releases.
    uint8_t unknown[]={2,0},short_packet[]={3};
    assert(!hid_media_decode(layout,unknown,2,state,held)&&held==7);
    assert(!hid_media_decode(layout,short_packet,1,state,held)&&held==7);
    assert(!hid_media_decode(layout,nullptr,0,state,held)&&held==7);
    assert(hid_media_parse(selector,sizeof(selector),layout));state={};
    uint8_t array[]={4,0xe9,0,0xe2,0};
    assert(hid_media_decode(layout,array,5,state,held)&&held==5);
    array[1]=0xea;assert(hid_media_decode(layout,array,5,state,held)&&held==3);
    for(unsigned n=0;n<5;++n)assert(!hid_media_decode(layout,array,n,state,held)&&held==3);
    array[1]=0x38;array[2]=2;array[3]=0; // AC Pan is not volume.
    assert(hid_media_decode(layout,array,5,state,held)&&!held);
    // Explicit array usages are selectors indexed by Logical Minimum, not
    // literal consumer codes. Null/out-of-range entries are ignored.
    const uint8_t list[]={5,0x0c,9,1,0xa1,1,0x15,1,0x25,3,
        9,0xe2,9,0xea,9,0xe9,0x75,8,0x95,1,0x81,0,0xc0};
    assert(hid_media_parse(list,sizeof(list),layout));state={};
    for(uint8_t value=0;value<5;++value){
        assert(hid_media_decode(layout,&value,1,state,held));
        assert(held==(value>=1&&value<=3 ? 1u<<(value-1) : 0));
    }
    // Unaligned fields and constant padding consume bits, Output/Feature do
    // not. Two IDs maintain independent held state within the same endpoint.
    const uint8_t mixed[]={5,0x0c,9,1,0xa1,1,0x85,1,0x15,0,0x25,1,
        0x75,3,0x95,1,0x81,3,0x75,1,9,0xea,0x81,2,
        0x75,8,0x91,3,0xb1,3,0x85,2,0x75,1,9,0xe2,0x81,2,0xc0};
    assert(hid_media_parse(mixed,sizeof(mixed),layout));state={};
    uint8_t down[]={1,8},mute[]={2,1},release_mute[]={2,0};
    assert(hid_media_decode(layout,down,2,state,held)&&held==2);
    assert(hid_media_decode(layout,mute,2,state,held)&&held==3);
    assert(hid_media_decode(layout,release_mute,2,state,held)&&held==2);
    // A vendor page masquerading as known usage IDs cannot generate volume.
    auto vendor=std::vector<uint8_t>(bitmap,bitmap+sizeof(bitmap));vendor[1]=0xff;
    assert(!hid_media_parse(vendor.data(),vendor.size(),layout));
    // No implicit Report ID guessing; non-numbered packets begin at bit zero.
    auto plain=std::vector<uint8_t>(bitmap,bitmap+sizeof(bitmap));plain.erase(plain.begin()+6,plain.begin()+8);
    assert(hid_media_parse(plain.data(),plain.size(),layout)&&!layout.numbered);state={};
    uint8_t value=4;assert(hid_media_decode(layout,&value,1,state,held)&&held==4);
    // Boot usages 0x7f/80/81 share the mixer, but F1-F3, digits and rollover
    // never become audio actions or guessed Fn combinations.
    uint8_t boot[]={0,0,0x7f,0x80,0x81,0,0,0};
    assert(hid_boot_media_decode(boot,8,held)&&held==7);
    boot[2]=1;assert(!hid_boot_media_decode(boot,8,held)&&held==7);
    boot[2]=0x3a;boot[3]=0x3b;boot[4]=0x3c;
    assert(hid_boot_media_decode(boot,8,held)&&!held);
    assert(!hid_boot_media_decode(boot,7,held));
    // Independent keyboards: a release on B must not cancel A's repeat.
    HIDMediaState a{},b{};std::vector<unsigned> emitted;
    auto emit=[&](unsigned key){emitted.push_back(key);};
    hid_media_update(a,4,100,emit);assert(emitted==std::vector<unsigned>{0x30});
    hid_media_update(a,4,150,emit);assert(emitted.size()==1);
    hid_media_update(b,2,200,emit);assert(emitted.back()==0x2e);
    hid_media_update(b,0,210,emit);hid_media_repeat(a,499,emit);assert(emitted.size()==2);
    hid_media_repeat(a,500,emit);assert(emitted.size()==3&&emitted.back()==0x30);
    hid_media_repeat(a,580,emit);assert(emitted.size()==4);
    hid_media_repeat(a,2000,emit);assert(emitted.size()==5); // no catch-up burst
    hid_media_update(a,0,2001,emit);hid_media_repeat(a,3000,emit);assert(emitted.size()==5);
    hid_media_update(b,1,3100,emit);hid_media_update(b,1,3200,emit);
    hid_media_repeat(b,4000,emit);assert(emitted.size()==6&&emitted.back()==0x20);
    hid_media_update(b,0,4100,emit);hid_media_update(b,1,4200,emit);assert(emitted.size()==7);
    b={};audio_events::wake=wake_test;
    hid_media_update(b,0,0,emit);assert(!wake_calls);
    hid_media_update(b,4,100,emit);assert(wake_calls==1);
    hid_media_update(b,4,110,emit);assert(wake_calls==1);
    hid_media_update(b,0,120,emit);assert(wake_calls==2); // cancel the worker deadline on release
    audio_events::wake=nullptr;
    // Truncation, stack/collection balance, bad ranges, report-ID zero and
    // bounded field/report counts fail closed under ASan/UBSan.
    for(size_t length=0;length<sizeof(bitmap);++length)assert(!hid_media_parse(bitmap,length,layout));
    const uint8_t badid[]={0x85,0};assert(!hid_media_parse(badid,sizeof(badid),layout));
    const uint8_t badpop[]={0xb4};assert(!hid_media_parse(badpop,sizeof(badpop),layout));
    const uint8_t longitem[]={0xfe,0,0};assert(!hid_media_parse(longitem,sizeof(longitem),layout));
    uint32_t random=0x56781234;
    for(unsigned i=0;i<10000;++i){
        uint8_t fuzz[64];for(auto& c:fuzz){random=random*1664525+1013904223;c=random>>24;}
        hid_media_parse(fuzz,i%65,layout);
    }
    puts("PASS: HID media bitmaps/arrays/IDs, boot usages, malformed input, independent keyboards and hold/release");
}
