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

namespace fs=std::filesystem;
static fs::path sandbox;
static bool write_failure=false;
static unsigned spawned=0,presents=0;
static std::string resolve(uint64_t value) { return sandbox.string()+(const char*)value; }
static uint64_t native_call(uint64_t n,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e) {
    switch(n){
    case 0:return read((int)a,(void*)b,c);
    case 1:return write_failure?(uint64_t)-1:write((int)a,(void*)b,c);
    case 2:return open(resolve(a).c_str(),(int)b,0644);
    case 3:return close((int)a);
    case 4:{struct stat st{};if(stat(resolve(a).c_str(),&st))return -1;
        auto* info=(file_info*)b;*info={};info->size=st.st_size;info->type=S_ISDIR(st.st_mode)?2:1;return 0;}
    case 8:return lseek((int)a,(off_t)b,(int)c);
    case 39:return 42;
    case 40:{std::error_code error;std::vector<fs::directory_entry> items;
        for(auto it=fs::directory_iterator(resolve(a),error);!error&&it!=fs::directory_iterator();it.increment(error))items.push_back(*it);
        if(error)return -1;if(b>=items.size())return 0;
        auto& item=items[b];std::strcpy((char*)c,item.path().filename().c_str());
        *(uint64_t*)e=item.is_directory()?2:1;*(uint64_t*)d=item.is_directory()?0:item.file_size();return 1;}
    case 59:case 213:++spawned;return 100+spawned;
    case 74:return fsync((int)a);
    case 80:return fs::is_directory(resolve(a))?0:-1;
    case 82:{if(fs::exists(resolve(b)))return -1;std::error_code ec;fs::rename(resolve(a),resolve(b),ec);return ec?-1:0;}
    case 83:return mkdir(resolve(a).c_str(),0755);
    case 84:return rmdir(resolve(a).c_str());
    case 87:return unlink(resolve(a).c_str());
    case 138:{std::ofstream out(resolve(a),std::ios::binary);out.write((const char*)b,c);return out?0:-1;}
    case 201:++presents;return 0;
    default:return -1;
    }
}
static void put(const char* path,const std::string& text) { std::ofstream out(sandbox.string()+path,std::ios::binary);out<<text; }
static std::string get(const char* path) { std::ifstream in(sandbox.string()+path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{}); }
static void choose(const char* name) {
    for(int i=0;i<entry_count;++i)if(!std::strcmp(entries[i].name,name)){select_row(i);return;}
    assert(false&&"missing explorer entry");
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
    choose("large.txt");copy_selection(false);navigate("destination");paste_selection();
    assert(get("/home/destination/large.txt")==std::string(9001,'X'));paste_selection();
    assert(get("/home/destination/large.txt")==std::string(9001,'X'));
    navigate("/home");choose("empty.txt");copy_selection(false);navigate("destination");paste_selection();
    assert(fs::exists(sandbox/"home/destination/empty.txt"));
    navigate("/home");choose("zeta.txt");copy_selection(false);navigate("destination");write_failure=true;paste_selection();write_failure=false;
    assert(!fs::exists(sandbox/"home/destination/zeta.txt")&&get("/home/zeta.txt")=="hello explorer\n");
    for(auto& item:fs::directory_iterator(sandbox/"home/destination"))assert(item.path().filename().string().find(".explorer-copy-")!=0);
    navigate("/home");choose("renamed.txt");copy_selection(true);navigate("destination");paste_selection();
    assert(!fs::exists(sandbox/"home/renamed.txt")&&get("/home/destination/renamed.txt")=="do not overwrite");
    choose("renamed.txt");begin_dialog(DELETE_ITEM);assert(fs::exists(sandbox/"home/destination/renamed.txt"));commit_dialog();
    assert(!fs::exists(sandbox/"home/destination/renamed.txt"));
    navigate("/home");choose("destination");begin_dialog(DELETE_ITEM);commit_dialog();assert(dialog==DELETE_ITEM);dialog=NONE;
    choose("New folder");begin_dialog(DELETE_ITEM);commit_dialog();assert(!fs::exists(sandbox/"home/New folder"));
    choose("zeta.txt");open_selected();assert(dialog==PREVIEW&&!std::strcmp(dialog_text,"hello explorer\n"));dialog=NONE;
    choose("alpha.bin");open_selected();assert(dialog==PREVIEW&&std::strstr(dialog_text,"binary content"));dialog=NONE;
    put("/home/demo.wiz","fake executable");load_directory();choose("demo.wiz");open_selected();assert(spawned==1);
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
    puts("Explorer navigation, sorting, dialogs, copy failure, moves, detached launch, limits and rendering passed");
}
