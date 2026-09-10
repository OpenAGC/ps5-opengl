// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef PS5_AGC_PACKAGE_H
#define PS5_AGC_PACKAGE_H

#include <stddef.h>
#include <stdint.h>

#include "psbc_compile.h"

/* Returns -7 if the shader requires currently unprovisioned scratch memory. */
int ps5_agc_package_build(const PsbcShaderOutput *shader,
                          uint32_t esgs_ring_itemsize,
                          uint8_t **package, size_t *package_size);

struct pipe_screen;
struct pipe_resource;
/* Internal bring-up path, not a public GL compute capability. All resources
 * must remain owned and unchanged until this synchronous call returns.
 * Only raw UBO/SSBO descriptors are supported; unbound slots must be zero. */
int ps5_agc_compute_execute(struct pipe_screen *screen,
                            const PsbcShaderOutput *shader,
                            struct pipe_resource *descriptors,
                            struct pipe_resource *const *buffers,
                            unsigned buffer_count, const uint32_t groups[3]);

#endif
