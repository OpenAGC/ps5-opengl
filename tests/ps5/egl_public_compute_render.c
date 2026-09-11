// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

/* Internal CS -> sampled draw -> CS transitions. No public compute cap or
 * display qualification: the oracle reads an offscreen target, never swaps. */
#include <stdio.h>
#include <string.h>
#include "compiler/nir/nir_builder.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "ps5_screen.h"
#include "psbc_compile.h"

#define IMAGE_WORDS (64 * 3) /* 17x3 R32_FLOAT, 256-byte rows. */
#define GUARD_WORD UINT32_C(0xcdcdcdcd)

static nir_shader *build_compute(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      psbc_get_nir_options(PSBC_STAGE_COMPUTE), "compute-render-cs");
   b.shader->info.workgroup_size[0] = 16;
   b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ubos = b.shader->info.num_images = 1;
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *x = nir_iadd_imm(&b, nir_channel(&b, nir_load_local_invocation_id(&b), 0), 1);
   nir_def *sign = nir_load_ubo(&b, 1, 32, zero, zero, .align_mul = 4, .range = 4);
   nir_def *value = nir_fmul(&b, nir_fmul_imm(&b, nir_u2f32(&b, x), 0.25), sign);
   nir_image_store(&b, zero, nir_vec4(&b, x, nir_imm_int(&b, 1), zero, zero), zero,
      nir_vec4(&b, value, zero, zero, zero), zero, .image_dim = GLSL_SAMPLER_DIM_2D,
      .format = PIPE_FORMAT_R32_FLOAT, .src_type = nir_type_float32);
   return b.shader;
}

static nir_shader *build_vertex(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
      psbc_get_nir_options(PSBC_STAGE_VERTEX), "compute-render-vs");
   nir_variable *pos = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(), "position");
   pos->data.location = VARYING_SLOT_POS;
   nir_def *id = nir_load_vertex_id_zero_base(&b);
   nir_store_var(&b, pos, nir_vec4(&b,
      nir_bcsel(&b, nir_ieq_imm(&b, id, 1), nir_imm_float(&b, 3), nir_imm_float(&b, -1)),
      nir_bcsel(&b, nir_ieq_imm(&b, id, 2), nir_imm_float(&b, 3), nir_imm_float(&b, -1)),
      nir_imm_float(&b, 0), nir_imm_float(&b, 1)), 0xf);
   return b.shader;
}

static nir_shader *build_fragment(void)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
      psbc_get_nir_options(PSBC_STAGE_FRAGMENT), "compute-render-fs");
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 2);
   tex->op = nir_texop_txf;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_imm_ivec2(&b, 1, 1));
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(&b, 0));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *negative = nir_flt_imm(&b, nir_channel(&b, &tex->def, 0), 0);
   nir_variable *color = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(), "color");
   color->data.location = FRAG_RESULT_DATA0;
   nir_store_var(&b, color, nir_vec4(&b, nir_b2f32(&b, negative),
      nir_b2f32(&b, nir_inot(&b, negative)), nir_imm_float(&b, 0), nir_imm_float(&b, 1)), 0xf);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}

static unsigned count_image(const uint32_t *words, float sign)
{
   unsigned correct = 0;
   for (unsigned i = 0; i < IMAGE_WORDS; ++i) {
      uint32_t expected = GUARD_WORD;
      if (i >= 65 && i < 81) {
         float value = (i - 64) * 0.25f * sign;
         memcpy(&expected, &value, sizeof(expected));
      }
      correct += words[i] == expected;
   }
   return correct;
}

static unsigned count_pixels(const uint8_t *pixels, unsigned stride, float sign)
{
   const uint8_t expected[4] = {sign < 0 ? 255 : 0, sign > 0 ? 255 : 0, 0, 255};
   unsigned correct = 0;
   for (unsigned y = 0; y < 8; ++y)
      for (unsigned x = 0; x < 8; ++x)
         correct += !memcmp(pixels + y * stride + 4 * x, expected, sizeof(expected));
   return correct;
}

