# stb_image

`stb_image.h` v2.31 from upstream commit
`965c80bc0b138ccf03bfa4a76d7c0a4c8816414a`:
https://github.com/nothings/stb/blob/965c80bc0b138ccf03bfa4a76d7c0a4c8816414a/stb_image.h

Upstream SHA-256: `0e266e9f94592d9b87e97fa801a6d7e1bf8ebfe5809e5d7838f78257cba46e0c`.
One local change makes the `stbi__mad4sizes_valid` preprocessor guard match
`stbi__malloc_mad4` (include PNG/PSD), fixing PNG/JPEG-only compilation.
The complete MIT/public-domain dual license is retained at the end of the header.
WyzarOS uses the MIT option. Runtime configuration is in `src/gui/image.cpp`:
memory-only PNG/JPEG, bounded allocations, no host stdio, TLS, HDR or SIMD.
PNG framing/CRC validation and pixel-count limits are supplied by our wrapper.
