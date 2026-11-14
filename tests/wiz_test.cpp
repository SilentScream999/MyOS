// Tests for src/core/wiz.h and its integration in src/gui/resources.h.
// Run through tests/run_wiz_tests.py, which generates the fixture packages.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "../src/gui/resources.h"

using Bytes = std::vector<uint8_t>;

static int live = 0;
void* image_allocate(uint64_t size) { void* p = std::malloc(size); if (p) ++live; return p; }
void image_deallocate(void* p) { if (p) { --live; std::free(p); } }

static std::string dir;
static Bytes load(const std::string& name) {
    std::ifstream file(dir + "/" + name, std::ios::binary); assert(file.good());
    return Bytes((std::istreambuf_iterator<char>(file)), {});
}

// ── Strict in-memory reader: the library must never ask for bytes outside the file.
struct Mem { const Bytes* data; };
static int mem_read(void* ctx, uint64_t offset, void* buf, uint64_t len) {
    const Bytes& d = *((Mem*)ctx)->data;
    assert(offset <= d.size() && len <= d.size() - offset && "reader asked for bytes outside the file");
    std::memcpy(buf, d.data() + offset, len); return 1;
}
static int open_mem(wiz_package* pkg, Mem* mem, const Bytes& data) {
    mem->data = &data; return wiz_open(pkg, mem_read, mem, data.size());
}
// Everything a consumer might do with a package; must never crash or over-read.
static int exercise(const Bytes& data) {
    wiz_package pkg; Mem mem;
    int status = open_mem(&pkg, &mem, data);
    if (status) return status;
    int count = wiz_validate(&pkg);
    wiz_manifest m; wiz_entry e;
    (void)wiz_read_manifest(&pkg, &m); (void)wiz_icon_mask(&pkg);
    (void)wiz_find(&pkg, "icons/32.png", &e); (void)wiz_find(&pkg, "data/readme.txt", &e);
    return count;
}
static void le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void le64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void fix_footer(Bytes& d) {
    uint8_t* f = d.data() + d.size() - WIZ_FOOTER_SIZE;
    le32(f + 36, wiz_crc32(0, f, 36));
}
// Recompute header checksum, payload CRC and footer CRC after a deliberate edit.
static void fix_all(Bytes& d, uint64_t header_offset) {
    uint8_t* f = d.data() + d.size() - WIZ_FOOTER_SIZE;
    uint64_t poff = wiz_le64(f + 16);
    uint8_t* h = d.data() + poff + header_offset;
    unsigned sum = 0; for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    std::snprintf((char*)h + 148, 8, "%06o", sum); h[154] = 0; h[155] = ' ';
    le32(f + 32, wiz_crc32(0, d.data(), d.size() - WIZ_FOOTER_SIZE));
    fix_footer(d);
}

