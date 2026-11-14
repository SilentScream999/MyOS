#ifndef WYZAR_RESOURCES_H
#define WYZAR_RESOURCES_H
#include "image.h"
#include "../core/app.h"
#include "../core/wiz.h"

// Runtime desktop resources. Only the resource worker calls these functions;
// the compositor consumes immutable Catalog snapshots containing cached pixels.
namespace resources {
constexpr unsigned max_apps = 16, max_cache = 32;
struct IO {
    // Returned bytes belong to image_allocate/image_deallocate. No implicit NUL.
    uint8_t* (*read)(const char*, uint64_t limit, uint64_t* size);
    int (*list)(const char*, uint64_t index, char* name, bool* directory);
    // Optional ranged read for .wiz packages: copy `len` bytes at `offset`
    // into buf and store the file size in *total. len==0 only reports the
    // size (buf may be null). Returns false if the file cannot be read.
    // Without it, packages are treated as ordinary executables.
    bool (*read_at)(const char*, uint64_t offset, void* buf, uint64_t len, uint64_t* total) = nullptr;
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
// One launchable application. `exec` is the stable identity used to match
// windows. Packaged (.wiz) apps also carry metadata read from inside the file.
struct App {
    char id[96], name[64], exec[256], icon[256];
    char version[32], description[128], pkgid[96], kind[16];
    unsigned icon_mask = 0;      // bit i: package ships icons/<wiz_icon_sizes[i]>.png
    bool icon_set = false;       // registration named an icon explicitly
    bool packaged = false;       // exec is a valid .wiz with an embedded manifest
    bool available = true;       // false for a pinned app whose file is missing
};
// Parse a registration manifest (/apps/<id>/app.manifest). Strictly, `name`
// and an absolute `exec` are required. With `inherit`, a package executable
// may omit `name`; the caller then fills it from the package.
static bool manifest(const uint8_t* data, uint64_t size, const char* id, App* out, bool inherit=false) {
    if (!data || !size || size>4096 || !identifier(id) || !out) return false;
    wiz_manifest m; if (wiz_manifest_parse(data,size,&m)!=0) return false;
    if (!(m.seen&WIZ_KEY_EXEC) || m.exec[0]!='/') return false;
    if (!(m.seen&WIZ_KEY_NAME) && !(inherit && wiz_name_is_package(m.exec))) return false;
    App app{}; if (!copy(app.id,sizeof(app.id),id) || !copy(app.icon,sizeof(app.icon),id)) return false;
    if (!copy(app.exec,sizeof(app.exec),m.exec)) return false;
    if (m.seen&WIZ_KEY_NAME) copy(app.name,sizeof(app.name),m.name);
    if (m.seen&WIZ_KEY_ICON) { copy(app.icon,sizeof(app.icon),m.icon); app.icon_set=true; }
    if (m.seen&WIZ_KEY_DESCRIPTION) copy(app.description,sizeof(app.description),m.description);
    if (m.seen&WIZ_KEY_KIND) copy(app.kind,sizeof(app.kind),m.kind);
    if (app.icon[0]!='/' && !identifier(app.icon)) return false;
    *out=app; return true;
}
// Ranged access to a package file through IO::read_at.
struct PackageSource { const IO* io; const char* path; };
static int package_read(void* ctx, uint64_t offset, void* buf, uint64_t len) {
    auto* source=(PackageSource*)ctx; uint64_t total=0;
    return source->io->read_at && source->io->read_at(source->path,offset,buf,len,&total) ? 1 : 0;
}
static bool file_exists(const IO& io, const char* path) {
    uint64_t total=0; return io.read_at && io.read_at(path,0,nullptr,0,&total);
}
static bool package_open(const IO& io, PackageSource* source, wiz_package* pkg) {
    uint64_t total=0;
    if (!io.read_at || !io.read_at(source->path,0,nullptr,0,&total)) return false;
    source->io=&io;
    return wiz_open(pkg,package_read,source,total)==0;
}
// Fill whatever the registration left unset from the package's own manifest.
// Returns false (leaving `app` untouched) if exec is not a valid package.
// `*status` (optional) receives WIZ_E_ABI when the package needs a newer OS:
// such an app must be hidden rather than shown as a plain executable.
static bool embed(const IO& io, App& app, int* status=nullptr) {
    PackageSource source{&io,app.exec}; wiz_package pkg; wiz_manifest m;
    if (status) *status=0;
    if (!package_open(io,&source,&pkg)) return false;
    int result=wiz_read_manifest(&pkg,&m);
    if (result==0) result=wiz_manifest_check_package(&m);
    if (result!=0) { if (status) *status=result; return false; }
    App next=app;
    if (!copy(next.pkgid,sizeof(next.pkgid),m.id) || !copy(next.version,sizeof(next.version),m.version)) return false;
    if (!next.name[0]) copy(next.name,sizeof(next.name),m.name);
    if (!next.description[0] && (m.seen&WIZ_KEY_DESCRIPTION)) copy(next.description,sizeof(next.description),m.description);
    if (!next.icon_set) { if (!copy(next.icon,sizeof(next.icon),(m.seen&WIZ_KEY_ICON) ? m.icon : m.id)) return false; }
    next.icon_mask=wiz_icon_mask(&pkg);
    next.packaged=true;
    app=next; return true;
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
        return load_bytes(path,data,size);
    }
    // Takes ownership of `data` (from image_allocate). `path` is the cache key:
    // a file path, or "<package>#icons/<N>.png" for an icon inside a package.
    const Image* load_bytes(const char* path, uint8_t* data, uint64_t size) {
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
    // One icon from inside a .wiz. Cached under "<package>#icons/<N>.png" and
    // fingerprinted like any file, so replacing the package refreshes it.
    const Image* load_package_icon(const char* package, uint32_t size) {
        char number[12]; unsigned n=0,v=size;
        do { number[n++]=(char)('0'+v%10); v/=10; } while(v);
        for (unsigned a=0;a<n/2;++a) { char t=number[a];number[a]=number[n-1-a];number[n-1-a]=t; } number[n]=0;
        char entry_name[32]; copy(entry_name,sizeof(entry_name),"icons/");
        if (!append(entry_name,sizeof(entry_name),number) || !append(entry_name,sizeof(entry_name),".png")) return nullptr;
        char key[256]; if (!copy(key,sizeof(key),package) || !append(key,sizeof(key),"#") ||
                           !append(key,sizeof(key),entry_name)) return nullptr;
        PackageSource source{&io,package}; wiz_package pkg; wiz_entry entry;
        if (!package_open(io,&source,&pkg) || wiz_find(&pkg,entry_name,&entry)<=0) return nullptr;
        if (!entry.size || entry.size>WIZ_MAX_ICON) return nullptr;
        auto* data=(uint8_t*)image_allocate(entry.size); if (!data) return nullptr;
        if (wiz_read_entry(&pkg,&entry,0,data,entry.size)!=0) { image_deallocate(data); return nullptr; }
        return load_bytes(key,data,entry.size);
    }
    // Same preference order as theme lookup: exact size, then the smallest
    // larger one, then the largest smaller one.
    const Image* embedded(const App& app, uint32_t requested) {
        if (!app.packaged || !app.icon_mask || !io.read_at) return nullptr;
        for (unsigned pass=0;pass<2;++pass) for (unsigned i=0;i<WIZ_ICON_SIZES;++i) {
            unsigned index=pass ? WIZ_ICON_SIZES-1-i : i, size=wiz_icon_sizes[index];
            if (!(app.icon_mask&(1u<<index))) continue;
            if ((!pass && size<requested) || (pass && size>=requested)) continue;
            if (const Image* image=load_package_icon(app.exec,size)) return image;
        }
        return nullptr;
    }
    // Icon resolution order: user/system theme (so themes can re-skin any app),
    // hicolor, the icon embedded in the app's own package, then generic ones.
    bool icon(const char* theme, const char* name, uint32_t size, Image* out, const App* app=nullptr) {
        const Image* source=lookup(theme,name,size);
        if (!source && !equal(theme,"hicolor") && name[0]!='/') source=lookup("hicolor",name,size);
        if (!source && app) source=embedded(*app,size);
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
constexpr unsigned max_extra = 4;
// `apps` is the Start menu. `extra` holds portable packages that are running
// from somewhere else (USB stick, Downloads): they are not in the menu but
// their windows still get the package's own name and icons.
struct Catalog { unsigned count; Entry apps[max_apps]; unsigned extra_count; Entry extra[max_extra];
                 Icons system[5]; char theme[64]; };
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
    for (auto& entry:catalog->extra) release_icons(entry.icons);
    for (auto& icons:catalog->system) release_icons(icons);
    image_deallocate(catalog);
}
static void load_icons(Store& store, const char* theme, const char* name, Icons& out,
                       uint32_t menu, uint32_t title, uint32_t task,
                       bool menu_visible, bool window_visible, const Icons* cached = nullptr,
                       const App* app = nullptr) {
    auto variant=[&](uint32_t size, Image& out, const Image* previous) {
        if (previous && previous->pixels) image_resize(*previous,previous->width,previous->height,&out);
        else store.icon(theme,name,size,&out,app);
    };
    if (menu_visible) variant(menu,out.menu,cached ? &cached->menu : nullptr);
    if (window_visible) {
        variant(title,out.title,cached ? &cached->title : nullptr);
        variant(task,out.task,cached ? &cached->task : nullptr);
    }
}
static bool has_exec(const Catalog* c, const char* exec) {
    for (unsigned i=0;i<c->count;++i) if (equal(c->apps[i].app.exec,exec)) return true;
    return false;
}
static bool has_package(const Catalog* c, const char* pkgid) {
    if (!pkgid[0]) return false;
    for (unsigned i=0;i<c->count;++i) if (equal(c->apps[i].app.pkgid,pkgid)) return true;
    return false;
}
static const Entry* find_extra(const Catalog* c, const char* exec) {
    if (c) for (unsigned i=0;i<c->extra_count && i<max_extra;++i)
        if (equal(c->extra[i].app.exec,exec)) return &c->extra[i];
    return nullptr;
}
// Directories scanned for executables that are not registered under /apps.
// /bin holds system programs; /home/Applications is the per-user "drop a
// .wiz here and it appears in Start" folder. Both accept .wiz and .elf.
static const char* const scan_roots[]={"/bin","/home/Applications"};

// App discovery is data-driven, in priority order:
//   1. /apps/<id>/app.manifest  (installed or pinned; may inherit from a .wiz)
//   2. unregistered executables in the scan roots (a .wiz contributes its
//      embedded name, id and icons; a plain .elf is named after its file)
// A package id appears once, so an installed copy hides a portable duplicate.
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
            App app{}; bool ok=manifest(data,size,name,&app,true); image_deallocate(data);
            if (!ok) continue;
            if (has_exec(catalog,app.exec)) continue;
            if (wiz_name_is_package(app.exec)) {
                // A pinned app records its name so the entry reads properly while
                // its drive is away; while the file is present the package wins.
                bool pinned=equal(app.kind,"pinned");
                char recorded[64]; copy(recorded,sizeof(recorded),app.name);
                if (pinned) app.name[0]=0;
                int status=0; bool embedded_ok=embed(store.io,app,&status);
                if (status==WIZ_E_ABI) continue;       // needs a newer WyzarOS: not launchable here
                if (!embedded_ok && !app.name[0]) copy(app.name,sizeof(app.name),recorded[0] ? recorded : app.id);
                if (pinned && store.io.read_at && !file_exists(store.io,app.exec)) app.available=false;
                if (has_package(catalog,app.pkgid)) continue;
            }
            catalog->apps[catalog->count++].app=app;
        }
        for (const char* root:scan_roots) {
            for (unsigned i=0;i<4096 && catalog->count<max_apps && store.io.list(root,i,name,&directory)>0;++i) {
                if (directory || !app_name_supported(name) || app_name_is_shell(name)) continue;
                App app{}; copy(app.exec,sizeof(app.exec),root);
                if (!append(app.exec,sizeof(app.exec),"/") || !append(app.exec,sizeof(app.exec),name)) continue;
                if (has_exec(catalog,app.exec)) continue;
                unsigned n=length(name)-4; if (n>=sizeof(app.id)) continue;
                for (unsigned k=0;k<n;++k) app.id[k]=name[k]; app.id[n]=0;
                unsigned display=n<sizeof(app.name)-1?n:sizeof(app.name)-1;
                for (unsigned k=0;k<display;++k) app.name[k]=app.id[k]; app.name[display]=0;
                copy(app.icon,sizeof(app.icon),app.id);
                if (wiz_name_is_package(name)) {
                    App packaged=app; packaged.name[0]=0;
                    int status=0;
                    if (embed(store.io,packaged,&status) && copy(packaged.id,sizeof(packaged.id),packaged.pkgid)) app=packaged;
                    if (status==WIZ_E_ABI) continue;
                    if (has_package(catalog,app.pkgid)) continue;
                }
                catalog->apps[catalog->count++].app=app;
            }
        }
    }
    for (unsigned i=0;i<catalog->count;++i)
        load_icons(store,theme,catalog->apps[i].app.icon,catalog->apps[i].icons,menu,title,task,
                    i<demand.menu_rows,demand.has_window(catalog->apps[i].app.exec),
                    known ? &known->apps[i].icons : nullptr,&catalog->apps[i].app);
    // Portable packages running from outside the menu keep their own identity.
    for (unsigned i=0;i<demand.window_count && i<32 && catalog->extra_count<max_extra;++i) {
        const char* exec=demand.executables[i];
        if (!wiz_name_is_package(exec) || has_exec(catalog,exec) || find_extra(catalog,exec)) continue;
        const Entry* previous=find_extra(known,exec);
        Entry& entry=catalog->extra[catalog->extra_count];
        if (previous) entry.app=previous->app;
        else {
            App app{}; if (!copy(app.exec,sizeof(app.exec),exec) || !embed(store.io,app)) continue;
            copy(app.id,sizeof(app.id),app.pkgid);
            entry.app=app;
        }
        load_icons(store,theme,entry.app.icon,entry.icons,menu,title,task,false,true,
                   previous ? &previous->icons : nullptr,&entry.app);
        ++catalog->extra_count;
    }
    const char* system[]={"terminal","visual-test","rainbow","boot-log","application-default"};
    for (unsigned i=0;i<5;++i)
        load_icons(store,theme,system[i],catalog->system[i],menu,title,task,false,demand.system[i],
                    known ? &known->system[i] : nullptr);
    return catalog;
}
}
#endif
