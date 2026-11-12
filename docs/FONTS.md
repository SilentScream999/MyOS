# Desktop font files

Updated 2026-11-11.

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

For development, `tests/qemu_font_selection_test.py` demonstrates installing a
TTF and setting in an offline disposable image. It uses the current ext4 image
helper on an extracted partition; the removed FAT `SystemVolume` tool is not
part of this workflow. Image updates preserve custom fonts/settings while
refreshing bundled Selawik. No font conversion or atlas-generation is required.

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

The runtime test command and prerequisites are in [TESTING](TESTING.md).
