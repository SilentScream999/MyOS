#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define NATIVE_HOST_TEST
#include "../user/explorer.cpp"
#include "../src/core/text_clipboard.h"
static TextClipboard text_clipboard;

namespace fs=std::filesystem;
static fs::path sandbox;
static bool write_failure=false;
static bool create_failure=false;
static bool cross_volume_move=false;
static int64_t eject_result=0;
static std::string ejected_path;
static uint64_t mount_generation=0;
static unsigned spawned=0,presents=0;
static unsigned partial_presents=0;
static bool publication_failure=false;
static std::vector<uint32_t> published_pixels;
static uint64_t damage_x,damage_y,damage_w,damage_h;
static std::string opened_document,opened_app;
static bool launch_failure=false;
static std::string resolve(uint64_t value) { return sandbox.string()+(const char*)value; }
static uint64_t native_call(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e) {
    switch(n){
    case 0:return read((int)a,(void*)b,c);
    case 1:return write_failure?(uint64_t)-1:write((int)a,(void*)b,c);
    case 2:{if(create_failure&&(b&0x40))return -1;int flags=(int)b;
#ifdef _WIN32
        // Native ABI flags follow Linux values; translate for the Windows host.
        flags=O_BINARY|((b&3)==2?O_RDWR:(b&3)==1?O_WRONLY:O_RDONLY);
        if(b&0x40)flags|=O_CREAT;if(b&0x80)flags|=O_EXCL;
        if(b&0x200)flags|=O_TRUNC;if(b&0x400)flags|=O_APPEND;
#endif
        return open(resolve(a).c_str(),flags,0644);}
    case 3:return close((int)a);
    case 4:{struct stat st{};if(stat(resolve(a).c_str(),&st))return -1;
        auto* info=(file_info*)b;*info={};info->size=st.st_size;info->type=S_ISDIR(st.st_mode)?2:1;return 0;}
    case 8:return lseek((int)a,(off_t)b,(int)c);
    case 39:return 42;
    case 40:{std::error_code error;std::vector<fs::directory_entry> items;
        for(auto it=fs::directory_iterator(resolve(a),error);!error&&it!=fs::directory_iterator();it.increment(error))items.push_back(*it);
        if(error)return -1;if(b>=items.size())return 0;
        auto& item=items[b];std::strcpy((char*)c,item.path().filename().string().c_str());
        *(uint64_t*)e=item.is_directory()?2:1;*(uint64_t*)d=item.is_directory()?0:item.file_size();return 1;}
    case 59:case 213:++spawned;return 100+spawned;
    case 216:opened_app=(char*)a;opened_document=(char*)b;return launch_failure?(uint64_t)-1:100+(++spawned);
    case 217:return a==0?text_clipboard.set((char*)b,c):a==1?text_clipboard.get((char*)b,c):-1;
    case 218:{static uint64_t tick=0;tick+=100;((ProcessMetadata*)d)->tick_ms=tick;return 0;}
    case 220:ejected_path=(const char*)a;return eject_result;
    case 221:{if(a==0)return mount_generation;const char* path=(const char*)b;
        return strncmp(path,"/mnt/",5)==0&&fs::is_directory(resolve(b))?1:0;}
    case 74:
#ifdef _WIN32
        return _commit((int)a);
#else
        return fsync((int)a);
#endif
    case 80:return fs::is_directory(resolve(a))?0:-1;
    case 82:{if(cross_volume_move&&!strstr((const char*)a,".explorer-copy-"))return -1;
        if(fs::exists(resolve(b)))return -1;std::error_code ec;fs::rename(resolve(a),resolve(b),ec);return ec?-1:0;}
    case 83:{std::error_code ec;bool made=fs::create_directory(resolve(a),ec);return made&&!ec?0:-1;}
    case 84:return rmdir(resolve(a).c_str());
    case 87:return unlink(resolve(a).c_str());
    case 138:{std::ofstream out(resolve(a),std::ios::binary);out.write((const char*)b,c);return out?0:-1;}
    case 201:++presents;return 0;
    case 222:
        if(publication_failure)return -1;
        published_pixels.assign(surface.pixels,surface.pixels+surface.w*surface.h);
        if(!b)++presents;return 0;
    case 204:assert(d>0&&e>0&&b+d<=(uint64_t)surface.w&&c+e<=(uint64_t)surface.h);
        damage_x=b;damage_y=c;damage_w=d;damage_h=e;++partial_presents;return 0;
    default:return -1;
    }
}
static void put(const char* path,const std::string& text) { std::ofstream out(sandbox.string()+path,std::ios::binary);out<<text; }
static std::string get(const char* path) { std::ifstream in(sandbox.string()+path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{}); }
static void choose(const char* name) {
    for(int i=0;i<entry_count;++i)if(!std::strcmp(entries[i].name,name)){select_row(i);return;}
    assert(false&&"missing explorer entry");
}
static void complete_transfer() {
    while(transfer.active)advance_transfer();
    if(dialog==TRANSFER)cancel_transfer();
}
int main(int argc,char** argv) {
    assert(argc==3);sandbox=fs::absolute(argv[1]);
    fs::create_directories(sandbox/"home/destination");fs::create_directories(sandbox/"home/zFolder");
    fs::create_directories(sandbox/"home/Applications");
    put("/home/zeta.txt","hello explorer\n");put("/home/alpha.bin",std::string("a\0b",3));
    put("/home/.hidden","hidden");put("/home/large.txt",std::string(9001,'X'));put("/home/empty.txt","");
    assert(navigate("/home"));assert(entries[0].type==2);assert(entry_count==7);
    assert(!navigate("/missing"));assert(!std::strcmp(current_dir,"/home"));
    assert(!navigate(std::string(300,'x').c_str()));assert(!valid_filename(".."));assert(!valid_filename("a/b"));
    assert(navigate("destination"));history_step(-1);assert(!std::strcmp(current_dir,"/home"));
    history_step(1);assert(!std::strcmp(current_dir,"/home/destination"));navigate("..");
    begin_dialog(NEW_FOLDER);native_copy(edit_text,256,"New folder");commit_dialog();assert(fs::is_directory(sandbox/"home/New folder"));
    begin_dialog(NEW_FILE);native_copy(edit_text,256,"note.txt");commit_dialog();assert(fs::exists(sandbox/"home/note.txt"));
    put("/home/note.txt","do not overwrite");begin_dialog(NEW_FILE);native_copy(edit_text,256,"note.txt");commit_dialog();
    assert(dialog==NEW_FILE&&get("/home/note.txt")=="do not overwrite");dialog=NONE;load_directory();
    choose("note.txt");begin_dialog(RENAME);native_copy(edit_text,256,"renamed.txt");commit_dialog();
    assert(get("/home/renamed.txt")=="do not overwrite"&&!fs::exists(sandbox/"home/note.txt"));
    choose("large.txt");copy_selection(false);navigate("destination");paste_selection();complete_transfer();
    assert(get("/home/destination/large.txt")==std::string(9001,'X'));paste_selection();
    assert(dialog==TRANSFER&&!transfer.active&&!transfer.success&&strstr(transfer.message,"already exists"));
    complete_transfer();
    assert(get("/home/destination/large.txt")==std::string(9001,'X'));
    navigate("/home");choose("empty.txt");copy_selection(false);navigate("destination");paste_selection();complete_transfer();
    assert(fs::exists(sandbox/"home/destination/empty.txt"));
    navigate("/home");choose("zeta.txt");copy_selection(false);navigate("destination");write_failure=true;paste_selection();complete_transfer();write_failure=false;
    assert(!fs::exists(sandbox/"home/destination/zeta.txt")&&get("/home/zeta.txt")=="hello explorer\n");
    for(auto& item:fs::directory_iterator(sandbox/"home/destination"))assert(item.path().filename().string().find(".explorer-copy-")!=0);
    navigate("/home");choose("renamed.txt");copy_selection(true);navigate("destination");paste_selection();complete_transfer();
    assert(!fs::exists(sandbox/"home/renamed.txt")&&get("/home/destination/renamed.txt")=="do not overwrite");
    choose("renamed.txt");begin_dialog(DELETE_ITEM);assert(fs::exists(sandbox/"home/destination/renamed.txt"));commit_dialog();
    assert(!fs::exists(sandbox/"home/destination/renamed.txt"));
    navigate("/home");choose("destination");begin_dialog(DELETE_ITEM);commit_dialog();assert(dialog==DELETE_ITEM);dialog=NONE;
    choose("New folder");begin_dialog(DELETE_ITEM);commit_dialog();assert(!fs::exists(sandbox/"home/New folder"));
    choose("zeta.txt");open_selected();assert(dialog==NONE&&opened_document=="/home/zeta.txt"&&opened_app=="/bin/notepad.wiz");
    choose("alpha.bin");open_selected();assert(dialog==PREVIEW&&std::strstr(dialog_text,"binary content"));dialog=NONE;
    put("/home/demo.wiz","fake executable");load_directory();choose("demo.wiz");open_selected();assert(spawned==2);
    choose("alpha.bin");int menu_row=selected;
    handle_event({1,WIDTH-30,LIST_TOP+menu_row*ROW+8,2,0,0});assert(context_open&&selected==menu_row&&context_x+180<=WIDTH&&context_y+CONTEXT_HEIGHT<=HEIGHT-36);
    handle_event({1,context_x+30,context_y+44,1,0,0});assert(dialog==OPEN_WITH&&!context_open);
    handle_event({5,0,0,'\n',0,0});assert(dialog==NONE&&opened_document=="/home/alpha.bin");
    choose_open_with();launch_failure=true;handle_event({5,0,0,'\n',0,0});assert(dialog==OPEN_WITH);launch_failure=false;
    handle_event({5,0,0,27,0,0});assert(dialog==NONE);
    choose("destination");choose_open_with();assert(dialog==NONE);
    put("/home/photo.PNG","fixture");load_directory();choose("photo.PNG");open_selected();
    assert(dialog==NONE&&opened_document=="/home/photo.PNG"&&opened_app=="/bin/image_viewer.wiz");
    choose_open_with();launch_failure=true;handle_event({5,0,0,'\n',0,0});assert(dialog==OPEN_WITH);launch_failure=false;
    handle_event({5,0,0,'i',0,0});assert(dialog==NONE&&opened_app=="/bin/image_viewer.wiz");
    assert(image_document("photo.JPEG")&&image_document("photo.jpg")&&!image_document("photo.png.txt"));
    // Path text is shared independently of file-copy state, including spaces.
    choose("zeta.txt");copy_selection(false);std::string file_copy=clipboard;
    handle_event({1,250,LIST_TOP+selected*ROW+8,2,0,0});assert(context_open);
    handle_event({1,context_x+25,context_y+6+4*31+12,1,0,0});assert(!context_open&&!strcmp(text_clipboard.data(),"/home/zeta.txt"));
    assert(file_copy==clipboard&&!clipboard_cut);
    choose("destination");handle_event({5,0,0,'C',3,0});assert(!strcmp(text_clipboard.data(),"/home/destination"));
    assert(navigate("/home/destination"));
    handle_event({5,0,0,'v',WIN_MOD_CTRL|WIN_MOD_TEXT_PASTE,0});
    assert(!fs::exists(sandbox/"home/destination/zeta.txt"));
    assert(navigate("/home"));
    handle_event({5,0,0,'l',1,0});handle_event({5,0,0,'v',WIN_MOD_CTRL|WIN_MOD_TEXT_PASTE,0});
    assert(!strcmp(address,"/home/destination"));
    handle_event({5,0,0,27,0,0});
    put("/home/name with spaces.txt","");load_directory();choose("name with spaces.txt");copy_as_path();
    assert(!strcmp(text_clipboard.data(),"/home/name with spaces.txt"));
    selected=-1;copy_as_path();assert(!strcmp(text_clipboard.data(),"/home/name with spaces.txt"));
    show_hidden=true;load_directory();choose(".hidden");show_hidden=false;load_directory();
    fs::create_directories(sandbox/"many");for(int i=0;i<513;++i)put(("/many/file"+std::to_string(i)).c_str(),"");
    navigate("/many");assert(entry_count==512&&listing_truncated);select_row(511);assert(scroll_row==501);
    navigate("/home");choose("destination");int row=selected;
    handle_event({1,220,LIST_TOP+row*ROW+10,1,0,0});++frame;
    handle_event({1,220,LIST_TOP+row*ROW+10,1,0,0});assert(!std::strcmp(current_dir,"/home/destination"));
    navigate("/home");choose("demo.wiz");handle_event({3,150,26,0,0,0});
    std::vector<uint32_t> pixels(WIDTH*HEIGHT);surface={pixels.data(),WIDTH,HEIGHT};render();assert(presents==1);
    std::ofstream image(argv[2],std::ios::binary);image<<"P6\n"<<WIDTH<<" "<<HEIGHT<<"\n255\n";
    for(uint32_t p:pixels){char rgb[]={(char)(p>>16),(char)(p>>8),(char)p};image.write(rgb,3);}
    choose("alpha.bin");handle_event({1,WIDTH-15,HEIGHT-48,2,0,0});assert(!context_open);
    handle_event({1,250,LIST_TOP+selected*ROW+8,2,0,0});assert(context_open);render();
    handle_event({5,0,0,27,0,0});assert(!context_open);
    choose_open_with();render();dialog=NONE;
    for(auto size: {std::pair{600,360},std::pair{1200,720},std::pair{860,560}}){
        layout(size.first,size.second);pixels.assign(WIDTH*HEIGHT,0);surface={pixels.data(),WIDTH,HEIGHT};
        for(int i=4;i<BUTTON_COUNT;++i)assert(buttons[i].x>=SIDE&&buttons[i].x+buttons[i].w<=WIDTH-16);
        assert(VISIBLE>0&&LIST_TOP+VISIBLE*ROW<=HEIGHT-36);render();
        begin_dialog(NEW_FOLDER);render();dialog=NONE;
    }
    address_edit=true;edit_replace=true;native_copy(address,sizeof(address),"abcd");
    handle_event({5,0,0,0x1044,0,0});handle_event({5,0,0,0x1043,0,0});
    handle_event({5,0,0,'X',0,0});assert(!strcmp(address,"aXbcd"));
    handle_event({5,0,0,8,0,0});assert(!strcmp(address,"abcd"));
    handle_event({5,0,0,'a',1,0});handle_event({5,0,0,'Z',0,0});assert(!strcmp(address,"Z"));
    // Clicking the address bar and leaving it idle must not redraw Explorer
    // on each caret blink. The live loop uses the same render_frame function.
    window_id=1;
    int first_glyph=ui_font_glyph(ui_font_for_pixels(14),'a')->advance;
    address_edit=false;native_copy(address,sizeof(address),"abcd");render();
    auto repaint=[&](NativeEvent event,uint64_t x,uint64_t y,uint64_t w,uint64_t h){
        auto before=pixels;
        unsigned full=presents,partial=partial_presents;
        handle_event(event);render_frame();
        assert(presents==full&&partial_presents==partial+1);
        assert(damage_x==x&&damage_y==y&&damage_w==w&&damage_h==h);
        assert(pixels!=before);
        for(unsigned i=0;i<pixels.size();++i)
            if(i%WIDTH<x||i%WIDTH>=x+w||i/WIDTH<y||i/WIDTH>=y+h)assert(pixels[i]==before[i]);
    };
    uint64_t address_w=WIDTH-18-ADDRESS_X;
    uint64_t text_h=ui_font_for_pixels(14)->height;
    repaint({1,ADDRESS_TEXT_X+first_glyph,24,1,0,0},ADDRESS_X,ADDRESS_Y,address_w,ADDRESS_H);
    assert(address_edit&&address_input.position==1&&address_input.anchor==1);
    handle_event({5,0,0,'X',0,0});render_frame();assert(!strcmp(address,"aXbcd"));
    repaint({1,ADDRESS_TEXT_X,24,1,0,0},ADDRESS_TEXT_X,18,WIDTH-30-ADDRESS_TEXT_X,text_h);
    frame=100;repaint({1,220,24,1,0,0},ADDRESS_TEXT_X,18,WIDTH-30-ADDRESS_TEXT_X,text_h);
    assert(address_input.position==5);
    unsigned full_before=presents,partial_before=partial_presents;auto visible_pixels=pixels;
    for(frame=101;frame<=160;++frame)render_frame();
    assert(!dirty&&presents==full_before&&partial_presents==partial_before+2);
    assert(pixels==visible_pixels);
    assert(damage_w==1);
    auto edit_frame=[&](uint32_t key,uint32_t mods,uint64_t x,uint64_t y,uint64_t w){
        repaint({5,0,0,key,mods,0},x,y,w,text_h);
    };
    edit_frame('q',0,168,18,WIDTH-30-168);assert(!strcmp(address,"aXbcdq"));
    edit_frame(0x1044,2,168,18,WIDTH-30-168);
    handle_event({5,0,0,'c',1,0});assert(!strcmp(text_clipboard.data(),"q"));
    edit_frame('x',1,168,18,WIDTH-30-168);assert(!strcmp(address,"aXbcd"));
    full_before=presents;partial_before=partial_presents;
    handle_event({5,0,0,0x203a,0,0});render_frame();
    assert(presents==full_before&&partial_presents==partial_before);
    repaint({5,0,0,27,0,0},ADDRESS_X,ADDRESS_Y,address_w,ADDRESS_H);
    // Moving from a toolbar button and clicking the address bar in one frame
    // updates the former button, its status hint, and the newly focused field.
    handle_event({3,130,26,0,0,0});render_frame();
    full_before=presents;partial_before=partial_presents;
    handle_event({3,220,24,0,0,0});handle_event({1,220,24,1,0,0});render_frame();
    assert(presents==full_before&&partial_presents==partial_before+3);
    auto partial_pixels=pixels;render();assert(pixels==partial_pixels);
    repaint({5,0,0,27,0,0},ADDRESS_X,ADDRESS_Y,address_w,ADDRESS_H);
    full_before=presents;partial_before=partial_presents;
    for(;frame<221;++frame)render_frame();
    assert(presents==full_before&&partial_presents==partial_before);
    begin_dialog(NEW_FOLDER);render_frame();
    edit_frame('n',0,(WIDTH-480)/2+30,(HEIGHT-202)/2+65,420);assert(!strcmp(edit_text,"n"));
    repaint({1,(WIDTH-480)/2+DIALOG_TEXT_INSET,(HEIGHT-202)/2+68,1,0,0},
            (WIDTH-480)/2+DIALOG_TEXT_INSET,(HEIGHT-202)/2+65,420,text_h);
    edit_frame('i',0,(WIDTH-480)/2+30,(HEIGHT-202)/2+65,420);assert(!strcmp(edit_text,"in"));
    dialog=NONE;address_edit=false;
    put("/home/cancel.bin",std::string(200000,'C'));navigate("/home");choose("cancel.bin");
    copy_selection(false);navigate("destination");paste_selection();
    assert(transfer.active&&dialog==TRANSFER&&transfer.done==0);
    render();advance_transfer();assert(transfer.active&&transfer.done==65536);
    render();
    {
        std::ofstream popup(std::string(argv[2])+".transfer.ppm",std::ios::binary);
        popup<<"P6\n"<<WIDTH<<" "<<HEIGHT<<"\n255\n";
        for(uint32_t pixel:pixels){char rgb[]={(char)(pixel>>16),(char)(pixel>>8),(char)pixel};popup.write(rgb,3);}
    }
    handle_event({5,0,0,27,0,0});
    assert(!transfer.active&&!fs::exists(sandbox/"home/destination/cancel.bin"));
    assert(get("/home/cancel.bin")==std::string(200000,'C'));
    for(auto& item:fs::directory_iterator(sandbox/"home/destination"))assert(item.path().filename().string().find(".explorer-copy-")!=0);
    cancel_transfer();paste_selection();while(transfer.active)advance_transfer();
    assert(dialog==NONE&&transfer.success);
    assert(get("/home/destination/cancel.bin")==std::string(200000,'C'));
    put("/home/move.bin",std::string(90000,'M'));navigate("/home");choose("move.bin");
    copy_selection(true);navigate("destination");cross_volume_move=true;paste_selection();
    assert(transfer.active&&fs::exists(sandbox/"home/move.bin"));
    complete_transfer();cross_volume_move=false;
    assert(!fs::exists(sandbox/"home/move.bin")&&get("/home/destination/move.bin")==std::string(90000,'M'));
    put("/home/create-fail.txt","source");navigate("/home");choose("create-fail.txt");
    copy_selection(false);navigate("destination");create_failure=true;paste_selection();create_failure=false;
    assert(dialog==TRANSFER&&!transfer.active&&!transfer.success&&strstr(transfer.message,"Cannot create"));
    assert(!fs::exists(sandbox/"home/destination/create-fail.txt"));complete_transfer();
    fs::create_directories(sandbox/"mnt/Camera");navigate("/mnt");choose("Camera");
    assert(button_enabled(14));eject_result=-2;button_action(14);
    assert(ejected_path=="/mnt/Camera"&&strstr(status_line,"busy"));
    eject_result=0;button_action(14);assert(strstr(status_line,"You can remove"));
    navigate("/mnt/Camera");eject_result=1;button_action(14);
    assert(!strcmp(current_dir,"/mnt")&&strstr(status_line,"no explicit"));
    navigate("/home");choose("zeta.txt");
    fs::create_directories(sandbox/"mnt/TRAVEL");++mount_generation;frame+=31;poll_storage_changes();
    assert(drive_count==2&&selected>=0&&!strcmp(entries[selected].name,"zeta.txt"));
    navigate("/mnt");choose("TRAVEL");open_selected();assert(current_external&&!strcmp(current_dir,"/mnt/TRAVEL"));
    fs::remove(sandbox/"mnt/TRAVEL");++mount_generation;frame+=31;poll_storage_changes();
    assert(!strcmp(current_dir,"/mnt")&&drive_count==1&&!listing_error);
    fs::create_directories(sandbox/"mnt/BACKUP");++mount_generation;address_edit=true;
    frame+=31;poll_storage_changes();assert(drive_count==1&&address_edit);
    address_edit=false;frame+=31;poll_storage_changes();assert(drive_count==2);
    assert(icon_only(7)&&buttons[7].w==32&&!icon_only(14)&&!strcmp(buttons[14].label,"Eject"));
    handle_event({3,buttons[7].x+8,buttons[7].y+8,0,0,0});render_frame();
    assert(hovered_button==7);
    put("/mnt/Camera/sample.wiz","preview");navigate("/mnt/Camera");choose("sample.wiz");copy_selection(false);
    handle_event({3,buttons[9].x+8,buttons[9].y+8,0,0,0});render_frame();
    {
        std::ofstream snapshot(std::string(argv[2])+".drives.ppm",std::ios::binary);
        snapshot<<"P6\n"<<WIDTH<<" "<<HEIGHT<<"\n255\n";
        for(uint32_t pixel:pixels){char rgb[]={(char)(pixel>>16),(char)(pixel>>8),(char)pixel};snapshot.write(rgb,3);}
    }
    navigate("/mnt");choose("Camera");hovered_button=-1;render();
    assert(entries[selected].external_drive);
    {
        std::ofstream snapshot(std::string(argv[2])+".disks.ppm",std::ios::binary);
        snapshot<<"P6\n"<<WIDTH<<" "<<HEIGHT<<"\n255\n";
        for(uint32_t pixel:pixels){char rgb[]={(char)(pixel>>16),(char)(pixel>>8),(char)pixel};snapshot.write(rgb,3);}
    }
    int camera_row=-1;for(int i=0;i<drive_count;++i)if(!strcmp(drives[i].name,"Camera"))camera_row=i;
    assert(camera_row>=0&&camera_row<visible_drives());
    int disclosure_y=101+5*place_step+place_height()/2;
    handle_event({1,23,disclosure_y,1,0,0});assert(!devices_expanded&&visible_drives()==0);
    assert(!strcmp(current_dir,"/mnt"));
    ++mount_generation;frame+=31;poll_storage_changes();assert(!devices_expanded&&drive_count==2);
    handle_event({1,DRIVE_TEXT_X+5,drive_sidebar_top()+DRIVE_ROW/2,1,0,0});
    assert(!strcmp(current_dir,"/mnt"));render();
    {
        std::ofstream snapshot(std::string(argv[2])+".collapsed.ppm",std::ios::binary);
        snapshot<<"P6\n"<<WIDTH<<" "<<HEIGHT<<"\n255\n";
        for(uint32_t pixel:pixels){char rgb[]={(char)(pixel>>16),(char)(pixel>>8),(char)pixel};snapshot.write(rgb,3);}
    }
    handle_event({1,60,disclosure_y,1,0,0});assert(!devices_expanded&&!strcmp(current_dir,"/mnt"));
    handle_event({1,23,disclosure_y,1,0,0});assert(devices_expanded&&visible_drives()==2);
    handle_event({1,DRIVE_TEXT_X+5,drive_sidebar_top()+camera_row*DRIVE_ROW+DRIVE_ROW/2,1,0,0});
    assert(!strcmp(current_dir,"/mnt/Camera"));
    layout(600,360);pixels.assign(WIDTH*HEIGHT,0);surface={pixels.data(),WIDTH,HEIGHT};render();
    assert(drive_sidebar_top()+DRIVE_ROW<=HEIGHT-36);
    handle_event({1,DRIVE_TEXT_X+5,drive_sidebar_top()+DRIVE_ROW/2,1,0,0});
    assert(!strcmp(current_dir,"/mnt"));
    drive_count=1;layout_sidebar();assert(visible_drives()==1&&drive_sidebar_top()+DRIVE_ROW<=HEIGHT-36);
    auto previous_publication=published_pixels;
    publication_failure=true;dirty=true;render_frame();assert(dirty);
    assert(published_pixels==previous_publication);
    publication_failure=false;render_frame();assert(!dirty);
    assert(published_pixels==pixels);
    puts("Explorer navigation, sorting, dialogs, copy failure, moves, detached launch, limits and rendering passed");
}
