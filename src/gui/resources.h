#ifndef WYZAR_RESOURCES_H
#define WYZAR_RESOURCES_H
#include "image.h"
#include "../core/app.h"

// Runtime desktop resources. Only the resource worker calls these functions;
// the compositor consumes immutable Catalog snapshots containing cached pixels.
namespace resources {
constexpr unsigned max_apps = 16, max_cache = 32;
struct IO {
    // Returned bytes belong to image_allocate/image_deallocate. No implicit NUL.
    uint8_t* (*read)(const char*, uint64_t limit, uint64_t* size);
    int (*list)(const char*, uint64_t index, char* name, bool* directory);
};
static unsigned length(const char* s) { unsigned n=0; while (s && s[n]) ++n; return n; }
static bool equal(const char* a, const char* b) {
    if (!a || !b) return a == b;
    while (*a && *a==*b) { ++a; ++b; } return *a==*b;
}
static bool copy(char* out, unsigned capacity, const char* text) {
    unsigned n=length(text); if (!text || n>=capacity) return false;
    for (unsigned i=0;i<=n;++i) out[i]=text[i]; return true;
}
static bool append(char* out, unsigned capacity, const char* text) {
    unsigned n=length(out); return n<capacity && copy(out+n,capacity-n,text);
}
static bool identifier(const char* s) {
    if (!s || !*s || equal(s,".") || equal(s,"..")) return false;
    for (;*s;++s) if (!((*s>='a'&&*s<='z') || (*s>='A'&&*s<='Z') ||
                       (*s>='0'&&*s<='9') || *s=='-' || *s=='_' || *s=='.')) return false;
    return true;
}
struct App { char id[96], name[64], exec[124], icon[256]; };
static bool manifest(const uint8_t* data, uint64_t size, const char* id, App* out) {
    if (!data || !size || size>4096 || !identifier(id) || !out) return false;
    App app{}; if (!copy(app.id,sizeof(app.id),id) || !copy(app.icon,sizeof(app.icon),id)) return false;
    unsigned seen=0; uint64_t at=0;
    while (at<size) {
        char line[512]; unsigned used=0;
        while (at<size && data[at]!='\n') {
            uint8_t c=data[at++];
            if ((c<32 && c!='\r' && c!='\t') || c>=127 || used+1>=sizeof(line)) return false;
            line[used++]=(char)c;
        }
        if (at<size) ++at;
        while (used && (line[used-1]=='\r'||line[used-1]==' '||line[used-1]=='\t')) --used;
        line[used]=0;
        char* key=line; while (*key==' '||*key=='\t') ++key;
        if (!*key || *key=='#') continue;
        char* value=key; while (*value && *value!='=') ++value;
        if (!*value) return false;
        *value++=0;
        char* end=value-1; while (end>key && (end[-1]==' '||end[-1]=='\t')) *--end=0;
        while (*value==' '||*value=='\t') ++value;
        unsigned bit=0; char* target=nullptr; unsigned capacity=0;
        if (equal(key,"name")) { bit=1; target=app.name; capacity=sizeof(app.name); }
        else if (equal(key,"exec")) { bit=2; target=app.exec; capacity=sizeof(app.exec); }
        else if (equal(key,"icon")) { bit=4; target=app.icon; capacity=sizeof(app.icon); }
        else continue; // Future metadata does not invalidate an installed app.
        if ((seen&bit) || !*value || !copy(target,capacity,value)) return false;
        seen|=bit;
    }
    if ((seen&3)!=3 || app.exec[0]!='/' ||
        (app.icon[0]!='/' && !identifier(app.icon))) return false;
    *out=app; return true;
}
struct CacheEntry { char path[256]; uint64_t hash, stamp; Image source; };
struct Store {
    IO io; CacheEntry entries[max_cache]; uint64_t bytes, clock, decodes, hits;
    static constexpr uint64_t capacity=8*1024*1024;
    void clear() {
        for (auto& entry:entries) { image_free(&entry.source); entry={}; } bytes=0;
    }
    const Image* load(const char* path) {
        uint64_t size=0;
        uint8_t* data=io.read(path,4*1024*1024,&size);
        if (!data) return nullptr;
        const uint8_t signature[]={137,80,78,71,13,10,26,10};
        bool png=size>=8;
        for (unsigned i=0;i<8 && png;++i) if (data[i]!=signature[i]) png=false;
        if (!png) { image_deallocate(data); return nullptr; }
        uint64_t hash=14695981039346656037ull;
        for (uint64_t i=0;i<size;++i) { hash^=data[i]; hash*=1099511628211ull; }
        CacheEntry* slot=nullptr;
        for (auto& entry:entries) if (equal(entry.path,path)) {
            slot=&entry;
            if (entry.hash==hash) { image_deallocate(data); entry.stamp=++clock; ++hits; return &entry.source; }
            break;
        }
        Image decoded{};
        bool ok=image_decode(data,size,&decoded,1024*1024);
        image_deallocate(data);
        if (!ok) return nullptr;
        ++decodes;
        uint64_t needed=(uint64_t)decoded.width*decoded.height*4;
        if (slot) { bytes-=(uint64_t)slot->source.width*slot->source.height*4; image_free(&slot->source); *slot={}; }
        for (;;) {
            CacheEntry* oldest=nullptr;
            for (auto& entry:entries) {
                if (!entry.source.pixels) { if (!slot) slot=&entry; }
                else if (!oldest || entry.stamp<oldest->stamp) oldest=&entry;
            }
            if (slot && bytes+needed<=capacity) break;
            if (!oldest) { image_free(&decoded); return nullptr; }
            bytes-=(uint64_t)oldest->source.width*oldest->source.height*4;
            image_free(&oldest->source); *oldest={}; if (!slot) slot=oldest;
        }
        if (!copy(slot->path,sizeof(slot->path),path)) { image_free(&decoded); return nullptr; }
        slot->hash=hash; slot->stamp=++clock; slot->source=decoded; bytes+=needed;
        return &slot->source;
    }
    const Image* lookup(const char* theme, const char* name, uint32_t requested) {
        if (name[0]=='/') return load(name);
        if (!identifier(name) || !identifier(theme)) return nullptr;
        const unsigned sizes[]={16,24,32,48,64,128,256,512};
        // Prefer exact, then the smallest larger asset, then the largest smaller.
        for (unsigned root=0;root<2;++root) {
            for (unsigned pass=0;pass<2;++pass) for (unsigned i=0;i<8;++i) {
                unsigned index=pass ? 7-i : i, size=sizes[index];
                if ((!pass && size<requested) || (pass && size>=requested)) continue;
                char number[12]; unsigned n=0,v=size;
                do { number[n++]=(char)('0'+v%10); v/=10; } while(v);
                for (unsigned a=0;a<n/2;++a) { char t=number[a];number[a]=number[n-1-a];number[n-1-a]=t; } number[n]=0;
                char path[256]; copy(path,sizeof(path),root ? "/share/icons/" : "/home/.local/share/icons/");
                if (!append(path,sizeof(path),theme) || !append(path,sizeof(path),"/") ||
                    !append(path,sizeof(path),number) || !append(path,sizeof(path),"x") ||
                    !append(path,sizeof(path),number) || !append(path,sizeof(path),"/apps/") ||
                    !append(path,sizeof(path),name) || !append(path,sizeof(path),".png")) continue;
                if (const Image* image=load(path)) return image;
            }
            char path[256]; copy(path,sizeof(path),root ? "/share/icons/" : "/home/.local/share/icons/");
            if (append(path,sizeof(path),theme) && append(path,sizeof(path),"/") &&
                append(path,sizeof(path),name) && append(path,sizeof(path),".png"))
                if (const Image* image=load(path)) return image;
        }
        return nullptr;
    }
    bool icon(const char* theme, const char* name, uint32_t size, Image* out) {
        const Image* source=lookup(theme,name,size);
        if (!source && !equal(theme,"hicolor") && name[0]!='/') source=lookup("hicolor",name,size);
        if (!source) source=lookup(theme,"application-default",size);
        if (!source && !equal(theme,"hicolor")) source=lookup("hicolor","application-default",size);
        if (!source) return false;
        uint32_t w=size,h=size;
        if (source->width>source->height) h=(uint64_t)size*source->height/source->width;
        else w=(uint64_t)size*source->width/source->height;
        return image_resize(*source,w?w:1,h?h:1,out);
    }
};
struct Icons { Image menu, title, task; };
struct Entry { App app; Icons icons; };
struct Catalog { unsigned count; Entry apps[max_apps]; Icons system[5]; char theme[64]; };
struct Demand {
    unsigned menu_rows, window_count;
    char executables[32][256];
    bool system[5];
    bool has_window(const char* executable) const {
        for (unsigned i=0;i<window_count && i<32;++i)
            if (equal(executables[i],executable)) return true;
        return false;
    }
};
static void release_icons(Icons& icons) { image_free(&icons.menu); image_free(&icons.title); image_free(&icons.task); }
static void release(Catalog* catalog) {
    if (!catalog) return;
    for (auto& entry:catalog->apps) release_icons(entry.icons);
    for (auto& icons:catalog->system) release_icons(icons);
    image_deallocate(catalog);
}
static void load_icons(Store& store, const char* theme, const char* name, Icons& out,
                       uint32_t menu, uint32_t title, uint32_t task,
                       bool menu_visible, bool window_visible, const Icons* cached = nullptr) {
    auto variant=[&](uint32_t size, Image& out, const Image* previous) {
        if (previous && previous->pixels) image_resize(*previous,previous->width,previous->height,&out);
        else store.icon(theme,name,size,&out);
    };
    if (menu_visible) variant(menu,out.menu,cached ? &cached->menu : nullptr);
    if (window_visible) {
        variant(title,out.title,cached ? &cached->title : nullptr);
        variant(task,out.task,cached ? &cached->task : nullptr);
    }
}
// App discovery is data-driven. /apps manifests take precedence over /bin
// compatibility entries with the same executable path.
static Catalog* build(Store& store, uint32_t menu, uint32_t title, uint32_t task,
                       const Demand& demand = {}, const Catalog* known = nullptr) {
    auto* catalog=(Catalog*)image_allocate(sizeof(Catalog)); if (!catalog) return nullptr;
    *catalog={};
    char* theme=catalog->theme; copy(theme,sizeof(catalog->theme),"hicolor");
    if (known) {
        copy(theme,sizeof(catalog->theme),known->theme);
        catalog->count=known->count;
        for (unsigned i=0;i<known->count;++i) catalog->apps[i].app=known->apps[i].app;
    } else {
        uint64_t size=0; uint8_t* data=store.io.read("/etc/icons.cfg",sizeof(catalog->theme)-1,&size);
        if (data) {
            while (size && (data[size-1]=='\n'||data[size-1]=='\r')) --size;
            char candidate[64]; unsigned i=0; for (;i<size;++i) candidate[i]=data[i]; candidate[i]=0;
            if (length(candidate)==size && identifier(candidate)) copy(theme,sizeof(catalog->theme),candidate);
            image_deallocate(data);
        }
        char name[256]; bool directory=false;
        for (unsigned i=0;i<4096 && catalog->count<max_apps && store.io.list("/apps",i,name,&directory)>0;++i) {
            if (!directory || !identifier(name) || length(name)>=96) continue;
            char path[256]; copy(path,sizeof(path),"/apps/");
            if (!append(path,sizeof(path),name) || !append(path,sizeof(path),"/app.manifest")) continue;
            data=store.io.read(path,4096,&size); if (!data) continue;
            App app{}; bool ok=manifest(data,size,name,&app); image_deallocate(data);
            if (!ok) continue;
            bool duplicate=false; for (unsigned k=0;k<catalog->count;++k)
                if (equal(catalog->apps[k].app.exec,app.exec)) duplicate=true;
            if (!duplicate) catalog->apps[catalog->count++].app=app;
        }
        for (unsigned i=0;i<4096 && catalog->count<max_apps && store.io.list("/bin",i,name,&directory)>0;++i) {
            if (directory || !app_name_supported(name) || app_name_is_shell(name)) continue;
            App app{}; copy(app.exec,sizeof(app.exec),"/bin/");
            if (!append(app.exec,sizeof(app.exec),name)) continue;
            bool duplicate=false; for (unsigned k=0;k<catalog->count;++k)
                if (equal(catalog->apps[k].app.exec,app.exec)) duplicate=true;
            if (duplicate) continue;
            unsigned n=length(name)-4; if (n>=sizeof(app.id)) continue;
            for (unsigned k=0;k<n;++k) app.id[k]=name[k]; app.id[n]=0;
            unsigned display=n<sizeof(app.name)-1?n:sizeof(app.name)-1;
            for (unsigned k=0;k<display;++k) app.name[k]=app.id[k]; app.name[display]=0;
            copy(app.icon,sizeof(app.icon),app.id);
            catalog->apps[catalog->count++].app=app;
        }
    }
    for (unsigned i=0;i<catalog->count;++i)
        load_icons(store,theme,catalog->apps[i].app.icon,catalog->apps[i].icons,menu,title,task,
                    i<demand.menu_rows,demand.has_window(catalog->apps[i].app.exec),
                    known ? &known->apps[i].icons : nullptr);
    const char* system[]={"terminal","visual-test","rainbow","boot-log","application-default"};
    for (unsigned i=0;i<5;++i)
        load_icons(store,theme,system[i],catalog->system[i],menu,title,task,false,demand.system[i],
                    known ? &known->system[i] : nullptr);
    return catalog;
}
}
#endif
