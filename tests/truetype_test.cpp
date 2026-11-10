#include <cassert>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
#include "../src/gui/truetype.h"
int main(int argc,char**argv){
    assert(argc==2);
    std::ifstream in(argv[1],std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});
    auto atlas=std::make_unique<ttf::Atlas>();auto scratch=std::make_unique<ttf::Scratch>();
    {
        // A synthetic composite rotates a two-point outline by 90 degrees.
        uint8_t data[48]={},loca[12]={};
        auto put=[&](unsigned o,int value){data[o]=value>>8;data[o+1]=value;};
        put(0,1);put(10,1);data[14]=data[15]=1;
        put(16,10);put(18,20);put(20,20);put(22,20);
        put(24,-1);put(34,0x82);put(36,0);put(40,0);put(42,16384);put(44,-16384);put(46,0);
        loca[7]=24;loca[11]=48;
        ttf::Face synthetic{};synthetic.count=2;synthetic.locsize=4;
        synthetic.loca={loca,sizeof(loca)};synthetic.glyf={data,sizeof(data)};
        assert(scratch->outline(synthetic,1));
        assert(scratch->np==2&&scratch->points[0].x==-20&&scratch->points[0].y==10);
        assert(scratch->points[1].x==-40&&scratch->points[1].y==30);
    }
    assert(ttf::bake(bytes.data(),bytes.size(),*atlas,*scratch));
    ui_active_fonts=atlas->fonts;ui_active_font_count=8;
    assert(ui_font_for_pixels(14)==&atlas->fonts[1]);
    assert(ui_text_width(ui_font_for_pixels(14),"WWW")>ui_text_width(ui_font_for_pixels(14),"iii"));
    std::vector<uint32_t> image(240*64,0xff102030),tiled=image;
    ui_text_draw(image.data(),240,64,240,ui_font_for_pixels(18),2,2,"TrueType desktop 123",0xffffffff,0,0,240,64);
    for(int y=0;y<64;y+=4)for(int x=0;x<240;x+=8)
        ui_text_draw(tiled.data(),240,64,240,ui_font_for_pixels(18),2,2,"TrueType desktop 123",0xffffffff,x,y,x+8,y+4);
    assert(image==tiled);bool gray=false;for(auto p:image)if(p!=0xff102030&&p!=0xffffffff)gray=true;assert(gray);
    ttf::Face face{};assert(face.init(bytes.data(),bytes.size()));
    // Exercise composites (accented Latin) as well as the ASCII atlas.
    unsigned id;assert(face.glyph(0xe9,id)&&id);scratch->np=scratch->nc=scratch->components=0;
    assert(scratch->outline(face,id));assert(scratch->np&&scratch->nc);
    for(size_t n=0;n<bytes.size();n+=97){ttf::Face truncated{};assert(!truncated.init(bytes.data(),n));}
    assert(!ttf::bake(nullptr,bytes.size(),*atlas,*scratch));
    auto corrupt=bytes;corrupt[0]='O';corrupt[1]='T';corrupt[2]='T';corrupt[3]='O';
    assert(!ttf::bake(corrupt.data(),corrupt.size(),*atlas,*scratch));
    // Seeded mutations exercise bounds checking under ASan/UBSan.
    uint32_t seed=42;
    for(unsigned run=0;run<300;++run){
        corrupt=bytes;
        for(int j=0;j<4;++j){seed=seed*1664525+1013904223;size_t off=seed%corrupt.size();seed=seed*1664525+1013904223;corrupt[off]=seed>>24;}
        ttf::bake(corrupt.data(),corrupt.size(),*atlas,*scratch);
    }
}
