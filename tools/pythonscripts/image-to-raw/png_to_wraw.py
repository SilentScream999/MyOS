from PIL import Image
import struct
import sys

# Writes a wallpaper file in the "WRAW" format understood by the kernel.
#
# Output format (little-endian):
#   0x00: b"WRAW"
#   0x04: uint32 width
#   0x08: uint32 height
#   0x0C: uint32 format (0 = BGRA8888)
#   0x10: raw pixels, BGRA per pixel, row-major, top-to-bottom


def main():
    if len(sys.argv) < 3:
        print("usage: python png_to_wraw.py <input.png> <output.wallpaper.raw>")
        sys.exit(2)

    in_path = sys.argv[1]
    out_path = sys.argv[2]

    img = Image.open(in_path).convert("RGBA")
    w, h = img.size
    pixels = list(img.getdata())

    out = bytearray()
    out += b"WRAW"
    out += struct.pack("<I", w)
    out += struct.pack("<I", h)
    out += struct.pack("<I", 0)  # BGRA8888

    for (r, g, b, a) in pixels:
        out.extend([b, g, r, a])

    with open(out_path, "wb") as f:
        f.write(out)

    print(f"wrote {out_path} ({w}x{h}, {len(out)} bytes)")


if __name__ == "__main__":
    main()

