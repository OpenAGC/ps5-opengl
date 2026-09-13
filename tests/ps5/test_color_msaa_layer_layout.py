#!/usr/bin/env python3
# PS5 OpenGL - OpenGL implementation for PlayStation 5.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check single-sample and MSAA color addressing against pinned AMD tables."""
import subprocess
import tempfile
from pathlib import Path
from test_depth_layer_layout import reference_function, source

code = '''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define BITFIELD_BIT(i) (1u<<(i))
enum pipe_format { B1=1, B2=2, B4=4, B8=8, B16=16 };
static unsigned util_format_get_blocksize(enum pipe_format f) { return f; }
'''
for name in ('ps5_tiled_color_msaa4_tile', 'ps5_tiled_affine_offset', 'ps5_tiled_color_offset',
             'ps5_tiled_color_msaa4_offset'):
    start = source.index(name + '(')
    start = source.rfind('static ', 0, start)
    code += source[start:source.index('\n}\n', start) + 3]
checks = []
for bpe, width, height in ((1,256,256), (2,256,128), (4,128,128), (8,128,64), (16,64,64)):
    code += reference_function(1, bpe, f'single{bpe}', width, 'R', height)
    code += f'''
static void single{bpe}(void) {{
    for (unsigned z=0; z<32; ++z)
    for (unsigned y=0; y<{height*2}; ++y) for (unsigned x=0; x<{width*2}; ++x)
        assert(ps5_tiled_color_offset({bpe},x,y,{width*3},z) ==
               reference_single{bpe}(x,y,0,{width*3},z));
}}
'''
    checks.append(f'single{bpe}();')
for bpe, width, height in ((1,128,128), (2,128,64), (4,64,64), (8,64,32), (16,32,32)):
    code += reference_function(4, bpe, f'color{bpe}', width, 'R', height)
    code += f'''
static void check{bpe}(void) {{
    for (unsigned z=0; z<32; ++z) for (unsigned s=0; s<4; ++s)
    for (unsigned y=0; y<{height*2}; ++y) for (unsigned x=0; x<{width*2}; ++x)
        assert(ps5_tiled_color_msaa4_offset({bpe},x,y,s,{width*3},z) ==
               reference_color{bpe}(x,y,s,{width*3},z));
    assert(ps5_tiled_color_msaa4_offset({bpe},0,0,4,{width},0)==SIZE_MAX);
}}
'''
    checks.append(f'check{bpe}();')
code += 'int main(void) {' + ''.join(checks) + '}\n'
with tempfile.TemporaryDirectory() as directory:
    executable = str(Path(directory) / 'color-msaa-layout')
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-x','c','-',
                    '-o',executable], input=code, text=True, check=True)
    subprocess.run([executable], check=True)
print('PASS: actual single-sample/MSAA color addressing, five texel sizes, 32 layers, all samples and tile boundaries')
