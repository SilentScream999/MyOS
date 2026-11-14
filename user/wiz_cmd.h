#ifndef WYZAR_WIZ_CMD_H
#define WYZAR_WIZ_CMD_H

// ── wiz_cmd.h ─────────────────────────────────────────────────────────────────
//
// The `wiz` shell command: inspect, install, pin and remove .wiz packages.
//
//   wiz info <file|id>           name, version, contents; verifies checksums
//   wiz install [-f] <file>      copy into /apps/<id>/ and add to Start
//   wiz pin <file>               add a portable package to Start in place
//   wiz remove [--purge] <id>    undo install or pin (--purge: also app data)
//   wiz list                     show installed and pinned apps
//
// A .wiz always runs straight from wherever it is ("portable"). Installing is
// optional: it keeps a private copy under /apps/<id>/ so the app survives the
// original being deleted. Pinning only records where the file is, so the entry
// reads "unavailable" while its USB stick is out.
//
// Included by user/init.c after its syscall wrappers (sys_open, print, ...),
// and compiled on the host by tests/wiz_cmd_test.c with those faked.

#include "src/core/wiz.h"

#define WZ_PATH       256
#define WZ_APPS       "/apps"
#define WZ_DATA       "/home/.local/share"
#define WZ_O_CREATE   0x41
#define WZ_COPY_CHUNK 4096

// ── String and path helpers (no libc) ─────────────────────────────────────────
static int wz_copy(char* out, unsigned capacity, const char* text) {
    unsigned n = wiz_str_len(text);
    if (n >= capacity) return 0;
    for (unsigned i = 0; i <= n; ++i) out[i] = text[i];
    return 1;
}
static int wz_append(char* out, unsigned capacity, const char* text) {
    unsigned used = wiz_str_len(out), n = wiz_str_len(text);
    if (used + n >= capacity) return 0;
    for (unsigned i = 0; i <= n; ++i) out[used + i] = text[i];
    return 1;
}
// Build "<a><b><c>" into out; returns 0 if it does not fit.
static int wz_join(char* out, unsigned capacity, const char* a, const char* b, const char* c) {
    return wz_copy(out, capacity, a) && wz_append(out, capacity, b) && wz_append(out, capacity, c);
}
// Absolute path with ".", ".." and duplicate slashes resolved.
static int wz_absolute(const char* in, char* out, unsigned capacity) {
    char joined[WZ_PATH * 2]; joined[0] = 0;
    if (in[0] != '/') {
        if (!wz_copy(joined, sizeof(joined), current_dir) || !wz_append(joined, sizeof(joined), "/")) return 0;
    }
    if (!wz_append(joined, sizeof(joined), in)) return 0;
    if (capacity < 2) return 0;
    unsigned used = 1; out[0] = '/';
    const char* p = joined;
    while (*p) {
        while (*p == '/') ++p;
        const char* start = p;
        while (*p && *p != '/') ++p;
        unsigned n = (unsigned)(p - start);
        if (!n) break;
        if (n == 1 && start[0] == '.') continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            unsigned k = used; while (k > 1 && out[k - 1] != '/') --k;   // k = index after last '/'
            used = k > 1 ? k - 1 : 1;
            continue;
        }
        if (used + (used > 1 ? 1 : 0) + n >= capacity) return 0;
        if (used > 1) out[used++] = '/';
        for (unsigned i = 0; i < n; ++i) out[used++] = start[i];
    }
    out[used] = 0;
    return 1;
}
static void wz_say(const char* a, const char* b, const char* c) {
    print("wiz: "); print(a); if (b) print(b); if (c) print(c); print("\n");
}
static void wz_fail_path(const char* path, int error) {
    print("wiz: "); print(path); print(": "); print(wiz_error_text(error)); print("\n");
}

static int wz_exists(const char* path, uint64_t* type) {
    file_info info;
    if (sys_stat(path, &info) != 0) return 0;
    if (type) *type = info.type;
    return 1;
}
static int wz_is_dir(const char* path) { uint64_t t = 0; return wz_exists(path, &t) && t == 2; }

// ── Ranged file access through syscalls ───────────────────────────────────────
typedef struct { uint64_t fd; } wz_handle;

static int wz_read_at(void* ctx, wiz_u64 offset, void* buf, wiz_u64 len) {
    uint64_t fd = ((wz_handle*)ctx)->fd;
    char* p = (char*)buf;
    if (!len) return 1;
    if (sys_lseek(fd, (int64_t)offset, 0) < 0) return 0;
    while (len) {
        uint64_t want = len > WZ_COPY_CHUNK ? WZ_COPY_CHUNK : len;
        uint64_t got = sys_read(fd, p, want);
        if (got == 0 || got > want) return 0;           // EOF early, error (-1) or odd result (-2)
        p += got; len -= got;
    }
    return 1;
}

