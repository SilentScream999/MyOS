#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "../src/gui/resources.h"

static int live=0;
void* image_allocate(uint64_t size) { void* p=std::malloc(size); if(p) ++live; return p; }
void image_deallocate(void* p) { if(p) { --live; std::free(p); } }
static std::map<std::string,std::vector<uint8_t>> files;
static std::map<std::string,std::vector<std::pair<std::string,bool>>> directories;
static std::vector<std::string> reads;
static uint8_t* read_file(const char* path,uint64_t limit,uint64_t* size) {
    reads.emplace_back(path);
    auto it=files.find(path); if(it==files.end() || it->second.empty() || it->second.size()>limit) return nullptr;
    *size=it->second.size(); auto* out=(uint8_t*)image_allocate(*size);
    std::memcpy(out,it->second.data(),*size); return out;
}
static int list(const char* path,uint64_t index,char* name,bool* directory) {
    const auto& entries=directories[path]; if(index>=entries.size()) return 0;
    std::strcpy(name,entries[index].first.c_str()); *directory=entries[index].second; return 1;
}
static void text(const char* path,const char* value) { files[path]=std::vector<uint8_t>(value,value+std::strlen(value)); }
static std::vector<uint8_t> read(const std::string& path) {
    std::ifstream file(path,std::ios::binary); assert(file.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),{});
}
int main(int argc,char** argv) {
    assert(argc==3);
    resources::Store store{{read_file,list}};
    auto alpha=read(std::string(argv[1])+"/alpha.png"),blue=read(std::string(argv[1])+"/blue.png");
    resources::App app{};
    const char* good="name = Picture App\r\nexec=/apps/pictures/main.wiz\nicon=pictures\nfuture=okay\n";
    assert(resources::manifest((const uint8_t*)good,std::strlen(good),"pictures",&app));
    assert(resources::equal(app.name,"Picture App"));
    for(const char* bad:{"name=x\nexec=relative", "name=x\nexec=/bin/a\nname=duplicate",
                         "exec=/bin/a", "name=x\nexec=/bin/a\nicon=../bad",
                         "name=x\nexec=/bin/a\nicon=", "broken"}) {
        auto previous=app;
        assert(!resources::manifest((const uint8_t*)bad,std::strlen(bad),"pictures",&app));
        assert(std::memcmp(&previous,&app,sizeof(app))==0);
    }
    assert(!resources::manifest((const uint8_t*)good,std::strlen(good),"..",&app));
    text("/etc/icons.cfg","custom\n");
    files["/share/icons/hicolor/32x32/apps/pictures.png"]=alpha;
    // A theme's larger supplied image wins over a fallback theme's exact size.
    files["/share/icons/custom/48x48/apps/pictures.png"]=blue;
    files["/share/icons/hicolor/32x32/apps/application-default.png"]=alpha;
    const Image* found=store.lookup("custom","pictures",32); assert(found && found->pixels[0]==0xff0000ff);
    Image scaled{}; assert(store.icon("custom","pictures",32,&scaled));
    assert(scaled.width==32 && scaled.height==16); image_free(&scaled);
    auto decodes=store.decodes;
    assert(store.lookup("custom","pictures",32)); assert(store.decodes==decodes && store.hits>0);
    // Same path, same dimensions, changed bytes: update without a reboot/build.
    files["/share/icons/custom/48x48/apps/pictures.png"]=alpha;
    found=store.lookup("custom","pictures",32); assert(found && found->pixels[0]==0x80800000);
    assert(store.decodes==decodes+1);
    files["/share/icons/custom/48x48/apps/pictures.png"]={0,1,2};
    assert(store.icon("custom","pictures",32,&scaled)); assert(scaled.pixels[0]==0x80800000); image_free(&scaled);
    files["/home/.local/share/icons/custom/32x32/apps/pictures.png"]=blue;
    found=store.lookup("custom","pictures",32); assert(found && found->pixels[0]==0xff0000ff);
    files["/apps/pictures/icon.png"]=alpha;
    assert(store.icon("custom","/apps/pictures/icon.png",24,&scaled)); image_free(&scaled);
    files["/apps/pictures/fake.png"]=read(argv[2]);
    assert(!store.load("/apps/pictures/fake.png")); // JPEG is supported for images, PNG is the icon contract.

    directories["/apps"]={{"pictures",true},{"invalid",true},{"..",true}};
    text("/apps/pictures/app.manifest",good);
    text("/apps/invalid/app.manifest","name=bad\nexec=relative");
    directories["/bin"]={{"init.elf",false},{"counter.wiz",false},{"README",false},{"folder.elf",true}};
    reads.clear(); decodes=store.decodes;
    auto* catalog=resources::build(store,24,16,24);
    assert(catalog && catalog->count==2);
    assert(resources::equal(catalog->apps[0].app.id,"pictures"));
    assert(!catalog->apps[0].icons.menu.pixels && !catalog->apps[1].icons.menu.pixels);
    assert(!catalog->apps[0].icons.title.pixels && !catalog->apps[0].icons.task.pixels);
    assert(store.decodes==decodes);
    for(const auto& path:reads) assert(path.find(".png")==std::string::npos);
    assert(resources::equal(catalog->apps[1].app.exec,"/bin/counter.wiz"));
    resources::Demand demand{}; demand.menu_rows=1; demand.system[0]=true;
    auto* shown=resources::build(store,24,16,24,demand,catalog);
    assert(shown && shown->apps[0].icons.menu.pixels && !shown->apps[1].icons.menu.pixels);
    assert(!shown->apps[0].icons.title.pixels && !shown->apps[0].icons.task.pixels);
    assert(shown->system[0].title.pixels && shown->system[0].task.pixels && !shown->system[0].menu.pixels);
    // Changing visibility alone reuses metadata/pixels without disk reads.
    reads.clear(); decodes=store.decodes;
    demand.menu_rows=0;
    auto* hidden=resources::build(store,24,16,24,demand,shown);
    assert(hidden && !hidden->apps[0].icons.menu.pixels);
    assert(hidden->system[0].title.pixels && reads.empty() && store.decodes==decodes);
    resources::release(hidden); resources::release(shown);
    demand={}; demand.window_count=1;
    resources::copy(demand.executables[0],256,"/bin/counter.wiz");
    auto* running=resources::build(store,24,16,24,demand,catalog);
    assert(running && running->apps[1].icons.title.pixels && running->apps[1].icons.task.pixels);
    assert(!running->apps[1].icons.menu.pixels && !running->apps[0].icons.title.pixels);
    resources::release(running);
    resources::release(catalog);
    // Manifest registration takes precedence over the compatibility bin scan.
    text("/apps/pictures/app.manifest","name=Counter\nexec=/bin/counter.wiz\nicon=pictures\n");
    catalog=resources::build(store,24,16,24); assert(catalog && catalog->count==1); resources::release(catalog);
    files.erase("/apps/pictures/app.manifest");
    catalog=resources::build(store,24,16,24); assert(catalog && catalog->count==1);
    assert(resources::equal(catalog->apps[0].app.id,"counter")); resources::release(catalog);
    for(int i=0;i<80;++i) {
        auto path="/home/icon-"+std::to_string(i)+".png";
        files[path]=blue; assert(store.load(path.c_str())); assert(store.bytes<=store.capacity);
    }
    text("/etc/icons.cfg","../../bad\n");
    demand={}; demand.system[0]=true;
    catalog=resources::build(store,24,16,24,demand); assert(catalog && catalog->system[0].title.pixels); resources::release(catalog);
    store.clear(); assert(live==0);
}