int main(void)
{
   int status = 1;
   struct pipe_screen *screen = ps5_screen_create();
   struct pipe_context *pipe = screen ? screen->context_create(screen, NULL, 0) : NULL;
   struct pipe_resource *image = NULL, *target = NULL;
   struct pipe_sampler_view *view = NULL;
   void *cs = NULL, *vs = NULL, *fs = NULL, *sampler = NULL;
   void *blend = NULL, *rasterizer = NULL, *depth = NULL;
   psbc_init();
   if (!pipe)
      goto cleanup;
   struct pipe_resource templ = {.target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_R32_FLOAT,
      .width0 = 17, .height0 = 3, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SHADER_IMAGE | PIPE_BIND_SAMPLER_VIEW};
   image = screen->resource_create(screen, &templ);
   templ.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   templ.width0 = templ.height0 = 8;
   templ.bind = PIPE_BIND_RENDER_TARGET;
   target = screen->resource_create(screen, &templ);
   void *image_data = NULL;
   size_t image_size = 0;
   if (!image || !target || ps5_resource_info(image, &image_data, &image_size, NULL) ||
       !image_data || image_size != IMAGE_WORDS * sizeof(uint32_t))
      goto cleanup;
   memset(image_data, 0xcd, image_size); /* The only CPU image write, before any dispatch. */
   struct pipe_compute_state compute = {.ir_type = PIPE_SHADER_IR_NIR, .prog = build_compute()};
   cs = pipe->create_compute_state(pipe, &compute);
   struct pipe_shader_state shader = {.type = PIPE_SHADER_IR_NIR, .ir.nir = build_vertex()};
   vs = pipe->create_vs_state(pipe, &shader);
   shader.ir.nir = build_fragment();
   fs = pipe->create_fs_state(pipe, &shader);
   if (!cs || !vs || !fs)
      goto cleanup;
   pipe->bind_compute_state(pipe, cs);
   pipe->bind_vs_state(pipe, vs);
   pipe->bind_fs_state(pipe, fs);
   const struct pipe_image_view iv = {.resource = image, .format = image->format,
      .access = PIPE_IMAGE_ACCESS_WRITE};
   pipe->set_shader_images(pipe, MESA_SHADER_COMPUTE, 0, 1, 0, &iv);
   struct pipe_sampler_view sv = {.target = PIPE_TEXTURE_2D, .format = image->format,
      .swizzle_r = PIPE_SWIZZLE_X, .swizzle_g = PIPE_SWIZZLE_Y,
      .swizzle_b = PIPE_SWIZZLE_Z, .swizzle_a = PIPE_SWIZZLE_W};
   view = pipe->create_sampler_view(pipe, image, &sv);
   const struct pipe_sampler_state ss = {.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE, .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_img_filter = PIPE_TEX_FILTER_NEAREST, .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = PIPE_TEX_MIPFILTER_NONE};
   sampler = pipe->create_sampler_state(pipe, &ss);
   const struct pipe_blend_state bs = {.rt[0].colormask = PIPE_MASK_RGBA};
   blend = pipe->create_blend_state(pipe, &bs);
   const struct pipe_rasterizer_state rs = {.front_ccw = true,
      .fill_front = PIPE_POLYGON_MODE_FILL, .fill_back = PIPE_POLYGON_MODE_FILL,
      .line_width = 1, .point_size = 1, .depth_clip_near = true, .depth_clip_far = true,
      .half_pixel_center = true};
   rasterizer = pipe->create_rasterizer_state(pipe, &rs);
   const struct pipe_depth_stencil_alpha_state ds = {0};
   depth = pipe->create_depth_stencil_alpha_state(pipe, &ds);
   if (!view || !sampler || !blend || !rasterizer || !depth)
      goto cleanup;
   pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &view);
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1, &sampler);
   pipe->bind_blend_state(pipe, blend);
   pipe->bind_rasterizer_state(pipe, rasterizer);
   pipe->bind_depth_stencil_alpha_state(pipe, depth);
   const struct pipe_viewport_state vp = {.scale = {4, 4, 0.5}, .translate = {4, 4, 0.5}};
   pipe->set_viewport_states(pipe, 0, 1, &vp);
   const struct pipe_framebuffer_state fb = {.width = 8, .height = 8, .nr_cbufs = 1,
      .cbufs[0] = {.texture = target, .format = PIPE_FORMAT_R8G8B8A8_UNORM}};
   pipe->set_framebuffer_state(pipe, &fb);
   for (unsigned phase = 0; phase < 4; ++phase) {
      const float sign = phase & 1 ? 1 : -1;
      float constants[4] = {sign, 0, 0, 0};
      const struct pipe_constant_buffer cb = {.user_buffer = constants, .buffer_size = sizeof(constants)};
      pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, 0, &cb);
      memset(constants, 0, sizeof(constants)); /* The constant uploader must own its copy. */
      const struct pipe_grid_info grid = {.work_dim = 1, .block = {16, 1, 1}, .grid = {1, 1, 1}};
      printf("[ps5-compute-render] phase=%u dispatch-start\n", phase);
      fflush(stdout);
      pipe->launch_grid(pipe, &grid);
      unsigned dispatches = 0, draws = 0;
      int compute_rc = ps5_context_last_compute_status(pipe, &dispatches);
      if (compute_rc || dispatches != phase + 1)
         goto cleanup;
      /* No CPU image map/read/copy between its GPU producer and sampler consumer. */
      const struct pipe_draw_info info = {.mode = MESA_PRIM_TRIANGLES, .instance_count = 1};
      const struct pipe_draw_start_count_bias draw = {.count = 3};
      pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
      int draw_rc = ps5_context_last_draw_status(pipe, &draws);
      struct pipe_transfer *transfer = NULL;
      uint8_t *pixels = draw_rc ? NULL : pipe_texture_map(pipe, target, 0, 0,
         PIPE_MAP_READ, 0, 0, 8, 8, &transfer);
      unsigned pixel_ok = pixels ? count_pixels(pixels, transfer->stride, sign) : 0;
      if (pixels)
         pipe->texture_unmap(pipe, transfer);
      unsigned image_ok = count_image(image_data, sign);
      printf("[ps5-compute-render] phase=%u compute=%d/%u draw=%d/%u pixels=%u/64 image=%u/%u\n",
         phase, compute_rc, dispatches, draw_rc, draws, pixel_ok, image_ok, IMAGE_WORDS);
      fflush(stdout);
      if (draw_rc || draws != phase + 1 || pixel_ok != 64 || image_ok != IMAGE_WORDS)
         goto cleanup;
   }
   status = 0;
