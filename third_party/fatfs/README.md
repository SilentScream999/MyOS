FatFs R0.16 by ChaN, with upstream patches 1 and 2 applied.

Source https://elm-chan.org/fsw/ff/arc/ff16.zip

Patches https://elm-chan.org/fsw/ff/patch/ff16p1.diff and https://elm-chan.org/fsw/ff/patch/ff16p2.diff

The license is retained in the source headers. Configuration enables FAT32 and exFAT
read/write, stack-based long filenames, UTF-8 API names, 512-byte sectors and
eight independent volume slices. Filesystem calls use the kernel storage mutex.
Formatting is disabled in the kernel and enabled in host tests to generate exFAT
fixtures. Creation and modification dates use the local RTC through get_fattime.
An unavailable RTC falls back to January 1, 1980. The WyzarOS-specific
wyzar_fatfs_sync function commits shared filesystem metadata before safe eject.
