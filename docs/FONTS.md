# Desktop font files

Wyzar reads real TrueType files at boot and rasterizes them into antialiased
desktop glyphs. Titles, taskbar labels, menus and the clock use the selected
font. The terminal, boot log and early boot diagnostics retain their bitmap font.

The bundled, OFL-licensed Selawik file is installed at
`/share/fonts/selawk.ttf` on the system volume and included in the recovery
initrd. `/etc/font.cfg` contains one absolute font path, optionally followed by
a newline. Without this setting, the default path is used.

To select a different font once file transfer is available:

1. Copy its `.ttf` file to `/share/fonts/custom.ttf` (or another VFS path).
2. Set `/etc/font.cfg` to `/share/fonts/custom.ttf`.
3. Reboot.

For development today, use `tools/system_volume.py`'s `SystemVolume.put` to
install the font and setting in an **offline, disposable** USB image. Image
updates preserve the selection and custom files, while refreshing bundled
Selawik. No font conversion or atlas-generation step is required.

The current text API displays printable ASCII, with `?` fallback. The decoder
supports Unicode cmap formats 4/12, short/long loca tables, simple quadratic
outlines, and composite outlines with transforms and point attachments.
It does not execute TrueType hinting instructions. CFF outlines (common in
`.otf` files), collections (`.ttc`), variable-font instance selection, kerning,
and complex-script shaping are not implemented. Eight sizes (12–32 px) are
cached at boot. The decoder caps file size, outline complexity, glyph dimensions
and atlas memory; unsupported or invalid fonts fall back to built-in Selawik.
The TTF file buffer and temporary rasterizer storage are freed after loading.

Format reference: [Microsoft OpenType specification](https://learn.microsoft.com/en-us/typography/opentype/spec/otff).

Validation in WSL:

```sh
clang++ -std=c++20 -O1 -g -fsanitize=address,undefined tests/truetype_test.cpp -o /tmp/ttf-test
/tmp/ttf-test assets/fonts/selawik/selawk.ttf
python3 tests/qemu_font_selection_test.py wyzar-usb.img assets/fonts/selawik/selawk.ttf
```
