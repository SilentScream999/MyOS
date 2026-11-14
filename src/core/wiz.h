#ifndef WYZAR_WIZ_H
#define WYZAR_WIZ_H

// ── wiz.h ─────────────────────────────────────────────────────────────────────
//
// The .wiz application package format (version 2; reads version 1 too).
//
// A .wiz file is still a valid static x86-64 ELF executable, so the existing
// loader runs it unchanged. After the ELF image come a ustar archive (the
// "payload": manifest, icons, optional read-only data) and a fixed footer:
//
//   [ ELF image ][ ustar payload ][ 40-byte footer ]
//
// Footer (little endian):
//    0  char[4]  "WIZ1"
//    4  u32      format version (2)
//    8  u64      elf_size       bytes of ELF image to load
//   16  u64      payload_offset where the tar begins (>= elf_size)
//   24  u64      payload_size   tar bytes (multiple of 512, >= 1024)
//   32  u32      content_crc32  CRC-32 of all bytes before the footer (v1: payload only)
//   36  u32      footer_crc32   CRC-32 of footer bytes 0..35
//
// Payload layout (all names relative, no directories required):
//   app.manifest                 key=value metadata (see wiz_manifest)
//   icons/<N>.png                square PNG icons, N in 16,24,32,48,64,128,256,512
//   data/...                     optional read-only application resources
//
// The reader is ranged: it never needs the whole file in memory, so the
// desktop can read a name and icon out of a multi-megabyte package cheaply.
// Everything here is plain C (no libc, no allocation) so the kernel, the
// shell, applications and host tests share one implementation.
//
// Package contents are untrusted input. Every offset and size is bounds
// checked against the file size before it is used.

typedef unsigned char      wiz_u8;
typedef unsigned int       wiz_u32;
#if defined(__LP64__)
typedef unsigned long      wiz_u64;   // matches uint64_t on x86-64 (kernel, init.c, host)
#else
typedef unsigned long long wiz_u64;
#endif

#define WIZ_FOOTER_SIZE    40u
#define WIZ_FORMAT_VERSION 2u
#define WIZ_ABI            3u          // ABI 3 adds desktop launch and private console sessions
#define WIZ_MAX_MANIFEST   4096u
#define WIZ_MAX_ICON       (4u * 1024u * 1024u)
#define WIZ_MAX_ENTRIES    512u
#define WIZ_ICON_SIZES     8u

// Error codes (negative). 0 is success.
#define WIZ_E_IO        (-1)   // read failed / truncated
#define WIZ_E_NOT_WIZ   (-2)   // no footer: an ordinary ELF
#define WIZ_E_VERSION   (-3)   // newer format than this reader understands
#define WIZ_E_CORRUPT   (-4)   // inconsistent footer, header or tar
#define WIZ_E_UNSAFE    (-5)   // unsafe entry name or unsupported entry type
#define WIZ_E_NOENT     (-6)   // entry not found
#define WIZ_E_TOO_BIG   (-7)   // entry exceeds a caller/format limit
#define WIZ_E_MANIFEST  (-8)   // missing/invalid manifest fields
#define WIZ_E_ABI       (-9)   // package needs a newer OS ABI

// Ranged read callback: copy exactly `len` bytes at absolute file `offset`.
// Return nonzero on success, zero on any failure or short read.
typedef int (*wiz_read_fn)(void* ctx, wiz_u64 offset, void* buf, wiz_u64 len);

typedef struct {
    wiz_u64 file_size, elf_size, payload_offset, payload_size;
    wiz_u32 content_crc, version;
} wiz_footer;

typedef struct {
    wiz_read_fn read;
    void*       ctx;
    wiz_footer  footer;
} wiz_package;

typedef struct {
    char    name[104];
    wiz_u64 size;     // file data bytes
    wiz_u64 offset;   // absolute offset of data in the package file
    int     is_dir;
} wiz_entry;

// Standard icon sizes in ascending order. Bit i of an icon mask means the
// package ships icons/<wiz_icon_sizes[i]>.png.
static const wiz_u32 wiz_icon_sizes[WIZ_ICON_SIZES] = {16, 24, 32, 48, 64, 128, 256, 512};

