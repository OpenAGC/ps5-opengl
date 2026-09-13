# PS5 OpenGL - OpenGL implementation for PlayStation 5.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path


screen = (Path(__file__).resolve().parents[2] / "src/gallium/ps5/ps5_screen.c").read_text()
assert "caps->max_vertex_streams =\n      PS5_ENABLE_TRANSFORM_FEEDBACK_CANDIDATE ? PIPE_MAX_VERTEX_STREAMS : 0;" in screen
