#ifndef TAR_H
#define TAR_H

#include <stdint.h>
#include "vfs.h"
#include "helpers.h"

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

static inline uint64_t tar_parse_octal(const char* str, int len) {
    uint64_t val = 0;
    for (int i = 0; i < len; i++) {
        if (str[i] < '0' || str[i] > '7') break;
        val = (val << 3) + (uint64_t)(str[i] - '0');
    }
    return val;
}

static inline void tar_unpack(const uint8_t* archive, uint64_t archive_size) {
    if (!archive || archive_size < 512) return;

    print((char*)"[tar] Unpacking initrd archive...");

    uint64_t offset = 0;
    uint32_t file_count = 0;

    while (offset + 512 <= archive_size) {
        const tar_header* hdr = (const tar_header*)(archive + offset);

        // An empty block (name[0] == 0) indicates archive end
        if (hdr->name[0] == '\0') {
            break;
        }

        uint64_t file_size = tar_parse_octal(hdr->size, 11);

        // Build clean path with leading '/'
        char full_path[256];
        int pos = 0;
        full_path[pos++] = '/';

        // If prefix is present in ustar, prepend it
        if (hdr->prefix[0] != '\0') {
            for (int i = 0; hdr->prefix[i] && i < 155 && pos < 250; i++) {
                full_path[pos++] = hdr->prefix[i];
            }
            if (pos > 1 && full_path[pos - 1] != '/') full_path[pos++] = '/';
        }

        // Add file name, skipping leading './' or '/'
        int name_start = 0;
        if (hdr->name[0] == '.' && hdr->name[1] == '/') name_start = 2;
        else if (hdr->name[0] == '/') name_start = 1;

        for (int i = name_start; hdr->name[i] && i < 100 && pos < 254; i++) {
            full_path[pos++] = hdr->name[i];
        }
        full_path[pos] = '\0';

        // Remove trailing slash if any
        if (pos > 1 && full_path[pos - 1] == '/') {
            full_path[--pos] = '\0';
        }

        const uint8_t* data = archive + offset + 512;

        if (pos > 1) {
            if (hdr->typeflag == '5') {
                // Directory
                vfs_mkdir_p(full_path);
            } else if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
                // Regular file
                vfs_ensure_parent_dirs(full_path);
                file f;
                if (vfs_open(full_path, 0, &f) == 0) {
                    if (file_size > 0 && offset + 512 + file_size <= archive_size) {
                        vfs_write(&f, data, file_size);
                    }
                    vfs_close(&f);
                    file_count++;
                }
            }
        }

        // Advance to next 512-byte aligned block
        uint64_t data_blocks = (file_size + 511) / 512;
        offset += (1 + data_blocks) * 512;
    }

    print((char*)"[tar] Unpack complete.");
}

#endif // TAR_H
