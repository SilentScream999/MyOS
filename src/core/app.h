#ifndef WYZAR_APP_H
#define WYZAR_APP_H

// Both app extensions contain a static x86-64 ELF executable. Keep name
// matching independent of executable filename casing.
static inline int app_name_equal(const char* a, const char* b) {
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}

static inline int app_name_supported(const char* name) {
    unsigned n = 0;
    while (name[n]) ++n;
    return n > 4 && (app_name_equal(name + n - 4, ".elf") ||
                     app_name_equal(name + n - 4, ".wiz"));
}

static inline int app_name_is_shell(const char* name) {
    return app_name_equal(name, "init.elf") || app_name_equal(name, "init.wiz");
}

static inline int app_path_is_shell(const char* path) {
    return app_name_is_shell(path) ||
           app_name_equal(path, "/bin/init.elf") ||
           app_name_equal(path, "/bin/init.wiz");
}

#endif