// ── Small helpers ─────────────────────────────────────────────────────────────
// Explicit stores keep structure copies freestanding even at -O0: user apps
// do not link a libc, and a C structure assignment can emit a memcpy call.
static inline void wiz_copy_bytes(void* out, const void* in, unsigned size) {
    volatile wiz_u8* dst = (volatile wiz_u8*)out;
    const wiz_u8* src = (const wiz_u8*)in;
    for (unsigned i = 0; i < size; ++i) dst[i] = src[i];
}
static inline wiz_u32 wiz_le32(const wiz_u8* p) {
    return (wiz_u32)p[0] | ((wiz_u32)p[1] << 8) | ((wiz_u32)p[2] << 16) | ((wiz_u32)p[3] << 24);
}
static inline wiz_u64 wiz_le64(const wiz_u8* p) {
    return (wiz_u64)wiz_le32(p) | ((wiz_u64)wiz_le32(p + 4) << 32);
}

// CRC-32 (IEEE 802.3, same as zlib.crc32). Pass 0 to start; chain calls freely.
static inline wiz_u32 wiz_crc32(wiz_u32 crc, const void* data, wiz_u64 len) {
    static const wiz_u32 table[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u,
        0x4db26158u, 0x5005713cu, 0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu};
    const wiz_u8* p = (const wiz_u8*)data;
    crc = ~crc;
    for (wiz_u64 i = 0; i < len; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ table[crc & 15u];
        crc = (crc >> 4) ^ table[crc & 15u];
    }
    return ~crc;
}