cleanup:
   if (pipe) {
      pipe->bind_compute_state(pipe, NULL);
      pipe->bind_vs_state(pipe, NULL);
      pipe->bind_fs_state(pipe, NULL);
      pipe->bind_blend_state(pipe, NULL);
      pipe->bind_rasterizer_state(pipe, NULL);
      pipe->bind_depth_stencil_alpha_state(pipe, NULL);
      void *none = NULL;
      pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1, &none);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 1, NULL);
      pipe_sampler_view_reference(&view, NULL);
      if (cs) pipe->delete_compute_state(pipe, cs);
      if (vs) pipe->delete_vs_state(pipe, vs);
      if (fs) pipe->delete_fs_state(pipe, fs);
      if (sampler) pipe->delete_sampler_state(pipe, sampler);
      if (blend) pipe->delete_blend_state(pipe, blend);
      if (rasterizer) pipe->delete_rasterizer_state(pipe, rasterizer);
      if (depth) pipe->delete_depth_stencil_alpha_state(pipe, depth);
      pipe->destroy(pipe);
   }
   pipe_resource_reference(&image, NULL);
   pipe_resource_reference(&target, NULL);
   if (screen) screen->destroy(screen);
   psbc_shutdown();
   printf("[ps5-compute-render] result=%d\n", status);
   return status;
}