static void test_basics() {
    assert(wiz_crc32(0, "123456789", 9) == 0xCBF43926u);
    assert(wiz_crc32(wiz_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
    assert(wiz_name_is_package("a.wiz") && wiz_name_is_package("A.WIZ") && wiz_name_is_package("x.Wiz"));
    assert(!wiz_name_is_package(".wiz") && !wiz_name_is_package("wiz") && !wiz_name_is_package("a.wizz") &&
           !wiz_name_is_package("a.elf"));
    for (const char* ok : {"app.manifest", "icons/16.png", "data/a/b-c_d.txt", ".hidden", "a.b"})
        assert(wiz_safe_name(ok, (unsigned)std::strlen(ok)));
    for (const char* bad : {"", "/abs", "a//b", "../x", "a/../b", "a/./b", ".", "..", "a/", "sp ace", "tab\t", "uni\xc3\xa9",
                            "back\\slash"})
        assert(!wiz_safe_name(bad, (unsigned)std::strlen(bad)));
    std::string long_name(100, 'a'); assert(!wiz_safe_name(long_name.c_str(), 100));
    assert(wiz_version_compare("1.0", "1.0") == 0 && wiz_version_compare("1.2", "1.10") < 0 &&
           wiz_version_compare("2", "1.9.9") > 0 && wiz_version_compare("1.0.0", "1.0") == 0 &&
           wiz_version_compare("1.0.1", "1.0") > 0 && wiz_version_compare("", "1") < 0);
    assert(wiz_identifier("abc-1.2_x") && !wiz_identifier("") && !wiz_identifier("..") && !wiz_identifier("a/b"));
}

static void test_manifest_parser() {
    auto parse = [](const std::string& text, wiz_manifest* m) {
        return wiz_manifest_parse((const uint8_t*)text.data(), text.size(), m);
    };
    wiz_manifest m;
    assert(parse("# comment\r\n id = a.b \r\nname=Two Words\nversion=1.0\nabi=1\nfuture=whatever\n\n", &m) == 0);
    assert(!std::strcmp(m.id, "a.b") && !std::strcmp(m.name, "Two Words") && m.abi == 1);
    assert(m.seen == (WIZ_KEY_ID | WIZ_KEY_NAME | WIZ_KEY_VERSION | WIZ_KEY_ABI));
    assert(wiz_manifest_check_package(&m) == 0);
    m.abi = WIZ_ABI; assert(wiz_manifest_check_package(&m) == 0);
    m.abi = WIZ_ABI + 1; assert(wiz_manifest_check_package(&m) == WIZ_E_ABI);
    wiz_manifest sentinel; std::memset(&sentinel, 0x5a, sizeof(sentinel));
    std::vector<std::string> bad = {
        "id=a\nid=b", "id", "id=", "name=\x01", "name=caf\xc3\xa9", "abi=12345", "abi=1x", "abi=",
        "name=" + std::string(64, 'n'), "id=" + std::string(96, 'i'), std::string(600, 'x') + "=1",
        "exec=" + std::string(256, '/')};
    for (const auto& text : bad) {
        wiz_manifest copy = sentinel;
        assert(parse(text, &copy) == WIZ_E_MANIFEST);
        assert(std::memcmp(&copy, &sentinel, sizeof(copy)) == 0 && "failed parse must not modify output");
    }
    assert(wiz_manifest_parse(nullptr, 5, &m) != 0 && wiz_manifest_parse((const uint8_t*)"x", 0, &m) != 0);
    std::string big(WIZ_MAX_MANIFEST + 1, '#'); assert(parse(big, &m) != 0);
    // Package manifests need all four required keys and a sane id/icon.
    assert(parse("id=a\nname=A\nversion=1\nabi=1\ntype=gui\n", &m) == 0 && wiz_manifest_check_package(&m) == 0);
    assert(parse("id=a\nname=A\nversion=1\nabi=1\ntype=console\n", &m) == 0 && wiz_manifest_check_package(&m) == 0);
    assert(parse("id=a\nname=A\nversion=1\nabi=1\ntype=invalid\n", &m) == 0 && wiz_manifest_check_package(&m) == WIZ_E_MANIFEST);
    assert(parse("id=a\nname=A\nabi=1\n", &m) == 0 && wiz_manifest_check_package(&m) == WIZ_E_MANIFEST);
    assert(parse("id=a b\nname=A\nversion=1\nabi=1\n", &m) == 0 && wiz_manifest_check_package(&m) == WIZ_E_MANIFEST);
    assert(parse("id=a\nname=A\nversion=1\nabi=1\nicon=../x\n", &m) == 0 && wiz_manifest_check_package(&m) == WIZ_E_MANIFEST);
    assert(parse("id=a\nname=A\nversion=1\nabi=0\n", &m) == 0 && wiz_manifest_check_package(&m) == WIZ_E_MANIFEST);
}

static void test_format() {
    Bytes good = load("good.wiz"), elf = load("good.elf"), blue = load("blue.png"), plain = load("plain.elf");
    wiz_package pkg; Mem mem;
    assert(open_mem(&pkg, &mem, good) == 0);
    assert(pkg.footer.elf_size == elf.size() && pkg.footer.payload_offset == elf.size());
    assert(std::memcmp(good.data(), elf.data(), elf.size()) == 0);
    assert(wiz_validate(&pkg) == 4);
    wiz_manifest m; assert(wiz_read_manifest(&pkg, &m) == 0 && wiz_manifest_check_package(&m) == 0);
    assert(!std::strcmp(m.id, "counter") && !std::strcmp(m.name, "Counter") && !std::strcmp(m.version, "1.2.3"));
    assert(!std::strcmp(m.description, "Counts things.") && m.abi == 1);
    assert(wiz_icon_mask(&pkg) == ((1u << 0) | (1u << 2)));            // 16 and 32
    wiz_entry e; assert(wiz_find(&pkg, "icons/32.png", &e) == 1 && e.size == blue.size());
    Bytes icon(e.size); assert(wiz_read_entry(&pkg, &e, 0, icon.data(), e.size) == 0 && icon == blue);
    assert(wiz_read_entry(&pkg, &e, 1, icon.data(), e.size) == WIZ_E_TOO_BIG);
    assert(wiz_read_entry(&pkg, &e, e.size + 1, icon.data(), 0) == WIZ_E_TOO_BIG);
    assert(wiz_find(&pkg, "nothing", &e) == WIZ_E_NOENT);
    assert(wiz_find(&pkg, "data/readme.txt", &e) == 1 && e.size == 13);
    assert(wiz_find(&pkg, "icons", &e) == WIZ_E_NOENT);

    Bytes copy = plain; assert(open_mem(&pkg, &mem, copy) == WIZ_E_NOT_WIZ);        // an ordinary ELF
    Bytes tiny(10, 0);   assert(open_mem(&pkg, &mem, tiny) == WIZ_E_NOT_WIZ);
    Bytes empty;         assert(open_mem(&pkg, &mem, empty) == WIZ_E_NOT_WIZ);

    struct Case { const char* file; int open; int validate; int manifest; };
    const Case cases[] = {
        {"legacy.wiz",          0, 4, 0},
        {"no-manifest.wiz",     0, 1, WIZ_E_MANIFEST},
        {"future-abi.wiz",      0, 1, 0},
        {"future-format.wiz",   WIZ_E_VERSION, 0, 0},
        {"dotdot.wiz",          0, WIZ_E_UNSAFE, 0},
        {"absolute.wiz",        0, WIZ_E_UNSAFE, 0},
        {"symlink.wiz",         0, WIZ_E_UNSAFE, 0},
        {"pax.wiz",             0, WIZ_E_UNSAFE, 0},
        {"trailing-slash.wiz",  0, WIZ_E_UNSAFE, 0},
        {"bad-id.wiz",          0, 1, 0},
        {"shadow.wiz",          0, 2, 0},
        {"huge-size.wiz",       0, WIZ_E_CORRUPT, 0},
        {"no-end.wiz",          0, WIZ_E_CORRUPT, 0},
        {"lying-footer.wiz",    WIZ_E_CORRUPT, 0, 0},
    };
    for (const auto& c : cases) {
        Bytes d = load(c.file); std::printf("  fixture %-20s", c.file);
        int status = open_mem(&pkg, &mem, d);
        assert(status == c.open);
        if (status == 0) {
            assert(wiz_validate(&pkg) == c.validate);
            if (c.validate > 0) assert(wiz_read_manifest(&pkg, &m) == c.manifest || c.manifest == 0);
        }
        std::printf(" ok\n");
    }
    { Bytes d = load("future-abi.wiz"); open_mem(&pkg, &mem, d); assert(wiz_read_manifest(&pkg, &m) == 0);
      assert(wiz_manifest_check_package(&m) == WIZ_E_ABI); }
    { Bytes d = load("bad-id.wiz"); open_mem(&pkg, &mem, d); assert(wiz_read_manifest(&pkg, &m) == 0);
      assert(wiz_manifest_check_package(&m) == WIZ_E_MANIFEST); }
    { Bytes d = load("shadow.wiz"); open_mem(&pkg, &mem, d); assert(wiz_read_manifest(&pkg, &m) == 0);
      assert(!std::strcmp(m.id, "counter") && "first manifest entry must win"); }
}

static uint64_t lcg_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd() { lcg_state = lcg_state * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(lcg_state >> 33); }

static void test_corruption() {
    Bytes good = load("good.wiz");
    // 1. Every truncation is rejected.
    for (size_t length = 0; length < good.size(); ++length) {
        Bytes cut(good.begin(), good.begin() + length);
        assert(exercise(cut) < 0 && "truncated package accepted");
    }
    // 2. Every byte is covered, including executable code and ELF headers.
    for (size_t i = 0; i < good.size(); ++i) for (uint8_t mask : {uint8_t(1), uint8_t(0x80)}) {
        Bytes d = good; d[i] ^= mask;
        int status = exercise(d);
        assert(status < 0 && "undetected corruption");
    }
    // 3. Attacker-computed checksums: corrupt tar headers, then repair every
    //    CRC so only structural validation stands between data and the reader.
    wiz_package pkg; Mem mem; assert(open_mem(&pkg, &mem, good) == 0);
    std::vector<uint64_t> headers; { uint64_t cursor = 0; wiz_entry e;
        for (;;) { uint64_t at = cursor; int s = wiz_next(&pkg, &cursor, &e); if (s <= 0) { headers.push_back(at); break; } headers.push_back(at); } }
    unsigned accepted = 0;
    for (int trial = 0; trial < 6000; ++trial) {
        Bytes d = good; uint64_t header = headers[rnd() % headers.size()];
        uint64_t poff = wiz_le64(d.data() + d.size() - WIZ_FOOTER_SIZE + 16);
        int edits = 1 + (int)(rnd() % 4);
        for (int k = 0; k < edits; ++k) {
            uint64_t at = poff + header + (rnd() % 3 == 0 ? 100 + rnd() % 60 : rnd() % 512);
            d[at] = (uint8_t)(rnd() % 4 == 0 ? 0xff : rnd());
        }
        fix_all(d, header);
        if (exercise(d) >= 0) ++accepted;      // only requirement: no crash, no over-read
    }
    // 4. Attacker-computed footers with hostile field values.
    const uint64_t interesting[] = {0, 1, 63, 64, 65, 512, 1023, 1024, 1025, good.size() - 40, good.size() - 39,
                                    good.size(), good.size() + 1, 0xffffffffull, 0x7fffffffffffffffull,
                                    0xffffffffffffffffull, 0xffffffffffffffd8ull};
    for (uint64_t a : interesting) for (uint64_t b : interesting) for (uint64_t c : interesting) {
        Bytes d = good; uint8_t* f = d.data() + d.size() - WIZ_FOOTER_SIZE;
        le64(f + 8, a); le64(f + 16, b); le64(f + 24, c); fix_footer(d);
        exercise(d);
    }
    std::printf("  corruption: %zu truncations, %zu byte flips x2, 6000 repaired-CRC edits (%u still parse), %zu hostile footers\n",
                good.size(), good.size(), accepted, (size_t)16 * 16 * 16);
}

// ── Desktop integration: discovery, precedence, icons, running portable apps.
static std::map<std::string, Bytes> files;
static std::map<std::string, std::vector<std::pair<std::string, bool>>> directories;
static std::vector<std::string> ranged;
static uint8_t* io_read(const char* path, uint64_t limit, uint64_t* size) {
    auto it = files.find(path); if (it == files.end() || it->second.empty() || it->second.size() > limit) return nullptr;
    *size = it->second.size(); auto* out = (uint8_t*)image_allocate(*size);
    std::memcpy(out, it->second.data(), *size); return out;
}
static int io_list(const char* path, uint64_t index, char* name, bool* directory) {
    const auto& entries = directories[path]; if (index >= entries.size()) return 0;
    std::strcpy(name, entries[index].first.c_str()); *directory = entries[index].second; return 1;
}
static bool io_read_at(const char* path, uint64_t offset, void* buf, uint64_t len, uint64_t* total) {
    ranged.emplace_back(path);
    auto it = files.find(path); if (it == files.end()) return false;
    *total = it->second.size();
    if (len) { assert(buf && offset <= it->second.size() && len <= it->second.size() - offset); std::memcpy(buf, it->second.data() + offset, len); }
    return true;
}
static void text(const char* path, const char* value) { files[path] = Bytes(value, value + std::strlen(value)); }
static const resources::Entry* find(const resources::Catalog* c, const char* exec) {
    for (unsigned i = 0; i < c->count; ++i) if (resources::equal(c->apps[i].app.exec, exec)) return &c->apps[i];
    return nullptr;
}

static void test_desktop() {
    Bytes good = load("good.wiz"), other = load("other.wiz"), plain = load("plain.elf");
    Bytes alpha = load("alpha.png"), blue = load("blue.png");
    const uint32_t kBlue = 0xff0000ff, kAlpha = 0x80800000;
    resources::Store store{{io_read, io_list, io_read_at}};
    files["/share/icons/hicolor/32x32/apps/application-default.png"] = alpha;

    // 1. A package dropped in /bin: name, id, version and icons all come from the file.
    files["/bin/counter.wiz"] = good; files["/bin/plain.elf"] = plain;
    directories["/bin"] = {{"counter.wiz", false}, {"plain.elf", false}, {"junk", true}};
    auto* catalog = resources::build(store, 24, 16, 24);
    assert(catalog && catalog->count == 2);
    const auto* portable = find(catalog, "/bin/counter.wiz"); assert(portable);
    assert(portable->app.packaged && portable->app.available);
    assert(resources::equal(portable->app.id, "counter") && resources::equal(portable->app.pkgid, "counter"));
    assert(resources::equal(portable->app.name, "Counter") && resources::equal(portable->app.version, "1.2.3"));
    assert(resources::equal(portable->app.description, "Counts things.") && portable->app.icon_mask == 5);
    const auto* bare = find(catalog, "/bin/plain.elf"); assert(bare && !bare->app.packaged);
    assert(resources::equal(bare->app.name, "plain") && resources::equal(bare->app.id, "plain"));
    // Embedded icon sizes follow the theme rule: exact, then next larger, then largest smaller.
    auto app = portable->app;
    auto* img = store.embedded(app, 32); assert(img && img->pixels[0] == kBlue);
    img = store.embedded(app, 24); assert(img && img->pixels[0] == kBlue);
    img = store.embedded(app, 16); assert(img && img->pixels[0] == kAlpha);
    img = store.embedded(app, 17); assert(img && img->pixels[0] == kBlue);
    img = store.embedded(app, 200); assert(img && img->pixels[0] == kBlue);
    Image scaled{}; assert(store.icon("hicolor", "counter", 32, &scaled, &app));      // falls through to the package
    assert(scaled.pixels[0] == kBlue); image_free(&scaled);
    // Themes can re-skin any app: a themed icon beats the embedded one.
    files["/share/icons/hicolor/32x32/apps/counter.png"] = alpha;
    assert(store.icon("hicolor", "counter", 32, &scaled, &app) && scaled.pixels[0] == kAlpha); image_free(&scaled);
    files.erase("/share/icons/hicolor/32x32/apps/counter.png");
    // An app without a package falls back to the generic icon.
    assert(store.icon("hicolor", "plain", 32, &scaled, &bare->app) && scaled.pixels[0] == kAlpha); image_free(&scaled);
    // Icon pixels are only decoded for what is on screen.
    resources::Demand demand{}; demand.menu_rows = 2;
    auto* shown = resources::build(store, 24, 16, 24, demand, catalog);
    assert(shown->apps[0].icons.menu.pixels && shown->apps[1].icons.menu.pixels);
    assert(!shown->apps[0].icons.title.pixels);
    resources::release(shown);
    resources::release(catalog);

    // 2. Installed beats portable: same package id appears once, installed copy wins.
    text("/apps/counter/app.manifest", "exec=/apps/counter/counter.wiz\nkind=installed\n");
    files["/apps/counter/counter.wiz"] = good;
    directories["/apps"] = {{"counter", true}, {"README", false}};
    catalog = resources::build(store, 24, 16, 24);
    assert(catalog->count == 2 && resources::equal(catalog->apps[0].app.exec, "/apps/counter/counter.wiz"));
    assert(resources::equal(catalog->apps[0].app.id, "counter") && resources::equal(catalog->apps[0].app.kind, "installed"));
    assert(resources::equal(catalog->apps[0].app.name, "Counter") && !find(catalog, "/bin/counter.wiz"));
    resources::release(catalog);
    // Registration keys override the package; the package fills the rest.
    text("/apps/counter/app.manifest", "exec=/apps/counter/counter.wiz\nname=My Counter\nicon=custom\n");
    catalog = resources::build(store, 24, 16, 24);
    assert(resources::equal(catalog->apps[0].app.name, "My Counter") && resources::equal(catalog->apps[0].app.icon, "custom"));
    assert(resources::equal(catalog->apps[0].app.version, "1.2.3"));
    assert(store.icon("hicolor", "custom", 32, &scaled, &catalog->apps[0].app) && scaled.pixels[0] == kBlue);   // embedded still backs it up
    image_free(&scaled);
    resources::release(catalog);

    // 3. Pinned portable app: dimmed (unavailable) while its drive is missing, then the package wins.
    text("/apps/counter/app.manifest", "exec=/apps/counter/counter.wiz\nkind=installed\n");
    text("/apps/usb/app.manifest", "exec=/mnt/usb/usb.wiz\nkind=pinned\nname=Recorded Name\n");
    directories["/apps"] = {{"counter", true}, {"usb", true}};
    catalog = resources::build(store, 24, 16, 24);
    const auto* pinned = find(catalog, "/mnt/usb/usb.wiz"); assert(pinned);
    assert(!pinned->app.available && !pinned->app.packaged && resources::equal(pinned->app.name, "Recorded Name"));
    assert(resources::equal(pinned->app.id, "usb"));
    resources::release(catalog);
    files["/mnt/usb/usb.wiz"] = other;
    catalog = resources::build(store, 24, 16, 24);
    pinned = find(catalog, "/mnt/usb/usb.wiz"); assert(pinned && pinned->app.available && pinned->app.packaged);
    assert(resources::equal(pinned->app.name, "Other App") && resources::equal(pinned->app.pkgid, "other-app"));
    resources::release(catalog);
    // A name-less manifest for a missing package falls back to the id.
    text("/apps/usb/app.manifest", "exec=/mnt/gone/x.wiz\nkind=pinned\n");
    catalog = resources::build(store, 24, 16, 24);
    pinned = find(catalog, "/mnt/gone/x.wiz"); assert(pinned && !pinned->app.available && resources::equal(pinned->app.name, "usb"));
    resources::release(catalog);
    // Legacy: a .wiz that is really a plain ELF, registered by hand, works as before.
    text("/apps/usb/app.manifest", "exec=/apps/usb/old.wiz\nname=Legacy\n");
    files["/apps/usb/old.wiz"] = plain;
    catalog = resources::build(store, 24, 16, 24);
    pinned = find(catalog, "/apps/usb/old.wiz"); assert(pinned && pinned->app.available && !pinned->app.packaged);
    assert(resources::equal(pinned->app.name, "Legacy"));
    resources::release(catalog);
    // Strict parsing still applies to non-package executables.
    text("/apps/usb/app.manifest", "exec=/bin/plain.elf\n");
    catalog = resources::build(store, 24, 16, 24); assert(catalog->count == 2); resources::release(catalog);

    // 4. The per-user Applications folder.
    files.erase("/apps/usb/app.manifest"); directories["/apps"] = {{"counter", true}};
    files["/home/Applications/other.wiz"] = other;
    directories["/home/Applications"] = {{"other.wiz", false}, {"notes.txt", false}};
    catalog = resources::build(store, 24, 16, 24);
    const auto* dropped = find(catalog, "/home/Applications/other.wiz");
    assert(catalog->count == 3 && dropped && resources::equal(dropped->app.name, "Other App") && resources::equal(dropped->app.id, "other-app"));
    // A corrupt package is not fatal: it degrades to a plain executable entry.
    // (Menu-time reads check structure, not the whole-payload CRC: `wiz install` does that.)
    Bytes broken = good; broken[wiz_le64(broken.data() + broken.size() - WIZ_FOOTER_SIZE + 16) + 3] ^= 1;
    files["/bin/bad.wiz"] = broken; directories["/bin"].push_back({"bad.wiz", false});
    resources::release(catalog);
    catalog = resources::build(store, 24, 16, 24);
    const auto* bad = find(catalog, "/bin/bad.wiz"); assert(bad && !bad->app.packaged && resources::equal(bad->app.name, "bad"));
    resources::release(catalog);

    // A package that needs a newer WyzarOS ABI is hidden everywhere (never shown as a plain program).
    files["/bin/future.wiz"] = load("future-abi.wiz"); directories["/bin"].push_back({"future.wiz", false});
    text("/apps/futurist/app.manifest", "exec=/bin/future.wiz\n"); directories["/apps"].push_back({"futurist", true});
    catalog = resources::build(store, 24, 16, 24);
    assert(!find(catalog, "/bin/future.wiz"));
    resources::release(catalog);
    files.erase("/apps/futurist/app.manifest"); directories["/apps"].pop_back();

    // 5. Portable apps running from anywhere keep their own name and icons.
    catalog = resources::build(store, 24, 16, 24);
    resources::Demand running{}; running.window_count = 2;
    resources::copy(running.executables[0], 256, "/mnt/usb/portable.wiz");
    resources::copy(running.executables[1], 256, "/bin/plain.elf");
    files["/mnt/usb/portable.wiz"] = other;
    auto* withextra = resources::build(store, 24, 16, 24, running, catalog);
    assert(withextra->extra_count == 1 && resources::equal(withextra->extra[0].app.name, "Other App"));
    assert(withextra->extra[0].icons.title.pixels && withextra->extra[0].icons.task.pixels);
    assert(!find(withextra, "/mnt/usb/portable.wiz") && withextra->count == catalog->count);
    // Reusing the previous snapshot must not re-read the package.
    ranged.clear();
    auto* again = resources::build(store, 24, 16, 24, running, withextra);
    assert(again->extra_count == 1 && again->extra[0].icons.title.pixels);
    for (const auto& path : ranged) assert(path != "/mnt/usb/portable.wiz");
    // A window for an unreadable or non-package executable gets no extra entry.
    resources::Demand missing{}; missing.window_count = 1; resources::copy(missing.executables[0], 256, "/mnt/none/x.wiz");
    auto* none = resources::build(store, 24, 16, 24, missing, catalog); assert(none->extra_count == 0);
    resources::release(none); resources::release(again); resources::release(withextra); resources::release(catalog);

    // 6. Without ranged reads the desktop treats .wiz files as ordinary executables.
    resources::Store basic{{io_read, io_list}};
    catalog = resources::build(basic, 24, 16, 24);
    assert(find(catalog, "/bin/counter.wiz") && !find(catalog, "/bin/counter.wiz")->app.packaged);
    assert(resources::equal(find(catalog, "/bin/counter.wiz")->app.name, "counter"));
    resources::release(catalog); basic.clear();

    store.clear(); assert(live == 0 && "leaked image memory");
}

int main(int argc, char** argv) {
    assert(argc == 2); dir = argv[1];
    std::printf("basics\n");        test_basics();
    std::printf("manifest\n");      test_manifest_parser();
    std::printf("format\n");        test_format();
    std::printf("corruption\n");    test_corruption();
    std::printf("desktop\n");       test_desktop();
    std::printf("wiz tests passed\n");
    return 0;
}