static inline int wiz_str_equal(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static inline unsigned wiz_str_len(const char* s) {
    unsigned n = 0; while (s[n]) ++n; return n;
}
// Case-insensitive suffix test, used for ".wiz" detection.
static inline int wiz_name_is_package(const char* name) {
    unsigned n = wiz_str_len(name);
    if (n <= 4) return 0;
    const char* t = name + n - 4; const char* w = ".wiz";
    for (int i = 0; i < 4; ++i) {
        char c = t[i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != w[i]) return 0;
    }
    return 1;
}

// ── Footer and package open ───────────────────────────────────────────────────
static inline int wiz_footer_parse(const wiz_u8* raw, wiz_u64 file_size, wiz_footer* out) {
    if (raw[0] != 'W' || raw[1] != 'I' || raw[2] != 'Z' || raw[3] != '1') return WIZ_E_NOT_WIZ;
    if (wiz_le32(raw + 36) != wiz_crc32(0, raw, 36)) return WIZ_E_CORRUPT;
    wiz_u32 version = wiz_le32(raw + 4);
    if (version == 0) return WIZ_E_CORRUPT;
    if (version > WIZ_FORMAT_VERSION) return WIZ_E_VERSION;
    wiz_footer f;
    f.file_size = file_size;
    f.elf_size = wiz_le64(raw + 8);
    f.payload_offset = wiz_le64(raw + 16);
    f.payload_size = wiz_le64(raw + 24);
    f.content_crc = wiz_le32(raw + 32);
    f.version = version;
    if (file_size < WIZ_FOOTER_SIZE + 64u + 1024u) return WIZ_E_CORRUPT;
    if (f.elf_size < 64u || f.elf_size > file_size) return WIZ_E_CORRUPT;
    if (f.payload_offset < f.elf_size || f.payload_offset > file_size) return WIZ_E_CORRUPT;
    if (f.payload_size < 1024u || (f.payload_size & 511u)) return WIZ_E_CORRUPT;
    if (f.payload_size > file_size) return WIZ_E_CORRUPT;
    if (f.payload_offset > file_size - WIZ_FOOTER_SIZE ||
        f.payload_size != file_size - WIZ_FOOTER_SIZE - f.payload_offset) return WIZ_E_CORRUPT;
    wiz_copy_bytes(out, &f, sizeof(f));
    return 0;
}

// Open a package for ranged reading. Returns WIZ_E_NOT_WIZ for a plain ELF.
static inline int wiz_open(wiz_package* pkg, wiz_read_fn read, void* ctx, wiz_u64 file_size) {
    wiz_u8 raw[WIZ_FOOTER_SIZE];
    if (file_size < WIZ_FOOTER_SIZE) return WIZ_E_NOT_WIZ;
    pkg->read = read; pkg->ctx = ctx;
    if (!read(ctx, file_size - WIZ_FOOTER_SIZE, raw, WIZ_FOOTER_SIZE)) return WIZ_E_IO;
    int status = wiz_footer_parse(raw, file_size, &pkg->footer);
    if (status) return status;
    wiz_u8 magic[4];
    if (!read(ctx, 0, magic, 4)) return WIZ_E_IO;
    if (magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') return WIZ_E_CORRUPT;
    return 0;
}

// ── Tar walking ───────────────────────────────────────────────────────────────
static inline int wiz_safe_name(const char* name, unsigned length) {
    if (!length || length > 99 || name[0] == '/') return 0;
    int component_start = 1;
    for (unsigned i = 0; i < length; ++i) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.' || c == '/';
        if (!ok) return 0;
        if (c == '/') {
            if (component_start) return 0;           // "//" or leading "/"
            component_start = 1;
        } else {
            if (component_start && c == '.' &&
                (i + 1 == length || name[i + 1] == '/' ||
                 (name[i + 1] == '.' && (i + 2 == length || name[i + 2] == '/')))) return 0; // "." / ".."
            component_start = 0;
        }
    }
    return !component_start;                          // no trailing '/'
}

static inline int wiz_octal(const wiz_u8* p, unsigned width, wiz_u64* out) {
    wiz_u64 v = 0; unsigned i = 0;
    while (i < width && p[i] == ' ') ++i;
    int digits = 0;
    for (; i < width && p[i] >= '0' && p[i] <= '7'; ++i, ++digits) v = (v << 3) + (wiz_u64)(p[i] - '0');
    for (; i < width; ++i) if (p[i] != 0 && p[i] != ' ') return 0;   // trailing junk / base-256
    if (!digits || digits > 11) return 0;
    *out = v; return 1;
}

// Advance *cursor (a payload-relative offset, start at 0). Returns 1 with
// `entry` filled, 0 at the end-of-archive marker, or a negative error.
static inline int wiz_next(const wiz_package* pkg, wiz_u64* cursor, wiz_entry* entry) {
    wiz_u8 h[512];
    wiz_u64 at = *cursor;
    if (at + 512u > pkg->footer.payload_size) return WIZ_E_CORRUPT;   // archive lacks its end marker
    if (!pkg->read(pkg->ctx, pkg->footer.payload_offset + at, h, 512)) return WIZ_E_IO;
    int zero = 1;
    for (unsigned i = 0; i < 512; ++i) if (h[i]) { zero = 0; break; }
    if (zero) return 0;
    if (!(h[257] == 'u' && h[258] == 's' && h[259] == 't' && h[260] == 'a' && h[261] == 'r')) return WIZ_E_CORRUPT;
    wiz_u64 stored;
    if (!wiz_octal(h + 148, 8, &stored)) return WIZ_E_CORRUPT;
    wiz_u32 sum = 0;
    for (unsigned i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if (sum != stored) return WIZ_E_CORRUPT;
    if (h[345]) return WIZ_E_UNSAFE;                                   // ustar prefix unsupported
    char type = (char)h[156];
    int is_dir = type == '5';
    if (type != '0' && type != 0 && !is_dir) return WIZ_E_UNSAFE;      // no links, devices, pax, GNU long names
    unsigned length = 0; while (length < 100 && h[length]) ++length;
    if (length == 100) return WIZ_E_UNSAFE;
    if (is_dir && length && h[length - 1] == '/') --length;
    if (!wiz_safe_name((const char*)h, length)) return WIZ_E_UNSAFE;
    wiz_u64 size;
    if (!wiz_octal(h + 124, 12, &size)) return WIZ_E_CORRUPT;
    if (is_dir) size = 0;
    wiz_u64 padded = (size + 511u) & ~(wiz_u64)511u;
    if (size > pkg->footer.payload_size || padded < size) return WIZ_E_CORRUPT;
    if (512u + padded > pkg->footer.payload_size - at) return WIZ_E_CORRUPT;
    for (unsigned i = 0; i < length; ++i) entry->name[i] = (char)h[i];
    entry->name[length] = 0;
    entry->size = size;
    entry->offset = pkg->footer.payload_offset + at + 512u;
    entry->is_dir = is_dir;
    *cursor = at + 512u + padded;
    return 1;
}

// Find a file entry by exact name. Returns 1 found, WIZ_E_NOENT if absent
// (archive otherwise valid), or another negative error. First match wins.
static inline int wiz_find(const wiz_package* pkg, const char* name, wiz_entry* out) {
    wiz_u64 cursor = 0;
    for (unsigned n = 0; n < WIZ_MAX_ENTRIES; ++n) {
        int status = wiz_next(pkg, &cursor, out);
        if (status < 0) return status;
        if (status == 0) return WIZ_E_NOENT;
        if (!out->is_dir && wiz_str_equal(out->name, name)) return 1;
    }
    return WIZ_E_CORRUPT;
}

// Read part of an entry. `offset + len` must lie inside the entry.
static inline int wiz_read_entry(const wiz_package* pkg, const wiz_entry* e, wiz_u64 offset,
                                 void* buf, wiz_u64 len) {
    if (offset > e->size || len > e->size - offset) return WIZ_E_TOO_BIG;
    if (!len) return 0;
    return pkg->read(pkg->ctx, e->offset + offset, buf, len) ? 0 : WIZ_E_IO;
}

// Full structural validation: every tar header, name and size, plus the
// content checksum (legacy v1 covers only the payload). Returns the entry count or a negative error.
static inline int wiz_validate(const wiz_package* pkg) {
    wiz_u64 cursor = 0; wiz_entry e; int count = 0;
    for (;;) {
        int status = wiz_next(pkg, &cursor, &e);
        if (status < 0) return status;
        if (status == 0) break;
        if (++count > (int)WIZ_MAX_ENTRIES) return WIZ_E_CORRUPT;
    }
    wiz_u8 chunk[1024]; wiz_u32 crc = 0;
    wiz_u64 start = pkg->footer.version == 1 ? pkg->footer.payload_offset : 0;
    wiz_u64 size = pkg->footer.file_size - WIZ_FOOTER_SIZE - start;
    for (wiz_u64 done = 0; done < size;) {
        wiz_u64 n = size - done; if (n > sizeof(chunk)) n = sizeof(chunk);
        if (!pkg->read(pkg->ctx, start + done, chunk, n)) return WIZ_E_IO;
        crc = wiz_crc32(crc, chunk, n); done += n;
    }
    return crc == pkg->footer.content_crc ? count : WIZ_E_CORRUPT;
}

// Bit mask of available icon sizes (bit i = wiz_icon_sizes[i]).
static inline unsigned wiz_icon_mask(const wiz_package* pkg) {
    wiz_u64 cursor = 0; wiz_entry e; unsigned mask = 0;
    for (unsigned n = 0; n < WIZ_MAX_ENTRIES; ++n) {
        if (wiz_next(pkg, &cursor, &e) <= 0) break;
        if (e.is_dir || e.name[0] != 'i' || e.name[1] != 'c' || e.name[2] != 'o' ||
            e.name[3] != 'n' || e.name[4] != 's' || e.name[5] != '/') continue;
        wiz_u32 v = 0; const char* p = e.name + 6; unsigned digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4) { v = v * 10 + (wiz_u32)(*p - '0'); ++p; ++digits; }
        if (!digits || !wiz_str_equal(p, ".png") || e.size == 0 || e.size > WIZ_MAX_ICON) continue;
        for (unsigned i = 0; i < WIZ_ICON_SIZES; ++i) if (wiz_icon_sizes[i] == v) mask |= 1u << i;
    }
    return mask;
}

// ── Manifest ──────────────────────────────────────────────────────────────────
//
// ASCII key=value lines; blank lines and '#' comments allowed; LF or CRLF.
// Unknown keys are ignored so newer packages stay readable by older systems.
// Embedded (package) keys:  id name version abi description icon
// Registration keys (/apps/<id>/app.manifest): exec kind source, plus any
// embedded key as an override (name, icon, description).
#define WIZ_KEY_ID          0x01u
#define WIZ_KEY_NAME        0x02u
#define WIZ_KEY_EXEC        0x04u
#define WIZ_KEY_ICON        0x08u
#define WIZ_KEY_VERSION     0x10u
#define WIZ_KEY_DESCRIPTION 0x20u
#define WIZ_KEY_ABI         0x40u
#define WIZ_KEY_KIND        0x80u
#define WIZ_KEY_SOURCE      0x100u
#define WIZ_KEY_TYPE        0x200u

typedef struct {
    char id[96], name[64], version[32], description[128], icon[256], exec[256], kind[16], source[256], type[16];
    wiz_u32 abi;
    wiz_u32 seen;   // WIZ_KEY_* bits that were present
} wiz_manifest;

static inline int wiz_manifest_store(char* out, unsigned capacity, const char* text) {
    unsigned n = wiz_str_len(text);
    if (n >= capacity) return 0;
    for (unsigned i = 0; i <= n; ++i) out[i] = text[i];
    return 1;
}

// Parse. Returns 0 on success or WIZ_E_MANIFEST. Does not check required
// keys; callers decide (see wiz_manifest_check_package).
static inline int wiz_manifest_parse(const wiz_u8* data, wiz_u64 size, wiz_manifest* out) {
    wiz_manifest m;
    unsigned char* zero = (unsigned char*)&m;
    for (unsigned i = 0; i < sizeof(m); ++i) zero[i] = 0;
    if (!data || !size || size > WIZ_MAX_MANIFEST) return WIZ_E_MANIFEST;
    wiz_u64 at = 0;
    while (at < size) {
        char line[512]; unsigned used = 0;
        while (at < size && data[at] != '\n') {
            wiz_u8 c = data[at++];
            if ((c < 32 && c != '\r' && c != '\t') || c >= 127 || used + 1 >= sizeof(line)) return WIZ_E_MANIFEST;
            line[used++] = (char)c;
        }
        if (at < size) ++at;
        while (used && (line[used - 1] == '\r' || line[used - 1] == ' ' || line[used - 1] == '\t')) --used;
        line[used] = 0;
        char* key = line; while (*key == ' ' || *key == '\t') ++key;
        if (!*key || *key == '#') continue;
        char* value = key; while (*value && *value != '=') ++value;
        if (!*value) return WIZ_E_MANIFEST;
        *value++ = 0;
        char* end = value - 1; while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        while (*value == ' ' || *value == '\t') ++value;
        wiz_u32 bit = 0; char* target = 0; unsigned capacity = 0;
        if      (wiz_str_equal(key, "id"))          { bit = WIZ_KEY_ID;          target = m.id;          capacity = sizeof(m.id); }
        else if (wiz_str_equal(key, "name"))        { bit = WIZ_KEY_NAME;        target = m.name;        capacity = sizeof(m.name); }
        else if (wiz_str_equal(key, "exec"))        { bit = WIZ_KEY_EXEC;        target = m.exec;        capacity = sizeof(m.exec); }
        else if (wiz_str_equal(key, "icon"))        { bit = WIZ_KEY_ICON;        target = m.icon;        capacity = sizeof(m.icon); }
        else if (wiz_str_equal(key, "version"))     { bit = WIZ_KEY_VERSION;     target = m.version;     capacity = sizeof(m.version); }
        else if (wiz_str_equal(key, "description")) { bit = WIZ_KEY_DESCRIPTION; target = m.description; capacity = sizeof(m.description); }
        else if (wiz_str_equal(key, "kind"))        { bit = WIZ_KEY_KIND;        target = m.kind;        capacity = sizeof(m.kind); }
        else if (wiz_str_equal(key, "source"))      { bit = WIZ_KEY_SOURCE;      target = m.source;      capacity = sizeof(m.source); }
        else if (wiz_str_equal(key, "type"))        { bit = WIZ_KEY_TYPE;        target = m.type;        capacity = sizeof(m.type); }
        else if (wiz_str_equal(key, "abi"))         { bit = WIZ_KEY_ABI; }
        else continue;                                   // future metadata is not an error
        if ((m.seen & bit) || !*value) return WIZ_E_MANIFEST;
        if (bit == WIZ_KEY_ABI) {
            wiz_u32 v = 0; unsigned digits = 0;
            for (const char* p = value; *p; ++p, ++digits) {
                if (*p < '0' || *p > '9' || digits >= 4) return WIZ_E_MANIFEST;
                v = v * 10 + (wiz_u32)(*p - '0');
            }
            m.abi = v;
        } else if (!wiz_manifest_store(target, capacity, value)) {
            return WIZ_E_MANIFEST;
        }
        m.seen |= bit;
    }
    wiz_copy_bytes(out, &m, sizeof(m));
    return 0;
}

// Identifier rule shared by IDs and theme icon names.
static inline int wiz_identifier(const char* s) {
    if (!s || !*s || wiz_str_equal(s, ".") || wiz_str_equal(s, "..")) return 0;
    for (; *s; ++s) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.')) return 0;
    }
    return 1;
}