static int wz_open_package(const char* path, wz_handle* h, wiz_package* pkg) {
    h->fd = sys_open(path, 0);
    if (h->fd == (uint64_t)-1) return WIZ_E_IO;
    int64_t size = sys_lseek(h->fd, 0, 2);
    if (size < 0) { sys_close(h->fd); return WIZ_E_IO; }
    int status = wiz_open(pkg, wz_read_at, h, (wiz_u64)size);
    if (status) sys_close(h->fd);
    return status;
}

static wiz_manifest wz_manifest_a, wz_manifest_b;   // static: keep the user stack small

// Open, fully validate (structure + checksums) and read the manifest. On
// success the file stays open in *h. Prints the reason on failure.
static int wz_load(const char* path, wz_handle* h, wiz_package* pkg, wiz_manifest* manifest) {
    if (!wz_exists(path, 0)) { wz_say(path, ": no such file", 0); return WIZ_E_IO; }
    int status = wz_open_package(path, h, pkg);
    if (status) { wz_fail_path(path, status); return status; }
    int entries = wiz_validate(pkg);
    if (entries < 0) { wz_fail_path(path, entries); sys_close(h->fd); return entries; }
    status = wiz_read_manifest(pkg, manifest);
    if (status == 0) status = wiz_manifest_check_package(manifest);
    if (status) { wz_fail_path(path, status); sys_close(h->fd); return status; }
    return 0;
}

// Read a small file whole. Returns its length or -1.
static int64_t wz_read_small(const char* path, char* buf, unsigned capacity) {
    uint64_t fd = sys_open(path, 0);
    if (fd == (uint64_t)-1) return -1;
    unsigned used = 0;
    for (;;) {
        if (used >= capacity) { sys_close(fd); return -1; }
        uint64_t got = sys_read(fd, buf + used, capacity - used);
        if (got == 0) break;
        if (got > capacity - used) { sys_close(fd); return -1; }
        used += (unsigned)got;
    }
    sys_close(fd);
    return (int64_t)used;
}

// Registration manifest at /apps/<id>/app.manifest, or 0 if absent/unparsable.
static int wz_registration(const char* id, wiz_manifest* out) {
    char path[WZ_PATH]; static char text[WIZ_MAX_MANIFEST];
    if (!wz_join(path, sizeof(path), WZ_APPS "/", id, "/app.manifest")) return 0;
    int64_t n = wz_read_small(path, text, sizeof(text));
    return n > 0 && wiz_manifest_parse((const wiz_u8*)text, (wiz_u64)n, out) == 0;
}

static int wz_write_registration(const char* id, const char* text) {
    char path[WZ_PATH];
    if (!wz_join(path, sizeof(path), WZ_APPS "/", id, "/app.manifest")) return 0;
    return sys_replace(path, text, wiz_str_len(text)) == 0;
}

static int wz_make_app_dir(const char* id) {
    char dir[WZ_PATH];
    if (!wz_join(dir, sizeof(dir), WZ_APPS "/", id, "")) return 0;
    sys_mkdir(WZ_APPS, 0755);              // may already exist
    sys_mkdir(dir, 0755);
    return wz_is_dir(dir);
}

// Recursively delete a directory tree. Returns 1 on success.
static int wz_remove_tree(const char* path, int depth) {
    if (depth > 8) return 0;
    char name[WZ_PATH]; uint64_t size = 0, type = 0; int ok = 1;
    uint64_t index = 0;
    for (;;) {
        if (sys_readdir(path, index, name, &size, &type) <= 0) break;
        if (wiz_str_equal(name, ".") || wiz_str_equal(name, "..")) { ++index; continue; }
        char child[WZ_PATH];
        if (!wz_copy(child, sizeof(child), path) || !wz_append(child, sizeof(child), "/") ||
            !wz_append(child, sizeof(child), name)) { ok = 0; ++index; continue; }
        int removed = type == 2 ? wz_remove_tree(child, depth + 1) : (sys_unlink(child) == 0);
        if (!removed) { ok = 0; ++index; }    // a survivor shifts nothing: step past it
    }
    return ok && sys_rmdir(path) == 0;
}

