#!/usr/bin/env python3
# PS5 OpenGL - OpenGL implementation for PlayStation 5.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every unsupported narrow integer vertex input needs an integer fallback."""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = (root / 'third_party/mesa-26.2.0/src/gallium/auxiliary/util/u_vbuf.c').read_text()
table = source.split('vbuf_format_fallbacks[] = {', 1)[1].split('};', 1)[0]
pairs = dict(re.findall(r'\{\s*(PIPE_FORMAT_\w+),\s*(PIPE_FORMAT_\w+)\s*\}', table))
for bits in (8, 16):
    for count in range(1, 5):
        for kind in ('SINT', 'UINT'):
            narrow = 'PIPE_FORMAT_' + ''.join(f'{c}{bits}' for c in 'RGBA'[:count]) + '_' + kind
            wide = 'PIPE_FORMAT_' + ''.join(f'{c}32' for c in 'RGBA'[:count]) + '_' + kind
            assert pairs.get(narrow) == wide, (narrow, pairs.get(narrow), wide)
print('PASS: all 16 narrow integer formats preserve signedness and component count')
