#ifndef WYZAR_GEN9_PIPELINE_H
#define WYZAR_GEN9_PIPELINE_H
#include <stdint.h>
// Fixed Gen9 3D copy pipeline, derived from IGT v1.28 rendercopy_gen9.c.
// https://chromium.googlesource.com/chromiumos/third_party/igt-gpu-tools/+/refs/tags/upstream/v1.28/lib/rendercopy_gen9.c
// Dynamic addresses, surfaces, blending and vertices are filled by gpu_render_commands.h.
/*
Copyright 1998-1999 Precision Insight, Inc., Cedar Park, Texas.
All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Copyright 2003,2006 Tungsten Graphics, Inc., Cedar Park, Texas.
All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Copyright © 2006-2011 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

Copyright © 2010 Red Hat, Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Copyright © 2011 Daniel Vetter

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

Copyright (c) 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011, 2012,
    2013, 2014 by Internet Systems Consortium, Inc. ("ISC")
Copyright (c) 1991, 1994, 1995, 1996, 1997, 1998, 1999, 2000, 2001,
    2002, 2003 by The Internet Software Consortium and Rich Salz

This code is derived from software contributed to the Internet Software
Consortium by Rich Salz.

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY
SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

Copyright (C) 1999-2007  Brian Paul   All Rights Reserved.
Copyright 2015 Philip Taylor <philip@zaynar.co.uk>
Copyright 2018 Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

*/
static constexpr uint32_t gpu_gen9_pipeline[] = {
    // PIPELINE_SELECT at byte 0
    0x69040300u,
    // STATE_SIP at byte 4
    0x61020001u, 0x00000000u, 0x00000000u,
    // 3DSTATE_PUSH_CONSTANT_ALLOC_VS at byte 16
    0x79120000u, 0x00000000u,
    // 3DSTATE_PUSH_CONSTANT_ALLOC_HS at byte 24
    0x79130000u, 0x00000000u,
    // 3DSTATE_PUSH_CONSTANT_ALLOC_DS at byte 32
    0x79140000u, 0x00000000u,
    // 3DSTATE_PUSH_CONSTANT_ALLOC_GS at byte 40
    0x79150000u, 0x00000000u,
    // 3DSTATE_PUSH_CONSTANT_ALLOC_PS at byte 48
    0x79160000u, 0x00000000u,
    // STATE_BASE_ADDRESS at byte 56
    0x61010010u, 0x00000001u, 0x00000000u, 0x00000001u, 0x00100001u, 0x00000000u,
    0x00100001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00100001u, 0x00000000u,
    0xFFFFF001u, 0x00001001u, 0xFFFFF001u, 0x00001001u, 0x00000000u, 0x00000000u,
    // MI_NOOP at byte 128
    0x00000000u,
    // 3DSTATE_VIEWPORT_STATE_POINTERS_CC at byte 132
    0x78230000u, 0x00000A20u,
    // 3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP at byte 140
    0x78210000u, 0x00000A40u,
    // 3DSTATE_URB_VS at byte 148
    0x78300000u, 0x08010040u,
    // 3DSTATE_URB_GS at byte 156
    0x78330000u, 0x08000000u,
    // 3DSTATE_URB_HS at byte 164
    0x78310000u, 0x08000000u,
    // 3DSTATE_URB_DS at byte 172
    0x78320000u, 0x08000000u,
    // 3DSTATE_BLEND_STATE_POINTERS at byte 180
    0x78240000u, 0x00000981u,
    // 3DSTATE_CC_STATE_POINTERS at byte 188
    0x780E0000u, 0x00000941u,
    // 3DSTATE_MULTISAMPLE at byte 196
    0x780D0000u, 0x00000000u,
    // 3DSTATE_SAMPLE_MASK at byte 204
    0x78180000u, 0x00000001u,
    // 3DSTATE_WM_HZ_OP at byte 212
    0x78520003u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_CONSTANT_HS at byte 232
    0x78190009u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_HS at byte 276
    0x781B0007u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_BINDING_TABLE_POINTERS_HS at byte 312
    0x78270000u, 0x00000000u,
    // 3DSTATE_SAMPLER_STATE_POINTERS_HS at byte 320
    0x782C0000u, 0x00000000u,
    // 3DSTATE_TE at byte 328
    0x781C0002u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_CONSTANT_GS at byte 344
    0x78160009u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_GS at byte 388
    0x78110008u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_BINDING_TABLE_POINTERS_GS at byte 428
    0x78290000u, 0x00000000u,
    // 3DSTATE_SAMPLER_STATE_POINTERS_GS at byte 436
    0x782E0000u, 0x00000000u,
    // 3DSTATE_CONSTANT_DS at byte 444
    0x781A0009u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_DS at byte 488
    0x781D0009u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_BINDING_TABLE_POINTERS_DS at byte 532
    0x78280000u, 0x00000000u,
    // 3DSTATE_SAMPLER_STATE_POINTERS_DS at byte 540
    0x782D0000u, 0x00000000u,
    // 3DSTATE_CONSTANT_VS at byte 548
    0x78150009u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_BINDING_TABLE_POINTERS_VS at byte 592
    0x78260000u, 0x00000000u,
    // 3DSTATE_SAMPLER_STATE_POINTERS_VS at byte 600
    0x782B0000u, 0x00000000u,
    // 3DSTATE_VS at byte 608
    0x78100007u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_STREAMOUT at byte 644
    0x781E0003u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_CLIP at byte 664
    0x78120002u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_SBE at byte 680
    0x781F0004u, 0x30400820u, 0x00000000u, 0x00000000u, 0x00000003u, 0x00000000u,
    // 3DSTATE_SBE_SWIZ at byte 704
    0x78510009u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_RASTER at byte 748
    0x78500003u, 0x00210000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_SF at byte 768
    0x78130002u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_WM at byte 784
    0x78140000u, 0x00000800u,
    // 3DSTATE_CONSTANT_PS at byte 792
    0x78170009u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_PS at byte 836
    0x7820000Au, 0x00000900u, 0x00000000u, 0x08080000u, 0x00000000u, 0x00000000u,
    0x1F000002u, 0x00060000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_PS_BLEND at byte 884
    0x784D0000u, 0x40000000u,
    // 3DSTATE_PS_EXTRA at byte 892
    0x784F0000u, 0x80000100u,
    // 3DSTATE_BINDING_TABLE_POINTERS_PS at byte 900
    0x782A0000u, 0x00000800u,
    // 3DSTATE_SAMPLER_STATE_POINTERS_PS at byte 908
    0x782F0000u, 0x000008C0u,
    // 3DSTATE_SCISSOR_STATE_POINTERS at byte 916
    0x780F0000u, 0x00000A80u,
    // 3DSTATE_WM_DEPTH_STENCIL at byte 924
    0x784E0002u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_DEPTH_BUFFER at byte 940
    0x78050006u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u,
    // 3DSTATE_HIER_DEPTH_BUFFER at byte 972
    0x78070003u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_STENCIL_BUFFER at byte 992
    0x78060003u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // 3DSTATE_CLEAR_PARAMS at byte 1012
    0x78040001u, 0x00000000u, 0x00000001u,
    // 3DSTATE_DRAWING_RECTANGLE at byte 1024
    0x79000002u, 0x00000000u, 0x003F003Fu, 0x00000000u,
    // 3DSTATE_VERTEX_BUFFERS at byte 1040
    0x78080003u, 0x0000400Cu, 0x00100A88u, 0x00000000u, 0x00000024u,
    // 3DSTATE_VERTEX_ELEMENTS at byte 1060
    0x78090005u, 0x02000000u, 0x22220000u, 0x02F60000u, 0x11230000u, 0x02850004u,
    0x11230000u,
    // 3DSTATE_VF_TOPOLOGY at byte 1088
    0x784B0000u, 0x0000000Fu,
    // 3DSTATE_VF at byte 1096
    0x780C0000u, 0x00000000u,
    // 3DSTATE_VF_INSTANCING at byte 1104
    0x78490001u, 0x00000000u, 0x00000000u,
    // 3DPRIMITIVE at byte 1116
    0x7B000005u, 0x00000000u, 0x00000003u, 0x00000000u, 0x00000001u, 0x00000000u,
    0x00000000u,
};
struct GpuStateWord { uint16_t index; uint32_t value; };
static constexpr GpuStateWord gpu_gen9_state[] = {
    {563, 0x00000092u},
    {609, 0x06200000u},
    {610, 0x00000002u},
    {611, 0x06200000u},
    {612, 0x00000002u},
    {613, 0x06200000u},
    {614, 0x00000002u},
    {615, 0x06200000u},
    {616, 0x00000002u},
    {617, 0x06200000u},
    {618, 0x00000002u},
    {619, 0x06200000u},
    {620, 0x00000002u},
    {621, 0x06200000u},
    {622, 0x00000002u},
    {623, 0x06200000u},
    {624, 0x00000002u},
    {625, 0x06200000u},
    {626, 0x00000002u},
    {627, 0x06200000u},
    {628, 0x00000002u},
    {629, 0x06200000u},
    {630, 0x00000002u},
    {631, 0x06200000u},
    {632, 0x00000002u},
    {633, 0x06200000u},
    {634, 0x00000002u},
    {635, 0x06200000u},
    {636, 0x00000002u},
    {637, 0x06200000u},
    {638, 0x00000002u},
    {639, 0x06200000u},
    {640, 0x00000002u},
    {648, 0xF99A130Cu},
    {649, 0x799A130Cu},
    {665, 0x3F800000u},
    {667, 0x3F800000u},
};
#endif