// Embedded manifests must identify the app and declare a supported ABI.
static inline int wiz_manifest_check_package(const wiz_manifest* m) {
    const wiz_u32 required = WIZ_KEY_ID | WIZ_KEY_NAME | WIZ_KEY_VERSION | WIZ_KEY_ABI;
    if ((m->seen & required) != required) return WIZ_E_MANIFEST;
    if (!wiz_identifier(m->id) || wiz_str_len(m->id) > 64) return WIZ_E_MANIFEST;
    if ((m->seen & WIZ_KEY_ICON) && !wiz_identifier(m->icon)) return WIZ_E_MANIFEST;
    if ((m->seen & WIZ_KEY_TYPE) && !wiz_str_equal(m->type,"gui") && !wiz_str_equal(m->type,"console")) return WIZ_E_MANIFEST;
    if (m->abi == 0) return WIZ_E_MANIFEST;
    if (m->abi > WIZ_ABI) return WIZ_E_ABI;
    return 0;
}

// Read and parse the embedded app.manifest using caller-supplied scratch
// (WIZ_MAX_MANIFEST bytes). The kernel loader uses this with heap memory
// because task stacks are small.
static inline int wiz_read_manifest_with(const wiz_package* pkg, wiz_manifest* out, wiz_u8* scratch) {
    wiz_entry e;
    int status = wiz_find(pkg, "app.manifest", &e);
    if (status < 0) return status == WIZ_E_NOENT ? WIZ_E_MANIFEST : status;
    if (e.size == 0 || e.size > WIZ_MAX_MANIFEST) return WIZ_E_TOO_BIG;
    status = wiz_read_entry(pkg, &e, 0, scratch, e.size);
    if (status) return status;
    return wiz_manifest_parse(scratch, e.size, out);
}
static inline int wiz_read_manifest(const wiz_package* pkg, wiz_manifest* out) {
    wiz_u8 buffer[WIZ_MAX_MANIFEST];
    return wiz_read_manifest_with(pkg, out, buffer);
}

