#include <cassert>
#include <cstdint>
#include <vector>
#include <fstream>
#include <iterator>
#include "../src/drivers/thinkpad_ec_protocol.h"
#include "../src/drivers/thinkpad_hotkey_profile.h"
#include "../src/drivers/thinkpad_smi.h"
#include "../src/drivers/acpi_mode.h"
#include "../src/drivers/audio_hotkeys.h"
static unsigned wake_calls=0;
static void wake_test(){++wake_calls;}

struct Ec {
    uint8_t memory[256]{}, status = 0, cmd = 0, address = 0;
    bool addressed = false;
    unsigned waits = 0, fail_wait = 0, reads = 0;
    std::vector<unsigned> writes;
    uint8_t in(uint16_t port) {
        if (port == 0x66) return status;
        assert(port == 0x62 && (status & 1));
        status &= ~1; ++reads; return memory[address];
    }
    void out(uint16_t port, uint8_t value) {
        writes.push_back(((unsigned)port << 8) | value);
        if (port == 0x66) {
            assert(value == 0x80 || value == 0x81);
            cmd = value; addressed = false;
        } else {
            assert(port == 0x62 && cmd);
            if (!addressed) {
                address = value; addressed = true;
                if (cmd == 0x80) status |= 1;
            } else {
                assert(cmd == 0x81); memory[address] = value; cmd = 0;
            }
        }
    }
    bool transaction(bool writing, uint8_t& value) {
        auto input = [&](uint16_t port) { return in(port); };
        auto output = [&](uint16_t port, uint8_t byte) { out(port, byte); };
        auto wait = [&](auto read, unsigned mask, unsigned expected) {
            ++waits; return waits != fail_wait && (read() & mask) == expected;
        };
        ThinkpadEcProtocol<decltype(input), decltype(output), decltype(wait)> protocol{0x62, 0x66, input, output, wait};
        return writing ? protocol.write(0x30, value) : protocol.read(0x30, value);
    }
};
int main(int argc,char** argv) {
    // Firmware owns SCI_EN: only the FADT enable command is written. Preserve
    // all PM1 control bits; wait for both PM1 blocks when a second is present.
    for(unsigned completion : {0,1,3,300,301}){
        for(bool second : {false,true}){
            unsigned writes=0,pauses=0;
            uint16_t a=0x100,b=0x200;
            auto read=[&](uint16_t port){assert(port==0x1804 || (second&&port==0x1810));return port==0x1804 ? a : b;};
            auto write=[&](uint16_t port,uint8_t value){assert(port==0xb2&&value==0xf0);++writes;if(!completion){a|=1;b|=1;}};
            auto pause=[&](){if(++pauses==completion){a|=1;b|=1;}return true;};
            int rc=acpi_enter_mode({0xb2,0x1804,uint16_t(second?0x1810:0),0xf0},read,write,pause);
            assert(rc==(completion<=300 ? 1 : -1) && writes==1 && pauses==(completion<300?completion:300));
            assert((a&~1u)==0x100 && (b&~1u)==0x200);
        }
    }
    {
        auto no_write=[](uint16_t,uint8_t){assert(false);};
        auto no_pause=[](){assert(false);return false;};
        assert(acpi_enter_mode({0,0x1804,0,0},[](uint16_t){return 1u;},no_write,no_pause)==0);
        for(auto ports : {AcpiModePorts{0,0x1804,0,0xf0},{0x10000,0x1804,0,0xf0},{0xb2,0x1804,0,0},{0xb2,0,0,0xf0},{0xb2,65535,0,0xf0},{0xb2,0x1804,65535,0xf0}}){
            assert(acpi_enter_mode(ports,[](uint16_t){return 0u;},no_write,no_pause)==-3);
        }
        unsigned writes=0;
        assert(acpi_enter_mode({0xb2,0x1804,0,0xf0},[](uint16_t){return 0u;},[&](uint16_t,uint8_t){++writes;},[](){return false;})==-1 && writes==1);
        writes=0;
        assert(acpi_enter_mode({0xb2,0x1804,0x1810,0xf0},[](uint16_t p){return p==0x1804 ? 1u : 0u;},
            [&](uint16_t,uint8_t){++writes;},[](){return false;})==-1 && writes==1);
    }
    if(argc==2){
        std::ifstream file(argv[1],std::ios::binary);
        std::vector<uint8_t> table{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        assert(thinkpad_hotkey_profile(table.data(),table.size()));
        assert(thinkpad_linux_hotkey_mailbox(table.data(),table.size())==0xbbe8f018ULL);
        // Repair the checksum after corrupting each profiled field/method byte:
        // rejection must come from the profile, not just the table checksum.
        for(auto range : {std::pair<unsigned,unsigned>{21457,98},{47934,45},{10,18}}){
            for(unsigned i=range.first;i<range.first+range.second;++i){
                auto changed=table;changed[i]^=1;
                uint8_t sum=0;for(auto byte:changed)sum+=byte;changed[9]-=sum;
                assert(!thinkpad_hotkey_profile(changed.data(),changed.size()));
            }
        }
        assert(!thinkpad_hotkey_profile(table.data(),table.size()-1));
        for(auto range : {std::pair<unsigned,unsigned>{2266,8},{2278,3},{76170,167},{76841,21},{12178,50}}){
            for(unsigned i=range.first;i<range.first+range.second;++i){
                auto changed=table;changed[i]^=1;
                uint8_t sum=0;for(auto byte:changed)sum+=byte;changed[9]-=sum;
                assert(!thinkpad_linux_hotkey_mailbox(changed.data(),changed.size()));
            }
        }
        auto relocated=table;relocated[2274]^=0x40;
        uint8_t sum=0;for(auto byte:relocated)sum+=byte;relocated[9]-=sum;
        assert(thinkpad_linux_hotkey_mailbox(relocated.data(),relocated.size())==0xbbe8f058ULL);
    }
    // Actual mailbox handshake: exact argument layout, delayed completion,
    // bounded failure, firmware errors, and untouched busy mailbox/neighbors.
    for(unsigned complete : {1,3,50,51}){
        for(unsigned error : {0,7}){
            uint8_t bytes[23];for(auto& b:bytes)b=0xa5;
            auto read=[&](unsigned o,unsigned n){assert(o+n<=21);uint32_t v=0;for(unsigned i=0;i<n;++i)v|=uint32_t(bytes[1+o+i])<<(8*i);return v;};
            auto write=[&](unsigned o,unsigned n,uint32_t v){assert(o+n<=21);for(unsigned i=0;i<n;++i)bytes[1+o+i]=v>>(8*i);};
            write(1,4,0);unsigned calls=0,pauses=0;
            auto trigger=[&](){
                assert(read(0,1)==2 && read(5,4)==0x1c && !read(9,4) && !read(13,4) && !read(17,4));
                if(++calls==complete){write(1,4,error);write(5,4,0x12345678);}
            };
            auto pause=[&](){++pauses;return true;};
            uint32_t status=99,result=99;
            int rc=thinkpad_linux_hotkey_init(read,write,trigger,pause,status,result);
            assert(rc==(complete>50 ? -1 : error ? -3 : 0));
            assert(calls==(complete>50 ? 50 : complete) && pauses==calls-1);
            assert(status==(complete>50 ? 1 : error));
            assert(result==(complete>50 ? 0 : 0x12345678));
            assert(bytes[0]==0xa5 && bytes[22]==0xa5);
        }
    }
    {
        unsigned writes=0,calls=0;
        uint32_t status=0,result=0;
        int rc=thinkpad_linux_hotkey_init([](unsigned o,unsigned n){assert(o==1&&n==4);return 1u;},
            [&](unsigned,unsigned,uint32_t){++writes;},[&](){++calls;},[](){assert(false);return false;},status,result);
        assert(rc==-2 && !writes && !calls && status==1);
        unsigned reads=0;
        rc=thinkpad_linux_hotkey_init([&](unsigned,unsigned){return reads++ ? 1u : 0u;},
            [](unsigned,unsigned,uint32_t){},[&](){++calls;},[](){return false;},status,result);
        assert(rc==-1 && calls==1);
    }
    assert(thinkpad_audio_identity(0x9d718086, 0x505b17aa, 0x14f15111));
    assert(!thinkpad_audio_identity(0x9d718086, 0x505b17aa, 0x10ec0897));
    assert(!thinkpad_audio_identity(0x26688086, 0x505b17aa, 0x14f15111));
    assert(!thinkpad_audio_identity(0x9d718086, 0x123417aa, 0x14f15111));
    for (unsigned state = 0; state < 256; ++state) {
        {
            uint8_t actual=state, before=0, after=0; unsigned writes=0;
            auto read=[&](unsigned a,uint8_t& v){assert(a==3);v=actual;return true;};
            auto write=[&](unsigned a,uint8_t v){assert(a==3);actual=v;++writes;return true;};
            int expected=(state&0x60)==0x40 ? 0 : 1;
            assert(thinkpad_ec_hotkey_update(read,write,before,after)==expected);
            assert(before==state && after==((state&0x9f)|0x40));
            assert((before&0x9f)==(after&0x9f) && writes==(unsigned)expected);
            assert(thinkpad_ec_hotkey_update(read,write,before,after)==0 && writes==(unsigned)expected);
        }
        uint8_t actual = state, before = 0, after = 0;
        unsigned writes = 0;
        auto read = [&](unsigned address, uint8_t& value) { assert(address == 0x30); value = actual; return true; };
        auto write = [&](unsigned address, uint8_t value) { assert(address == 0x30); actual = value; ++writes; return true; };
        assert(thinkpad_ec_audio_update(1, read, write, before, after) == ((state & 0x40) ? 1 : 0));
        assert(before == state && after == (state & ~0x40u) && actual == after);
        assert(writes == ((state & 0x40) ? 1u : 0u));
        assert((before & ~0x40u) == (after & ~0x40u));
        assert(thinkpad_ec_audio_update(2, read, write, before, after) == 1 && (after & 0x40));
        assert(thinkpad_ec_audio_update(2, read, write, before, after) == 1 && !(after & 0x40));
        uint32_t next = thinkpad_global_lock_claim(state);
        assert((next & ~3u) == (state & ~3u) && (next & 2));
        assert(bool(next & 1) == bool(state & 2));
    }
    uint8_t before = 0, after = 0;
    assert(!thinkpad_hotkey_profile(nullptr,78213));
    uint8_t empty[36]{};
    assert(!thinkpad_hotkey_profile(empty,sizeof(empty)));
    auto hotkey_stale=[](unsigned a,uint8_t& v){assert(a==3);v=0x9f;return true;};
    auto hotkey_write=[](unsigned a,uint8_t v){assert(a==3 && v==0xdf);return true;};
    assert(thinkpad_ec_hotkey_update(hotkey_stale,hotkey_write,before,after)==-2);
    bool wrote=false;
    assert(thinkpad_ec_hotkey_update([](unsigned,uint8_t&){return false;},[&](unsigned,uint8_t){wrote=true;return true;},before,after)==-1 && !wrote);
    auto stale_read = [&](unsigned, uint8_t& value) { value = 0x4e; return true; };
    auto write = [&](unsigned, uint8_t value) { assert(value == 0x0e); return true; };
    assert(thinkpad_ec_audio_update(1, stale_read, write, before, after) == -2);
    Ec ec; ec.memory[0x30] = 0x4e;
    uint8_t value = 0;
    assert(ec.transaction(false, value) && value == 0x4e && ec.reads == 1);
    assert(ec.writes == std::vector<unsigned>({0x6680, 0x6230}));
    value = 0x0e; ec.writes.clear();
    assert(ec.transaction(true, value) && ec.memory[0x30] == 0x0e);
    assert(ec.writes == std::vector<unsigned>({0x6681, 0x6230, 0x620e}));
    for (unsigned stage = 1; stage <= 3; ++stage) {
        Ec failed; failed.fail_wait = stage; value = 0xa5;
        assert(!failed.transaction(false, value) && value == 0xa5 && !failed.reads);
        assert(failed.writes.size() == stage - 1);
    }
    for (unsigned stage = 1; stage <= 4; ++stage) {
        Ec failed; failed.fail_wait = stage; value = 0x0e;
        assert(!failed.transaction(true, value) && failed.writes.size() == stage - 1);
    }
    for (uint8_t status : {1, 2, 3, 255}) {
        Ec occupied; occupied.status = status;
        assert(!occupied.transaction(false, value) && occupied.writes.empty() && !occupied.reads);
    }
    AudioHotkeyQueue keys{};
    for(uint8_t code : {0x3b,0x3c,0x3d,0xbb,0xbc,0xbd}){
        assert(!keys.accept(false,code,false,100));
        assert(!keys.accept(false,code,true,100));
    }
    keys.tick(1000);assert(!keys.pop() && !keys.received && !keys.held);
    assert(!keys.accept(false, 0x30, false)); // ordinary B
    assert(keys.accept(true, 0x30, false));
    assert(keys.accept(true, 0x30, true) && keys.accept(true, 0xb0, false));
    assert(keys.received == 1 && keys.pop() == 0x30 && !keys.pop());
    for (unsigned i = 0; i < 9; ++i) {
        unsigned key=i & 1 ? 0x2e : 0x20;
        assert(keys.accept(true,key,false));assert(keys.accept(true,key|0x80,false));
    }
    assert(keys.count == 8 && keys.dropped == 1);
    for (unsigned i = 0; i < 8; ++i) assert(keys.pop() == (i & 1 ? 0x2e : 0x20));
    assert(!keys.pop());
    keys = {};
    assert(keys.accept(true,0x30,false,100)&&keys.pop()==0x30);
    keys.tick(499);assert(!keys.pop());keys.tick(500);assert(keys.pop()==0x30);
    assert(keys.accept(true,0x30,true,510));assert(!keys.pop());
    keys.tick(580);assert(keys.pop()==0x30);keys.tick(1000);assert(keys.pop()==0x30&&!keys.pop());
    assert(keys.accept(true,0xb0,false,1001));keys.tick(2000);assert(!keys.pop());
    assert(keys.accept(true,0x20,false,2100)&&keys.pop()==0x20);
    assert(keys.accept(true,0x20,true,2600));keys.tick(3000);assert(!keys.pop());
    keys={};audio_events::wake=wake_test;
    assert(keys.accept(true,0xb0,false) && !wake_calls); // orphan release
    assert(!keys.accept(false,0x3d,false) && !wake_calls);
    assert(keys.accept(true,0x30,false,100) && wake_calls==1);
    assert(keys.accept(true,0x30,true,110) && wake_calls==1);
    assert(keys.accept(true,0xb0,false,120) && wake_calls==2);
    assert(keys.accept(true,0xb0,false,130) && wake_calls==2);
    audio_events::wake=nullptr;
}
