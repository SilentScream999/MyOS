# Selawik UI font

Unmodified `selawk.ttf` from Microsoft's [Selawik 1.01 release](https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip).
The original `LICENSE.txt` is bundled alongside it (SIL Open Font License 1.1).
Microsoft describes Selawik as an [open-source replacement for Segoe UI](https://learn.microsoft.com/en-us/typography/font-list/selawik).

`make font-atlas` uses Python's standard library and system FreeType (`libfreetype6`
in WSL) to regenerate `src/gui/ui_font_data.h`. Normal builds use the checked-in
atlas and need no new font library. Generated metadata records the TTF SHA256.
Printable ASCII is baked at 12, 14, 16, 18, 21, 24, 28 and 32 pixels with grayscale
coverage and proportional advances. Unknown bytes display `?`; UTF-8 shaping,
runtime TTF selection and additional glyph ranges remain future work.

The compositor uses these atlases for window titles, Start/menu/task buttons and
the tray clock. Shell text, boot-log contents, debug HUD and panic text retain
their existing monospace bitmap renderer. The font is embedded in the kernel's
read-only data; it is not loaded from the system disk during startup.
The original font license is installed as `/share/fonts/selawik.txt` on the system
volume and in the recovery initrd.