// Dotted-numeric version comparison: -1, 0 or 1. Non-numeric parts count as 0.
static inline int wiz_version_compare(const char* a, const char* b) {
    while (*a || *b) {
        wiz_u64 x = 0, y = 0;
        while (*a >= '0' && *a <= '9') x = x * 10 + (wiz_u64)(*a++ - '0');
        while (*b >= '0' && *b <= '9') y = y * 10 + (wiz_u64)(*b++ - '0');
        if (x != y) return x < y ? -1 : 1;
        while (*a && *a != '.') ++a;
        while (*b && *b != '.') ++b;
        if (*a == '.') ++a;
        if (*b == '.') ++b;
    }
    return 0;
}

static inline const char* wiz_error_text(int error) {
    switch (error) {
        case 0:               return "ok";
        case WIZ_E_IO:        return "read error or truncated file";
        case WIZ_E_NOT_WIZ:   return "not a .wiz package (plain ELF)";
        case WIZ_E_VERSION:   return "package format is newer than this system understands";
        case WIZ_E_CORRUPT:   return "package is corrupt or inconsistent";
        case WIZ_E_UNSAFE:    return "package contains an unsafe or unsupported entry";
        case WIZ_E_NOENT:     return "entry not found";
        case WIZ_E_TOO_BIG:   return "entry too large";
        case WIZ_E_MANIFEST:  return "manifest missing or invalid (need id, name, version, abi)";
        case WIZ_E_ABI:       return "package needs a newer WyzarOS ABI";
        default:              return "unknown error";
    }
}

#endif