// ── wiz info ──────────────────────────────────────────────────────────────────
static int wz_resolve(const char* arg, char* out, unsigned capacity) {
    unsigned n = wiz_str_len(arg);
    int has_slash = 0; for (unsigned i = 0; i < n; ++i) if (arg[i] == '/') has_slash = 1;
    if (!has_slash && !wiz_name_is_package(arg) && wiz_identifier(arg)) {
        // An id: follow its registration to the package file.
        if (!wz_registration(arg, &wz_manifest_b) || !(wz_manifest_b.seen & WIZ_KEY_EXEC)) {
            wz_say("no installed or pinned app named '", arg, "'"); return 0;
        }
        return wz_copy(out, capacity, wz_manifest_b.exec);
    }
    return wz_absolute(arg, out, capacity);
}

static int wz_info(const char* arg) {
    char path[WZ_PATH]; wz_handle h; wiz_package pkg;
    if (!wz_resolve(arg, path, sizeof(path))) return 1;
    if (wz_load(path, &h, &pkg, &wz_manifest_a)) return 1;
    wiz_manifest* m = &wz_manifest_a;
    print(m->name); print(" "); print(m->version); print("\n");
    print("  id          "); print(m->id); print("\n");
    if (m->seen & WIZ_KEY_DESCRIPTION) { print("  about       "); print(m->description); print("\n"); }
    print("  abi         "); print_dec(m->abi); print("\n");
    print("  file        "); print(path); print("\n");
    print("  program     "); print_dec(pkg.footer.elf_size); print(" bytes\n");
    print("  payload     "); print_dec(pkg.footer.payload_size); print(" bytes, checksum ok\n");
    wiz_u64 cursor = 0; wiz_entry e;
    while (wiz_next(&pkg, &cursor, &e) > 0) {
        if (e.is_dir) continue;
        print("    "); print(e.name); print(" ("); print_dec(e.size); print(" bytes)\n");
    }
    sys_close(h.fd);
    return 0;
}

// ── wiz install ───────────────────────────────────────────────────────────────
static int wz_install(const char* arg, int force) {
    char src[WZ_PATH], dir[WZ_PATH], final_path[WZ_PATH], tmp[WZ_PATH];
    if (!wz_absolute(arg, src, sizeof(src))) { wz_say("path too long", 0, 0); return 1; }
    wz_handle in; wiz_package pkg;
    if (wz_load(src, &in, &pkg, &wz_manifest_a)) return 1;
    wiz_manifest* m = &wz_manifest_a;
    if (!wz_join(dir, sizeof(dir), WZ_APPS "/", m->id, "") ||
        !wz_join(final_path, sizeof(final_path), dir, "/", m->id) || !wz_append(final_path, sizeof(final_path), ".wiz") ||
        !wz_join(tmp, sizeof(tmp), dir, "/install.tmp", "")) {
        wz_say("app id too long", 0, 0); sys_close(in.fd); return 1;
    }
    if (wiz_str_equal(src, final_path)) { wz_say(m->name, " is already installed from this file", 0); sys_close(in.fd); return 1; }

    // An existing install: refuse to go backwards unless forced.
    wz_handle old; wiz_package old_pkg; int upgrading = 0; static char old_version[32];
    old_version[0] = 0;
    if (wz_open_package(final_path, &old, &old_pkg) == 0) {
        if (wiz_read_manifest(&old_pkg, &wz_manifest_b) == 0 && (wz_manifest_b.seen & WIZ_KEY_VERSION)) {
            wz_copy(old_version, sizeof(old_version), wz_manifest_b.version);
            upgrading = 1;
        }
        sys_close(old.fd);
        if (upgrading && wiz_version_compare(m->version, old_version) < 0 && !force) {
            wz_say("a newer version (", old_version, ") is installed; use 'wiz install -f' to downgrade");
            sys_close(in.fd); return 1;
        }
    }
    if (!wz_make_app_dir(m->id)) { wz_say("cannot create ", dir, ": check storage"); sys_close(in.fd); return 1; }

    // Copy to a temporary name, verify the copy, then swap it into place.
    sys_unlink(tmp);
    uint64_t out = sys_open(tmp, WZ_O_CREATE);
    if (out == (uint64_t)-1) { wz_say("cannot create ", tmp, 0); sys_close(in.fd); return 1; }
    static char chunk[WZ_COPY_CHUNK];
    wiz_u64 total = pkg.footer.file_size, done = 0; int ok = 1;
    if (sys_lseek(in.fd, 0, 0) < 0) ok = 0;
    while (ok && done < total) {
        wiz_u64 want = total - done; if (want > sizeof(chunk)) want = sizeof(chunk);
        uint64_t got = sys_read(in.fd, chunk, want);
        if (got == 0 || got > want) { ok = 0; break; }
        if (sys_write(out, chunk, got) != got) { ok = 0; break; }
        done += got;
    }
    sys_close(in.fd);
    sys_close(out);
    if (!ok || done != total) { wz_say("copy failed (storage full or read error)", 0, 0); sys_unlink(tmp); return 1; }
    wz_handle check; wiz_package check_pkg;
    int status = wz_open_package(tmp, &check, &check_pkg);
    if (status == 0) { status = wiz_validate(&check_pkg); if (status > 0) status = 0; sys_close(check.fd); }
    if (status) { wz_fail_path(tmp, status); sys_unlink(tmp); return 1; }
    // Existing installs stay reachable until the atomic exchange commits. The
    // old file then lives at tmp; a crash during cleanup leaves only an extra
    // complete copy. On failure keep tmp too: storage may have gone offline.
    int published = wz_exists(final_path, 0) ? sys_exchange(tmp, final_path) : sys_rename(tmp, final_path);
    if (published != 0) { wz_say("cannot publish the copy; check storage before retrying", 0, 0); return 1; }
    sys_unlink(tmp);

    // Keep an existing install registration (it may carry the user's own
    // name/icon overrides); anything else becomes a fresh one.
    int keep = wz_registration(m->id, &wz_manifest_b) && (wz_manifest_b.seen & WIZ_KEY_KIND) &&
               wiz_str_equal(wz_manifest_b.kind, "installed");
    if (!keep) {
        static char text[WZ_PATH * 3];
        if (!wz_join(text, sizeof(text), "# Registered by 'wiz install'. Add name= or icon= lines to customize.\nexec=", final_path, "\nkind=installed\n") ||
            !wz_append(text, sizeof(text), "source=") || !wz_append(text, sizeof(text), src) || !wz_append(text, sizeof(text), "\n") ||
            !wz_write_registration(m->id, text)) { wz_say("installed the file but could not write its registration", 0, 0); return 1; }
    }
    int same = upgrading && wiz_version_compare(m->version, old_version) == 0;
    print("wiz: "); print(same ? "reinstalled " : upgrading ? "updated " : "installed ");
    print(m->name); print(" "); print(m->version);
    if (upgrading && !same) { print(" (was "); print(old_version); print(")"); }
    print("\n     "); print(final_path); print("\n     It now appears in the Start menu.\n");
    return 0;
}

