#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#define NATIVE_HOST_TEST
#include "../user/notepad.cpp"

static uint64_t damaged_pixels=0,full_submissions=0;
static uint64_t native_call(uint64_t n,uint64_t,uint64_t,uint64_t,uint64_t w,uint64_t h){
    if(n==201){++full_submissions;damaged_pixels+=(uint64_t)surface.w*surface.h;}
    if(n==204)damaged_pixels+=w*h;
    return 0;
}
static void verify_paint(){
    render();std::vector<uint32_t> incremental(surface.pixels,surface.pixels+surface.w*surface.h);
    paint_valid=false;render();
    assert(incremental==std::vector<uint32_t>(surface.pixels,surface.pixels+surface.w*surface.h));
}
int main(){
    std::vector<uint32_t> pixels(760*520);surface={pixels.data(),760,520};
    std::string document;for(unsigned i=0;i<60000;++i)document+=i%77==76?'\n':"Wide letters iW\t"[i%15];
    insert(document.data(),(unsigned)document.size());render();
    for(uint32_t key: {0x1044u,0x1041u,0x1042u,0x1043u,8u,'x'+0u}){key_event(key,0);verify_paint();}
    for(uint32_t key: {0x1044u,0x1041u,0x1042u,0x1043u}){key_event(key,2);verify_paint();}
    key_event(0x1100,1);verify_paint();key_event(0x1101,1);verify_paint();
    handle_event({4,0,0,0,0,3});verify_paint();
    menu=1;verify_paint();menu=-1;verify_paint();
    modal=ABOUT;verify_paint();modal=NORMAL;verify_paint();
    show_status=false;verify_paint();show_status=true;verify_paint();
    for(auto size: {std::pair{480,300},std::pair{1200,720},std::pair{760,520}}){
        pixels.assign(size.first*size.second,0);surface={pixels.data(),size.first,size.second};reveal();verify_paint();}
    key_event('a',1);key_event('Q',0);verify_paint();key_event('z',1);verify_paint();
    key_event(0x1101,1);render();
    auto benchmark=[&](const char* name,uint32_t key,unsigned count){
        damaged_pixels=full_submissions=0;
        auto start=std::chrono::steady_clock::now();
        for(unsigned i=0;i<count;++i){key_event(key,0);render();}
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        printf("%s: %.2f ms for %u keys; %llu damaged pixels; %llu full submissions\n",name,ms,count,
            (unsigned long long)damaged_pixels,(unsigned long long)full_submissions);
        assert(full_submissions==0);
        assert(damaged_pixels<(uint64_t)count*surface.w*LINE_HEIGHT*3);
    };
    benchmark("Move left",0x1044,500);benchmark("Move right",0x1043,500);
    benchmark("Backspace",8,300);benchmark("Type",'x',300);
    assert(length==document.size());
    // Also cover the maximum number of visual rows and a document with no newlines.
    key_event('a',1);std::string lines(CAP,'\n');insert(lines.data(),CAP);
    assert(layout_count==CAP+1);key_event(8,0);assert(layout_count==CAP);verify_paint();
    key_event('a',1);std::string unbroken(CAP,'W');insert(unbroken.data(),CAP);
    key_event(0x1041,0);verify_paint();key_event(8,0);verify_paint();
    // Compare incremental reflow with a fresh layout after arbitrary replacements,
    // especially edits at a wrap boundary, newline, tab, or document end.
    key_event('a',1);insert("",0);unsigned random=711;
    for(unsigned round=0;round<300;++round){auto next=[&](){random=random*1664525u+1013904223u;return random;};
        caret=next()%(length+1);anchor=next()%(length+1);
        std::string replacement;for(unsigned i=next()%80;i;--i)replacement+="Wi \t\r\n"[next()%6];
        insert(replacement.data(),(unsigned)replacement.size());
        unsigned start=0,row=0;
        for(;;){TextRow expected=text_row(start),actual=row_at(row++);
            assert(expected.start==actual.start&&expected.end==actual.end&&expected.next==actual.next);
            if(expected.end==length)break;start=expected.next;}
        assert(row==layout_count);verify_paint();
    }
    puts("Incremental repaint matches full repaint; large-document repeat checks passed");
}
