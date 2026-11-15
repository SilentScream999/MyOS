#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <fstream>
#include <set>
#define NATIVE_HOST_TEST
#include "../user/notepad.cpp"
#include "../src/core/text_clipboard.h"
static TextClipboard text_clipboard;
static std::map<std::string,std::string> files;
static std::string reading;
static unsigned offset=0;
static bool fail_save=false,fail_read=false,fail_role=false;
static std::set<std::string> directories={"/","/home","/home/Applications","/home/projects","/mnt"};
static std::map<uint64_t,std::vector<uint32_t>> windows;
static std::map<uint64_t,uint64_t> window_roles;
static uint64_t next_window=100;
static uint64_t native_call(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e){
    switch(n){
    case 2:if(!files.count((char*)a))return -1;reading=(char*)a;offset=0;return 10;
    case 3:return 0;
    case 0:{if(fail_read)return -1;auto& data=files[reading];unsigned count=(unsigned)c;
        if(count>7)count=7;if(count>data.size()-offset)count=(unsigned)data.size()-offset;
        memcpy((void*)b,data.data()+offset,count);offset+=count;return count;}
    case 4:*(file_info*)b={};if(directories.count((char*)a)){((file_info*)b)->type=2;return 0;}
        if(!files.count((char*)a))return -1;((file_info*)b)->type=1;((file_info*)b)->size=files[(char*)a].size();return 0;
    case 40:{std::vector<std::pair<std::string,unsigned>> listing;std::string prefix=(char*)a;if(!directories.count(prefix))return -1;
        if(prefix!="/")prefix+="/";
        for(const auto& dir:directories)if(dir.starts_with(prefix)&&dir.size()>prefix.size()&&dir.find('/',prefix.size())==std::string::npos)listing.push_back({dir.substr(prefix.size()),2});
        for(const auto& [name,value]:files)if(name.starts_with(prefix)&&name.find('/',prefix.size())==std::string::npos)listing.push_back({name.substr(prefix.size()),1});
        if(b>=listing.size())return 0;strcpy((char*)c,listing[b].first.c_str());*(uint64_t*)d=0;*(uint64_t*)e=listing[b].second;return 1;}
    case 138:if(fail_save)return -1;files[(char*)a]=std::string((char*)b,c);return 0;
    case 201:return 0;
    case 200:windows[++next_window].resize(d*e);return next_window;
    case 202:windows.erase(a);window_roles.erase(a);return 0;
    case 203:case 206:return (uint64_t)windows[a].data();
    case 209:if(fail_role||!windows.count(a)||b>1)return -1;window_roles[a]=b;return 0;
    case 217:return a==0?text_clipboard.set((char*)b,c):a==1?text_clipboard.get((char*)b,c):-1;
    default:return -1;
    }
}
static void image(const std::string& filename,const NativeSurface& canvas){
    std::ofstream out(filename,std::ios::binary);out<<"P6\n"<<canvas.w<<" "<<canvas.h<<"\n255\n";
    for(int i=0;i<canvas.w*canvas.h;++i){uint32_t p=canvas.pixels[i];char rgb[]={(char)(p>>16),(char)(p>>8),(char)p};out.write(rgb,3);}
}
int main(int argc,char** argv){
    std::vector<uint32_t> pixels(760*520);surface={pixels.data(),760,520};
    assert(text_clipboard.set("old",3)==0);char tiny[2]={'X','Y'};
    assert(text_clipboard.get(tiny,2)==-1&&tiny[0]=='X'&&tiny[1]=='Y');
    assert(text_clipboard.set("bad\0value",9)==-1&&!strcmp(text_clipboard.bytes,"old"));
    assert(text_clipboard.set("x",TextClipboard::capacity+1)==-1&&!strcmp(text_clipboard.bytes,"old"));
    files["/home/notes.txt"]="one\ntwo\n";assert(load("/home/notes.txt"));assert(!modified&&length==8);
    caret=anchor=3;insert("!",1);assert(!strcmp(text,"one!\ntwo\n"));
    anchor=0;caret=4;insert("A",1);assert(!strcmp(text,"A\ntwo\n")); // overlapping shrink
    anchor=0;caret=1;insert("longer",6);assert(!strcmp(text,"longer\ntwo\n")); // overlapping grow
    key_event('z',1);assert(!strcmp(text,"A\ntwo\n"));
    fail_save=true;assert(!save_to(path));assert(modified&&files[path]=="one\ntwo\n");fail_save=false;
    assert(save_to(path));assert(!modified&&files[path]=="A\ntwo\n");
    files["/home/binary"]=std::string("a\0b",3);assert(!load("/home/binary"));assert(!strcmp(text,"A\ntwo\n"));
    files["/home/unicode"]="caf\xc3\xa9";assert(!load("/home/unicode"));
    files["/home/huge"]=std::string(CAP+1,'x');assert(!load("/home/huge"));assert(!load("relative.txt"));
    fail_read=true;assert(!load("/home/notes.txt"));fail_read=false;assert(!strcmp(text,"A\ntwo\n"));
    key_event('a',1);key_event('c',1);key_event('x',1);assert(length==0);key_event('v',1);assert(!strcmp(text,"A\ntwo\n"));
    request(NEW_DOCUMENT);assert(modal==UNSAVED);key_event(27,0);assert(modal==NORMAL&&modified);
    request(NEW_DOCUMENT);save();assert(length==0&&!modified&&path[0]==0);
    insert("new notes",9);request(CLOSE_DOCUMENT);assert(modal==UNSAVED&&!exiting);
    save();assert(modal==SAVE_PATH);native_copy(field,sizeof(field),"/home/notes.txt");submit_path();assert(modal==OVERWRITE);
    fail_save=true;key_event('\n',0);assert(!exiting&&modified&&modal==OVERWRITE);fail_save=false;
    key_event(27,0);assert(!exiting&&modified);request(CLOSE_DOCUMENT);save();native_copy(field,sizeof(field),"/home/new.txt");submit_path();
    assert(exiting&&!modified&&files["/home/new.txt"]=="new notes");exiting=false;
    request(NEW_DOCUMENT);std::string full(CAP,'x');insert(full.data(),CAP);insert("!",1);assert(length==CAP);
    key_event('a',1);insert("",0);assert(length==0);key_event('z',1);assert(length==CAP);
    request(NEW_DOCUMENT);perform_pending();insert("first\nsecond\nthird",18);caret=anchor=2;
    key_event(0x1042,0);assert(caret==8);key_event(0x1041,0);assert(caret==2);
    key_event(0x1100,0);assert(caret==0);key_event(0x1101,0);assert(caret==5);
    key_event(0x1102,0);assert(!strcmp(text,"firstsecond\nthird"));
    key_event(0x1101,1);assert(caret==length);key_event(0x1100,1);assert(caret==0);
    unsigned before=length;key_event(0x203a,0);assert(length==before); // Function keys are not text.
    // Compare arbitrary overlapping replacements against the standard string implementation.
    std::string model(text,length);unsigned random=12345;
    for(unsigned round=0;round<2000;++round){auto next=[&](){random=random*1664525u+1013904223u;return random;};
        caret=next()%(length+1);anchor=next()%(length+1);unsigned first=low(),count=high()-low();
        std::string replacement(next()%25,(char)('a'+next()%26));
        model.replace(first,count,replacement);insert(replacement.data(),(unsigned)replacement.size());
        assert(model==std::string(text,length));assert(caret<=length&&anchor<=length);}
    request(NEW_DOCUMENT);perform_pending();const char* sample="Notes\n\nWelcome to Notepad.\nYour text files can now be edited in WyzarOS.\n\nCtrl+S saves. Ctrl+O opens a file.";
    insert(sample,native_length(sample));
    // Menus use the same commands as shortcuts and suppress editing while open.
    handle_event({1,20,15,1,0,0});assert(menu==0);unsigned size_before=length;
    key_event('Q',0);assert(length==size_before);key_event(0x1043,0);assert(menu==1);
    key_event(27,0);assert(menu==-1);key_event('e',4);assert(menu==1);
    menu_item=5;key_event('\n',0);assert(low()==0&&high()==length);
    key_event('v',4);key_event('\n',0);assert(!show_status&&menu==-1);show_status=true;
    key_event('h',4);key_event('\n',0);assert(modal==ABOUT);key_event(27,0);assert(modal==NORMAL);
    // The common picker browses real listings, retains folders under filtering,
    // selects existing files, and returns a path for a new save name.
    files["/home/projects/draft.txt"]="draft";files["/home/projects/notes.md"]="notes";files["/home/.hidden.txt"]="hidden";
    NativeFileDialog browser;
    fail_role=true;assert(!browser.show(false,"/home/")&&!browser.window&&windows.empty());fail_role=false;
    assert(browser.show(false,"/home/"));assert(window_roles[browser.window]==1);assert(browser.entries[0].type==2);
    auto choose=[&](const char* name){for(int i=0;i<browser.count;++i)if(!strcmp(browser.entries[i].name,name)){browser.select(i);browser.focus=NativeFileDialog::LIST;return;}assert(false);};
    choose("projects");int project_row=browser.selected;browser.last_click=-1;
    assert(browser.event({1,200,106+project_row*30,1,0,0},1)==NativeFileDialog::WAIT);
    assert(browser.event({1,200,106+project_row*30,1,0,0},2)==NativeFileDialog::WAIT&&!strcmp(browser.directory,"/home/projects"));
    assert(!browser.navigate("missing")&&!strcmp(browser.directory,"/home/projects"));
    browser.text_only=true;browser.navigate(".");assert(browser.count==1&&!strcmp(browser.entries[0].name,"draft.txt"));
    choose("draft.txt");assert(browser.submit()==NativeFileDialog::ACCEPT&&!strcmp(browser.result,"/home/projects/draft.txt"));
    browser.event({5,0,0,'l',1,0},1);for(const char* p="../";*p;++p)browser.event({5,0,0,(uint32_t)*p,0,0},1);
    browser.event({5,0,0,'\n',0,0},1);assert(!strcmp(browser.directory,"/home"));
    browser.focus=NativeFileDialog::NAME;native_copy(browser.filename,256,"missing.txt");assert(browser.submit()==NativeFileDialog::WAIT);
    browser.saving=true;assert(browser.submit()==NativeFileDialog::ACCEPT&&!strcmp(browser.result,"/home/missing.txt"));
    char resolved[256];assert(NativeFileDialog::absolute("/home/projects","../../mnt/./",resolved)&&!strcmp(resolved,"/mnt"));
    assert(!NativeFileDialog::absolute("/",std::string(256,'x').c_str(),resolved));browser.hide();
    assert(text_clipboard.set("/home/projects/draft.txt",native_length("/home/projects/draft.txt"))==0);
    assert(browser.show(false,"/home/"));browser.event({5,0,0,'v',1,0},3);
    assert(!strcmp(browser.filename,"/home/projects/draft.txt")&&browser.submit()==NativeFileDialog::ACCEPT);browser.hide();
    assert(!windows.size());
    // Accepting a browser selection loads the file; Cancel preserves edits.
    modified=false;request(OPEN_DOCUMENT);assert(picker.window&&modal==OPEN_PATH);
    assert(picker.navigate("/home/projects"));picker.focus=NativeFileDialog::NAME;native_copy(picker.filename,256,"draft.txt");
    picker_event({5,0,0,'\n',0,0});assert(modal==NORMAL&&!picker.window&&!strcmp(text,"draft"));
    insert("!",1);save(true);assert(picker.window&&modal==SAVE_PATH);picker_event({7,0,0,0,0,0});assert(modified&&modal==NORMAL&&!picker.window);
    save(true);picker.focus=NativeFileDialog::NAME;native_copy(picker.filename,256,"new draft.txt");fail_save=true;
    picker_event({5,0,0,'\n',0,0});assert(modified&&picker.window&&modal==SAVE_PATH&&!files.count("/home/projects/new draft.txt"));
    fail_save=false;picker_event({5,0,0,'\n',0,0});assert(!modified&&!picker.window&&files["/home/projects/new draft.txt"]==text);
    request(OPEN_DOCUMENT);picker.focus=NativeFileDialog::NAME;native_copy(picker.filename,256,"draft.txt");fail_read=true;
    picker_event({5,0,0,'\n',0,0});assert(picker.window&&modal==OPEN_PATH);fail_read=false;picker_event({7,0,0,0,0,0});
    request(NEW_DOCUMENT);perform_pending();insert(sample,native_length(sample));
    // A path copied by another app replaces the current selection as text.
    assert(native_clipboard_set("/home/name with spaces.txt",native_length("/home/name with spaces.txt")));caret=anchor=length;
    key_event('v',1);assert(std::string(text,length).ends_with("/home/name with spaces.txt"));
    for(auto size: {std::pair{480,300},std::pair{760,520},std::pair{1200,720}}){
        pixels.assign(size.first*size.second,0);surface={pixels.data(),size.first,size.second};
        for(int i=NORMAL;i<=ABOUT;++i){modal=(Modal)i;render();}modal=NORMAL;
        for(int i=0;i<4;++i){menu=i;menu_item=0;render();}menu=-1;}
    assert(browser.show(false,"/home/"));assert(window_roles[browser.window]==1);
    for(auto size: {std::pair{560,360},std::pair{1000,680}}){std::vector<uint32_t> dialog_pixels(size.first*size.second);
        browser.canvas={dialog_pixels.data(),size.first,size.second};browser.bound_scroll();browser.render();}
    browser.hide();
    pixels.assign(760*520,0);surface={pixels.data(),760,520};render();
    if(argc>1){image(argv[1],surface);menu=0;render();image(std::string(argv[1])+"-file-menu.ppm",surface);menu=-1;
        assert(browser.show(false,"/home/projects/"));browser.render();image(std::string(argv[1])+"-open.ppm",browser.canvas);browser.hide();
        assert(browser.show(true,"/home/Untitled.txt"));browser.render();image(std::string(argv[1])+"-save-as.ppm",browser.canvas);browser.hide();}
    puts("Notepad editing, selection, undo, read rejection, atomic-save failure, prompts, limits and rendering passed");
}