// ── wiz pin ───────────────────────────────────────────────────────────────────
static int wz_pin(const char* arg) {
    char src[WZ_PATH]; wz_handle in; wiz_package pkg;
    if (!wz_absolute(arg, src, sizeof(src))) { wz_say("path too long", 0, 0); return 1; }
    if (wz_load(src, &in, &pkg, &wz_manifest_a)) return 1;
    sys_close(in.fd);
    wiz_manifest* m = &wz_manifest_a;
    if (wz_registration(m->id, &wz_manifest_b) && (wz_manifest_b.seen & WIZ_KEY_KIND) &&
        wiz_str_equal(wz_manifest_b.kind, "installed")) {
        wz_say(m->name, " is installed; run 'wiz remove ", m->id); return 1;
    }
    if (!wz_make_app_dir(m->id)) { wz_say("cannot create /apps entry: check storage", 0, 0); return 1; }
    static char text[WZ_PATH * 2];
    // The recorded name keeps the entry readable while the drive is away; while
    // the file is present the package's own name is shown.
    if (!wz_join(text, sizeof(text), "# Registered by 'wiz pin'. The app stays where it is.\nexec=", src, "\nkind=pinned\nname=") ||
        !wz_append(text, sizeof(text), m->name) || !wz_append(text, sizeof(text), "\n") ||
        !wz_write_registration(m->id, text)) { wz_say("could not write the registration", 0, 0); return 1; }
    print("wiz: pinned "); print(m->name); print(" "); print(m->version); print("\n     "); print(src);
    print("\n     It appears in Start; it reads unavailable whenever that file is missing.\n");
    return 0;
}

