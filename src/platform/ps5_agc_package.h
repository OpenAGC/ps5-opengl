// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef PS5_AGC_PACKAGE_H
#define PS5_AGC_PACKAGE_H

#include <stddef.h>
#include <stdint.h>

#include "psbc_compile.h"

/* Returns -7 for scratch: native addressing must be validated before execution. */
int ps5_agc_package_build(const PsbcShaderOutput *shader,
                          uint32_t esgs_ring_itemsize,
                          uint8_t **package, size_t *package_size);

struct pipe_screen;
struct pipe_resource;
#define PS5_AGC_COMPUTE_MAX_TEXTURES 16u
#define PS5_AGC_COMPUTE_MAX_RESOURCES (16u + 15u + 8u + PS5_AGC_COMPUTE_MAX_TEXTURES)
/* Internal bring-up path, not a public GL compute capability. All resources
 * must remain owned and unchanged until this synchronous call returns.
 * Raw UBO/SSBO and validated mip/layer image views; unbound slots are zero. */
int ps5_agc_compute_execute(struct pipe_screen *screen,
                            const PsbcShaderOutput *shader,
                            struct pipe_resource *descriptors,
                            struct pipe_resource *const *buffers,
                            unsigned buffer_count, const uint32_t groups[3]);

#endif
