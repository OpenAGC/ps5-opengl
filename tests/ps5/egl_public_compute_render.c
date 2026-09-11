// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

/* Internal CS -> sampled draw -> CS fetch/size transitions. Four alternating
 * R32_FLOAT phases verify all result channels and padding at units 0 and 7.
 * No public compute cap or display qualification: this test never swaps. */
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
   /* Indexed Gallium texture instructions require explicit usage metadata;
    * gather_info counts sampler variables but does not build these masks. */
   b.shader->info.num_textures = 1;
   BITSET_SET(b.shader->info.textures_used, 0);
   BITSET_SET(b.shader->info.textures_used_by_txf, 0);
   return b.shader;
}

static nir_shader *compute_sample(unsigned test, unsigned unit)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      psbc_get_nir_options(PSBC_STAGE_COMPUTE), "compute-sampled-input");
   b.shader->info.workgroup_size[0] = 16;
   b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = 1;
   b.shader->info.num_textures = unit + 1;
   BITSET_SET(b.shader->info.textures_used, unit);
   nir_def *id = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, test == 3 ? 1 : 2);
   tex->op = test == 3 ? nir_texop_txs : test == 4 ? nir_texop_txl : nir_texop_txf;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->texture_index = tex->sampler_index = unit;
   tex->coord_components = test == 3 ? 0 : 2;
   tex->dest_type = test == 1 ? nir_type_uint32 :
                   test == 2 || test == 3 ? nir_type_int32 : nir_type_float32;
   if (test == 3) {
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(&b, 0));
   } else {
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, test == 4 ?
         nir_imm_vec2(&b, 0.25, 0.5) : nir_vec2(&b, nir_iadd_imm(&b, id, 1), nir_imm_int(&b, 1)));
      tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_lod, test == 4 ?
         nir_imm_float(&b, 0) : nir_imm_int(&b, 0));
   }
   nir_def_init(&tex->instr, &tex->def, test == 3 ? 2 : 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *value = test == 3 ? nir_vec4(&b, nir_channel(&b, &tex->def, 0),
      nir_channel(&b, &tex->def, 1), nir_imm_int(&b, 0), nir_imm_int(&b, 1)) : &tex->def;
   nir_store_ssbo(&b, value, nir_imm_int(&b, 0), nir_imul_imm(&b, id, 16),
      .write_mask = 0xf, .align_mul = 16);
   return b.shader;
}

static unsigned count_sampled(const uint32_t *words, float sign, bool size_query)
{
   unsigned correct = 0;
   for (unsigned i = 0; i < 80; ++i) {
      uint32_t expected = GUARD_WORD;
      if (i >= 8 && i < 72) {
         unsigned lane = (i - 8) % 4, invocation = (i - 8) / 4;
         if (size_query) {
            const uint32_t size[4] = {17, 3, 0, 1};
            expected = size[lane];
         } else {
            float value = lane == 0 ? (invocation + 1) * 0.25f * sign : lane == 3 ? 1 : 0;
            memcpy(&expected, &value, 4);
         }
      }
      correct += words[i] == expected;
   }
   return correct;
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
   struct pipe_resource *image = NULL, *target = NULL, *render_pool = NULL;
   struct pipe_resource *sample_output = NULL;
   void *sample_cs[4] = {0};
   struct pipe_sampler_view *view = NULL;
   void *cs = NULL, *vs = NULL, *fs = NULL, *sampler = NULL;
   void *blend = NULL, *rasterizer = NULL, *depth = NULL;
   psbc_init();
   if (!pipe)
      goto cleanup;
   /* The graphics backend requires its normal render pool even for offscreen
    * draws. It may open VideoOut internally; this test never swaps buffers. */
   const struct pipe_resource pool_template = {.target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_R8G8B8A8_UNORM, .width0 = PS5_SCANOUT_WIDTH,
      .height0 = PS5_SCANOUT_HEIGHT, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET};
   render_pool = screen->resource_create(screen, &pool_template);
   if (!render_pool) {
      printf("[ps5-compute-render] setup-failed render-pool=%ux%u\n",
         (unsigned)PS5_SCANOUT_WIDTH, (unsigned)PS5_SCANOUT_HEIGHT);
      goto cleanup;
   }
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
   const struct pipe_resource output_template = {.target = PIPE_BUFFER,
      .format = PIPE_FORMAT_R8_UNORM, .width0 = 80 * 4, .height0 = 1,
      .depth0 = 1, .array_size = 1, .bind = PIPE_BIND_SHADER_BUFFER};
   sample_output = screen->resource_create(screen, &output_template);
   uint32_t *sample_words = NULL;
   size_t sample_bytes = 0;
   if (!sample_output || ps5_resource_info(sample_output, (void **)&sample_words, &sample_bytes, NULL) ||
       !sample_words || sample_bytes != 80 * 4)
      goto cleanup;
   const struct pipe_shader_buffer output_binding = {sample_output, 8 * 4, 64 * 4};
   pipe->set_shader_buffers(pipe, MESA_SHADER_COMPUTE, 0, 1, &output_binding, 1);
   pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 0, 1, 0, &view);
   pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 7, 1, 0, &view);
   for (unsigned i = 0; i < 4; ++i) {
      compute.prog = compute_sample(i & 1 ? 3 : 0, i >= 2 ? 7 : 0);
      sample_cs[i] = pipe->create_compute_state(pipe, &compute);
      if (!sample_cs[i])
         goto cleanup;
   }
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
      pipe->bind_compute_state(pipe, cs);
      pipe->launch_grid(pipe, &grid);
      unsigned dispatches = 0, draws = 0;
      int compute_rc = ps5_context_last_compute_status(pipe, &dispatches);
      if (compute_rc || dispatches != phase * 5 + 1)
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
      if (draw_rc || pixel_ok != 64)
         goto cleanup;
      for (unsigned i = 0; i < 4; ++i) {
         memset(sample_words, 0xcd, sample_bytes); /* Output only; source stays GPU-written. */
         pipe->bind_compute_state(pipe, sample_cs[i]);
         pipe->launch_grid(pipe, &grid);
         unsigned sampled_dispatches = 0;
         int rc = ps5_context_last_compute_status(pipe, &sampled_dispatches);
         unsigned correct = count_sampled(sample_words, sign, i & 1);
         printf("[ps5-compute-sampled] phase=%u case=%u rc=%d dispatches=%u words=%u/80\n",
            phase, i, rc, sampled_dispatches, correct);
         if (correct != 80)
            printf("[ps5-compute-sampled] first=%08x,%08x,%08x,%08x next=%08x,%08x,%08x,%08x guards=%08x/%08x\n",
               sample_words[8], sample_words[9], sample_words[10], sample_words[11],
               sample_words[12], sample_words[13], sample_words[14], sample_words[15],
               sample_words[0], sample_words[79]);
         if (rc || sampled_dispatches != phase * 5 + i + 2 || correct != 80)
            goto cleanup;
      }
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
      pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 0, 0, 8, NULL);
      pipe_sampler_view_reference(&view, NULL);
      if (cs) pipe->delete_compute_state(pipe, cs);
      for (unsigned i = 0; i < 4; ++i)
         if (sample_cs[i]) pipe->delete_compute_state(pipe, sample_cs[i]);
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
   pipe_resource_reference(&sample_output, NULL);
   pipe_resource_reference(&render_pool, NULL);
   if (screen) screen->destroy(screen);
   psbc_shutdown();
   printf("[ps5-compute-render] result=%d\n", status);
   return status;
}