// ── wiz remove ────────────────────────────────────────────────────────────────
static int wz_remove(const char* id, int purge) {
    if (!wiz_identifier(id) || wiz_str_len(id) > 64) { wz_say("not a valid app id: ", id, 0); return 1; }
    if (!wz_registration(id, &wz_manifest_a)) { wz_say("no installed or pinned app named '", id, "'"); return 1; }
    int managed = (wz_manifest_a.seen & WIZ_KEY_KIND) &&
                  (wiz_str_equal(wz_manifest_a.kind, "installed") || wiz_str_equal(wz_manifest_a.kind, "pinned"));
    if (!managed) {
        wz_say("'", id, "' was registered by hand; delete /apps/<id> yourself if you really mean it"); return 1;
    }
    int pinned = wiz_str_equal(wz_manifest_a.kind, "pinned");
    char dir[WZ_PATH];
    if (!wz_join(dir, sizeof(dir), WZ_APPS "/", id, "") || !wz_remove_tree(dir, 0)) {
        wz_say("could not remove ", dir, " (is the app running?)"); return 1;
    }
    print("wiz: "); print(pinned ? "unpinned " : "removed "); print(id); print("\n");
    char data[WZ_PATH];
    if (!wz_join(data, sizeof(data), WZ_DATA "/", id, "")) return 0;
    if (wz_is_dir(data)) {
        if (purge) {
            if (wz_remove_tree(data, 0)) { print("     deleted its data in "); print(data); print("\n"); }
            else wz_say("could not delete ", data, 0);
        } else { print("     kept its data in "); print(data); print(" (use --purge to delete)\n"); }
    }
    return 0;
}

// ── wiz list ──────────────────────────────────────────────────────────────────
static int wz_list(void) {
    char name[WZ_PATH]; uint64_t size = 0, type = 0; int count = 0;
    for (uint64_t index = 0; sys_readdir(WZ_APPS, index, name, &size, &type) > 0; ++index) {
        if (type != 2 || !wiz_identifier(name) || !wz_registration(name, &wz_manifest_a)) continue;
        wiz_manifest* r = &wz_manifest_a; ++count;
        print("  "); print(name);
        for (unsigned i = wiz_str_len(name); i < 20; ++i) print(" ");
        print((r->seen & WIZ_KEY_KIND) ? r->kind : "manual");
        for (unsigned i = (r->seen & WIZ_KEY_KIND) ? wiz_str_len(r->kind) : 6; i < 11; ++i) print(" ");
        if (!(r->seen & WIZ_KEY_EXEC)) { print("(no exec)\n"); continue; }
        wz_handle h; wiz_package pkg;
        if (wz_open_package(r->exec, &h, &pkg) == 0) {
            if (wiz_read_manifest(&pkg, &wz_manifest_b) == 0 && (wz_manifest_b.seen & WIZ_KEY_VERSION)) {
                print(wz_manifest_b.version);
                for (unsigned i = wiz_str_len(wz_manifest_b.version); i < 9; ++i) print(" ");
            } else print("-        ");
            sys_close(h.fd);
        } else if (wz_exists(r->exec, 0)) print("-        ");
        else print("UNAVAILABLE ");
        print(r->exec); print("\n");
    }
    if (!count) print("  No installed or pinned apps. Try: wiz install <file.wiz>\n");
    return 0;
}

// ── Command entry point ───────────────────────────────────────────────────────
static void wz_usage(void) {
    print("Usage: wiz <command>\n");
    print("  info <file|id>            show a package's name, version and contents\n");
    print("  install [-f] <file.wiz>   copy into /apps and add to Start (-f: allow downgrade)\n");
    print("  pin <file.wiz>            add a portable package to Start without copying it\n");
    print("  remove [--purge] <id>     remove an installed/pinned app (--purge: and its data)\n");
    print("  list                      show installed and pinned apps\n");
    print("A .wiz also runs directly: 'run <file.wiz>' needs no installation.\n");
}

// `args` points just past the word "wiz"; it is modified in place.
static void wiz_command(char* args) {
    char* words[6]; int count = 0;
    while (*args && count < 6) {
        while (*args == ' ') ++args;
        if (!*args) break;
        words[count++] = args;
        while (*args && *args != ' ') ++args;
        if (*args) *args++ = 0;
    }
    if (!count) { wz_usage(); return; }
    const char* sub = words[0];
    int force = 0, purge = 0; const char* target = 0;
    for (int i = 1; i < count; ++i) {
        if (wiz_str_equal(words[i], "-f") || wiz_str_equal(words[i], "--force")) force = 1;
        else if (wiz_str_equal(words[i], "--purge")) purge = 1;
        else if (!target) target = words[i];
        else { wz_usage(); return; }
    }
    if (wiz_str_equal(sub, "list") && !target) wz_list();
    else if (wiz_str_equal(sub, "info") && target) wz_info(target);
    else if (wiz_str_equal(sub, "install") && target) wz_install(target, force);
    else if (wiz_str_equal(sub, "pin") && target) wz_pin(target);
    else if ((wiz_str_equal(sub, "remove") || wiz_str_equal(sub, "uninstall")) && target) wz_remove(target, purge);
    else wz_usage();
}

#endif
