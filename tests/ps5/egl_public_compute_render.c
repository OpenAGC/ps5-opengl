// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

/* Internal CS -> sampled draw -> CS fetch/size transitions. Twelve alternating
 * R32_FLOAT/UINT/SINT phases verify channels and padding at units 0 and 7.
 * Native qualified: 138 dispatches, 12 draws, including 52 mip/view checks.
 * No public compute cap or display qualification: this test never swaps. */
#include <stdio.h>
#include <string.h>
#include "compiler/nir/nir_builder.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "ps5_screen.h"
#include "psbc_compile.h"

#define IMAGE_WORDS (64 * 3) /* 17x3 R32, 256-byte rows. */
#define GUARD_WORD UINT32_C(0xcdcdcdcd)
static const enum pipe_format image_formats[] = {
   PIPE_FORMAT_R32_FLOAT, PIPE_FORMAT_R32_UINT, PIPE_FORMAT_R32_SINT};

static nir_shader *build_compute(unsigned kind)
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
   if (kind == 1)
      value = nir_iadd(&b, nir_imul_imm(&b, x, 3), nir_bcsel(&b, nir_flt_imm(&b, sign, 0),
         nir_imm_int(&b, 0x80000005u), nir_imm_int(&b, 0x10000005u)));
   else if (kind == 2) {
      value = nir_iadd_imm(&b, nir_imul_imm(&b, x, 7), 1000);
      value = nir_bcsel(&b, nir_flt_imm(&b, sign, 0), nir_ineg(&b, value), value);
   }
   nir_image_store(&b, zero, nir_vec4(&b, x, nir_imm_int(&b, 1), zero, zero), zero,
      nir_vec4(&b, value, zero, zero, zero), zero, .image_dim = GLSL_SAMPLER_DIM_2D,
      .format = image_formats[kind], .src_type = kind == 0 ? nir_type_float32 :
         kind == 1 ? nir_type_uint32 : nir_type_int32);
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

static nir_shader *build_fragment(unsigned kind)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
      psbc_get_nir_options(PSBC_STAGE_FRAGMENT), "compute-render-fs");
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 2);
   tex->op = nir_texop_txf;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = kind == 0 ? nir_type_float32 : kind == 1 ? nir_type_uint32 : nir_type_int32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_imm_ivec2(&b, 1, 1));
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(&b, 0));
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *negative = nir_flt_imm(&b, nir_channel(&b, &tex->def, 0), 0);
   if (kind == 1)
      negative = nir_ine_imm(&b, nir_iand_imm(&b, nir_channel(&b, &tex->def, 0), 0x80000000u), 0);
   else if (kind == 2)
      negative = nir_ilt_imm(&b, nir_channel(&b, &tex->def, 0), 0);
   nir_def *channels_ok = nir_imm_true(&b);
   for (unsigned lane = 1; lane < 4; ++lane) {
      nir_def *component = nir_channel(&b, &tex->def, lane);
      nir_def *matches = kind == 0 ? nir_feq_imm(&b, component, lane == 3 ? 1 : 0) :
                                    nir_ieq_imm(&b, component, lane == 3 ? 1 : 0);
      channels_ok = nir_iand(&b, channels_ok, matches);
   }
   nir_variable *color = nir_variable_create(b.shader, nir_var_shader_out, glsl_vec4_type(), "color");
   color->data.location = FRAG_RESULT_DATA0;
   nir_store_var(&b, color, nir_vec4(&b, nir_b2f32(&b, negative),
      nir_b2f32(&b, nir_inot(&b, negative)), nir_b2f32(&b, nir_inot(&b, channels_ok)), nir_imm_float(&b, 1)), 0xf);
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
   tex->op = test == 3 ? nir_texop_txs : test >= 4 ? nir_texop_txl : nir_texop_txf;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->texture_index = tex->sampler_index = unit;
   tex->coord_components = test == 3 ? 0 : 2;
   tex->dest_type = test == 1 ? nir_type_uint32 :
                   test == 2 || test == 3 ? nir_type_int32 : nir_type_float32;
   if (test == 3) {
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(&b, 0));
   } else {
      tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, test >= 4 ?
         nir_vec2(&b, nir_bcsel(&b, nir_ine_imm(&b, nir_iand_imm(&b, id, 1), 0),
            nir_imm_float(&b, test == 5 ? 1.25 : 0.75),
            nir_imm_float(&b, test == 5 ? -0.25 : 0.25)), test == 5 ?
            nir_bcsel(&b, nir_ine_imm(&b, nir_iand_imm(&b, id, 1), 0),
               nir_imm_float(&b, 1.5), nir_imm_float(&b, -0.5)) : nir_imm_float(&b, 0.5)) :
         nir_vec2(&b, nir_iadd_imm(&b, id, 1), nir_imm_int(&b, 1)));
      tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_lod, test >= 4 ?
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

static uint32_t expected_red(unsigned kind, unsigned x, float sign)
{
   if (kind == 1)
      return (sign < 0 ? UINT32_C(0x80000005) : UINT32_C(0x10000005)) + x * 3;
   if (kind == 2)
      return (uint32_t)((sign < 0 ? -1 : 1) * (1000 + (int)x * 7));
   float value = x * 0.25f * sign;
   uint32_t word;
   memcpy(&word, &value, 4);
   return word;
}

static nir_shader *compute_mip(unsigned operation, unsigned unit, unsigned level)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      psbc_get_nir_options(PSBC_STAGE_COMPUTE), "compute-mip");
   b.shader->info.workgroup_size[0] = 16;
   b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = 1;
   b.shader->info.num_textures = unit + 1;
   BITSET_SET(b.shader->info.textures_used, unit);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, operation == 1 ? 1 : 2);
   tex->op = operation == 1 ? nir_texop_txs : operation >= 2 ? nir_texop_txl : nir_texop_txf;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->texture_index = tex->sampler_index = unit;
   tex->coord_components = operation == 1 ? 0 : 2;
   tex->dest_type = operation == 1 ? nir_type_int32 : nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_lod, operation >= 2 ?
      nir_imm_float(&b, level + (operation == 3 ? 0.5 : 0)) : nir_imm_int(&b, level));
   if (operation != 1)
      tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_coord, operation >= 2 ?
         nir_imm_vec2(&b, 0.5, 0.5) : nir_imm_ivec2(&b, 0, 0));
   nir_def_init(&tex->instr, &tex->def, operation == 1 ? 2 : 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_def *value = operation == 1 ? nir_vec4(&b, nir_channel(&b, &tex->def, 0),
      nir_channel(&b, &tex->def, 1), nir_imm_int(&b, 0), nir_imm_int(&b, 1)) : &tex->def;
   nir_def *id = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
   nir_store_ssbo(&b, value, nir_imm_int(&b, 0), nir_imul_imm(&b, id, 16), .write_mask = 15, .align_mul = 16);
   return b.shader;
}

static unsigned count_mip(const uint32_t *words, unsigned operation, unsigned physical_level)
{
   uint32_t expected[4] = {0, 0, 0, 0x3f800000};
   if (operation == 1) {
      expected[0] = 16u >> physical_level;
      expected[1] = 8u >> physical_level;
      expected[3] = 1;
   } else {
      float value = (float)(1u << physical_level) * (operation == 3 ? 1.5f : 1.0f);
      memcpy(expected, &value, 4);
   }
   unsigned correct = 0;
   for (unsigned i = 0; i < 80; ++i)
      correct += words[i] == (i < 8 || i >= 72 ? GUARD_WORD : expected[(i - 8) % 4]);
   return correct;
}

/* operation: fetch, size, then nearest/linear pairs for edge, repeat, mirror. */
static unsigned count_sampled(const uint32_t *words, float sign, unsigned operation, unsigned kind)
{
   unsigned correct = 0;
   for (unsigned i = 0; i < 80; ++i) {
      uint32_t expected = GUARD_WORD;
      if (i >= 8 && i < 72) {
         unsigned lane = (i - 8) % 4, invocation = (i - 8) / 4;
         if (operation == 1) {
            const uint32_t size[4] = {17, 3, 0, 1};
            expected = size[lane];
         } else {
            expected = lane == 0 ? expected_red(kind, invocation + 1, sign) :
               lane == 3 ? (kind == 0 ? UINT32_C(0x3f800000) : 1) : 0;
            if (operation >= 2 && lane == 0) {
               /* Exact coordinates .25/.75 yield texel positions 3.75/12.25. */
               bool high = (invocation & 1) ^ (operation == 4 || operation == 5);
               float value = sign * (high ? (operation & 1 ? 3.0625f : 3.0f) :
                                           (operation & 1 ? 0.9375f : 1.0f));
               memcpy(&expected, &value, 4);
            }
         }
      }
      correct += words[i] == expected;
   }
   return correct;
}

static unsigned count_image(const uint32_t *words, float sign, unsigned kind)
{
   unsigned correct = 0;
   for (unsigned i = 0; i < IMAGE_WORDS; ++i) {
      uint32_t expected = GUARD_WORD;
      if (i >= 65 && i < 81) {
         expected = expected_red(kind, i - 64, sign);
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

static int run_mips(struct pipe_context *pipe, uint32_t *words, size_t bytes)
{
   int status = 1;
   struct pipe_resource *image = NULL;
   struct pipe_sampler_view *views[3] = {0};
   void *states[2] = {0}, *shaders[4][2][4] = {{{0}}};
   const unsigned first[3] = {0, 1, 2}, last[3] = {3, 3, 2};
   const struct pipe_resource templ = {.target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_R32_FLOAT,
      .width0 = 16, .height0 = 8, .depth0 = 1, .array_size = 1, .last_level = 3,
      .bind = PIPE_BIND_SAMPLER_VIEW};
   image = pipe->screen->resource_create(pipe->screen, &templ);
   void *data = NULL; size_t size = 0;
   if (!image || ps5_resource_info(image, &data, &size, NULL) || !data || size != 3840) goto cleanup;
   memset(data, 0xcd, size);
   for (unsigned level = 0; level < 4; ++level) {
      struct pipe_transfer *transfer = NULL;
      uint8_t *mapped = pipe_texture_map(pipe, image, level, 0, PIPE_MAP_WRITE,
         0, 0, 16 >> level, 8 >> level, &transfer);
      if (!mapped) goto cleanup;
      const float value = (float)(1u << level);
      for (unsigned y = 0; y < (8u >> level); ++y) for (unsigned x = 0; x < (16u >> level); ++x)
         memcpy(mapped + y * transfer->stride + x * 4, &value, 4);
      pipe->texture_unmap(pipe, transfer);
   }
   for (unsigned i = 0; i < 3; ++i) {
      struct pipe_sampler_view sv = {.target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_R32_FLOAT,
         .swizzle_r = PIPE_SWIZZLE_X, .swizzle_g = PIPE_SWIZZLE_Y,
         .swizzle_b = PIPE_SWIZZLE_Z, .swizzle_a = PIPE_SWIZZLE_W};
      sv.u.tex.first_level = first[i]; sv.u.tex.last_level = last[i];
      views[i] = pipe->create_sampler_view(pipe, image, &sv);
      if (!views[i]) goto cleanup;
   }
   for (unsigned i = 0; i < 2; ++i) {
      const struct pipe_sampler_state ss = {.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE, .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
         .min_img_filter = PIPE_TEX_FILTER_NEAREST, .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
         .min_mip_filter = i ? PIPE_TEX_MIPFILTER_LINEAR : PIPE_TEX_MIPFILTER_NEAREST, .max_lod = 3};
      states[i] = pipe->create_sampler_state(pipe, &ss);
      if (!states[i]) goto cleanup;
   }
   for (unsigned op = 0; op < 4; ++op) for (unsigned unit = 0; unit < 2; ++unit)
      for (unsigned level = 0; level < (op == 3 ? 1u : 4u); ++level) {
         struct pipe_compute_state cs = {.ir_type = PIPE_SHADER_IR_NIR,
            .prog = compute_mip(op, unit ? 7 : 0, level)};
         shaders[op][unit][level] = pipe->create_compute_state(pipe, &cs);
         if (!shaders[op][unit][level]) goto cleanup;
      }
   unsigned expected_dispatches = 0;
   if (ps5_context_last_compute_status(pipe, &expected_dispatches)) goto cleanup;
   const struct pipe_grid_info grid = {.work_dim = 1, .block = {16, 1, 1}, .grid = {1, 1, 1}};
   for (unsigned view = 0; view < 3; ++view) for (unsigned op = 0; op < 4; ++op)
      for (unsigned unit = 0; unit < 2; ++unit)
         for (unsigned level = 0; level < (op == 3 ? (last[view] > first[view] ? 1u : 0u) : last[view] - first[view] + 1); ++level) {
            const unsigned slot = unit ? 7 : 0;
            pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, slot, 1, 0, &views[view]);
            pipe->bind_sampler_states(pipe, MESA_SHADER_COMPUTE, slot, 1, &states[op == 3]);
            pipe->bind_compute_state(pipe, shaders[op][unit][level]);
            memset(words, 0xcd, bytes);
            pipe->launch_grid(pipe, &grid);
            unsigned dispatches = 0;
            int rc = ps5_context_last_compute_status(pipe, &dispatches);
            unsigned correct = count_mip(words, op, first[view] + level);
            printf("[ps5-compute-mip] view=%u op=%u unit=%u lod=%u rc=%d dispatches=%u words=%u/80 first=%08x,%08x\n",
               view, op, slot, level, rc, dispatches, correct, words[8], words[9]);
            fflush(stdout);
            if (rc || dispatches != ++expected_dispatches || correct != 80) goto cleanup;
         }
   /* Check all uploaded pixels and row padding after sampling. */
   unsigned offset = 0, correct = 0;
   for (unsigned level = 4; level-- > 0;) {
      const float value = (float)(1u << level); uint32_t bits; memcpy(&bits, &value, 4);
      for (unsigned y = 0; y < (8u >> level); ++y) for (unsigned x = 0; x < 64; ++x)
         correct += ((uint32_t *)data)[offset + y * 64 + x] == (x < (16u >> level) ? bits : GUARD_WORD);
      offset += (8u >> level) * 64;
   }
   printf("[ps5-compute-mip] image=%u/960\n", correct);
   status = correct == 960 ? 0 : 1;
cleanup:
   pipe->bind_compute_state(pipe, NULL);
   void *none = NULL;
   pipe->bind_sampler_states(pipe, MESA_SHADER_COMPUTE, 0, 1, &none);
   pipe->bind_sampler_states(pipe, MESA_SHADER_COMPUTE, 7, 1, &none);
   pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 0, 0, 8, NULL);
   for (unsigned i = 0; i < 3; ++i) pipe_sampler_view_reference(&views[i], NULL);
   for (unsigned i = 0; i < 2; ++i) if (states[i]) pipe->delete_sampler_state(pipe, states[i]);
   for (unsigned op = 0; op < 4; ++op) for (unsigned unit = 0; unit < 2; ++unit)
      for (unsigned level = 0; level < 4; ++level)
         if (shaders[op][unit][level]) pipe->delete_compute_state(pipe, shaders[op][unit][level]);
   pipe_resource_reference(&image, NULL);
   return status;
}

int main(void)
{
   int status = 1;
   struct pipe_screen *screen = ps5_screen_create();
   struct pipe_context *pipe = screen ? screen->context_create(screen, NULL, 0) : NULL;
   struct pipe_resource *images[3] = {0}, *target = NULL, *render_pool = NULL;
   struct pipe_resource *sample_output = NULL;
   void *sample_cs[3][4] = {{0}};
   void *filtered_cs[2][2] = {{0}}, *linear_sampler = NULL, *wrap_samplers[2][2] = {{0}};
   struct pipe_sampler_view *views[3] = {0};
   void *writers[3] = {0}, *fragments[3] = {0}, *vs = NULL, *sampler = NULL;
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
   void *image_data[3] = {0};
   for (unsigned kind = 0; kind < 3; ++kind) {
      templ.format = image_formats[kind];
      images[kind] = screen->resource_create(screen, &templ);
      size_t size = 0;
      if (!images[kind] || ps5_resource_info(images[kind], &image_data[kind], &size, NULL) ||
          !image_data[kind] || size != IMAGE_WORDS * 4)
         goto cleanup;
      memset(image_data[kind], 0xcd, size); /* No CPU source writes after initialization. */
   }
   templ.format = PIPE_FORMAT_R8G8B8A8_UNORM;
   templ.width0 = templ.height0 = 8;
   templ.bind = PIPE_BIND_RENDER_TARGET;
   target = screen->resource_create(screen, &templ);
   if (!target)
      goto cleanup;
   struct pipe_compute_state compute = {.ir_type = PIPE_SHADER_IR_NIR};
   struct pipe_shader_state shader = {.type = PIPE_SHADER_IR_NIR, .ir.nir = build_vertex()};
   vs = pipe->create_vs_state(pipe, &shader);
   if (!vs)
      goto cleanup;
   pipe->bind_vs_state(pipe, vs);
   struct pipe_sampler_view sv = {.target = PIPE_TEXTURE_2D,
      .swizzle_r = PIPE_SWIZZLE_X, .swizzle_g = PIPE_SWIZZLE_Y,
      .swizzle_b = PIPE_SWIZZLE_Z, .swizzle_a = PIPE_SWIZZLE_W};
   for (unsigned kind = 0; kind < 3; ++kind) {
      sv.format = image_formats[kind];
      views[kind] = pipe->create_sampler_view(pipe, images[kind], &sv);
      compute.prog = build_compute(kind);
      writers[kind] = pipe->create_compute_state(pipe, &compute);
      shader.ir.nir = build_fragment(kind);
      fragments[kind] = pipe->create_fs_state(pipe, &shader);
      if (!views[kind] || !writers[kind] || !fragments[kind])
         goto cleanup;
   }
   const struct pipe_sampler_state ss = {.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE, .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
      .min_img_filter = PIPE_TEX_FILTER_NEAREST, .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
      .min_mip_filter = PIPE_TEX_MIPFILTER_NONE};
   sampler = pipe->create_sampler_state(pipe, &ss);
   struct pipe_sampler_state linear = ss;
   linear.min_img_filter = linear.mag_img_filter = PIPE_TEX_FILTER_LINEAR;
   linear_sampler = pipe->create_sampler_state(pipe, &linear);
   for (unsigned wrap = 0; wrap < 2; ++wrap) for (unsigned filter = 0; filter < 2; ++filter) {
      struct pipe_sampler_state state = filter ? linear : ss;
      state.wrap_s = state.wrap_t = state.wrap_r = wrap ? PIPE_TEX_WRAP_MIRROR_REPEAT : PIPE_TEX_WRAP_REPEAT;
      wrap_samplers[wrap][filter] = pipe->create_sampler_state(pipe, &state);
      if (!wrap_samplers[wrap][filter]) goto cleanup;
   }
   const struct pipe_blend_state bs = {.rt[0].colormask = PIPE_MASK_RGBA};
   blend = pipe->create_blend_state(pipe, &bs);
   const struct pipe_rasterizer_state rs = {.front_ccw = true,
      .fill_front = PIPE_POLYGON_MODE_FILL, .fill_back = PIPE_POLYGON_MODE_FILL,
      .line_width = 1, .point_size = 1, .depth_clip_near = true, .depth_clip_far = true,
      .half_pixel_center = true};
   rasterizer = pipe->create_rasterizer_state(pipe, &rs);
   const struct pipe_depth_stencil_alpha_state ds = {0};
   depth = pipe->create_depth_stencil_alpha_state(pipe, &ds);
   if (!sampler || !linear_sampler || !blend || !rasterizer || !depth)
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
   for (unsigned kind = 0; kind < 3; ++kind) for (unsigned i = 0; i < 4; ++i) {
      compute.prog = compute_sample(i & 1 ? 3 : kind, i >= 2 ? 7 : 0);
      sample_cs[kind][i] = pipe->create_compute_state(pipe, &compute);
      if (!sample_cs[kind][i])
         goto cleanup;
   }
   for (unsigned outside = 0; outside < 2; ++outside) for (unsigned i = 0; i < 2; ++i) {
      compute.prog = compute_sample(4 + outside, i ? 7 : 0);
      filtered_cs[outside][i] = pipe->create_compute_state(pipe, &compute);
      if (!filtered_cs[outside][i]) goto cleanup;
   }
   pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, 1, &sampler);
   pipe->bind_blend_state(pipe, blend);
   pipe->bind_rasterizer_state(pipe, rasterizer);
   pipe->bind_depth_stencil_alpha_state(pipe, depth);
   const struct pipe_viewport_state vp = {.scale = {4, 4, 0.5}, .translate = {4, 4, 0.5}};
   pipe->set_viewport_states(pipe, 0, 1, &vp);
   const struct pipe_framebuffer_state fb = {.width = 8, .height = 8, .nr_cbufs = 1,
      .cbufs[0] = {.texture = target, .format = PIPE_FORMAT_R8G8B8A8_UNORM}};
   pipe->set_framebuffer_state(pipe, &fb);
   for (unsigned phase = 0; phase < 12; ++phase) {
      const unsigned kind = phase / 4;
      struct pipe_sampler_view *view = views[kind];
      const struct pipe_image_view iv = {.resource = images[kind], .format = image_formats[kind],
         .access = PIPE_IMAGE_ACCESS_WRITE};
      pipe->set_shader_images(pipe, MESA_SHADER_COMPUTE, 0, 1, 0, &iv);
      pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 0, 1, 0, &view);
      pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 7, 1, 0, &view);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 1, 0, &view);
      pipe->bind_fs_state(pipe, fragments[kind]);
      const float sign = phase & 1 ? 1 : -1;
      float constants[4] = {sign, 0, 0, 0};
      const struct pipe_constant_buffer cb = {.user_buffer = constants, .buffer_size = sizeof(constants)};
      pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, 0, &cb);
      memset(constants, 0, sizeof(constants)); /* The constant uploader must own its copy. */
      const struct pipe_grid_info grid = {.work_dim = 1, .block = {16, 1, 1}, .grid = {1, 1, 1}};
      printf("[ps5-compute-render] phase=%u dispatch-start\n", phase);
      fflush(stdout);
      pipe->bind_compute_state(pipe, writers[kind]);
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
         pipe->bind_compute_state(pipe, sample_cs[kind][i]);
         pipe->launch_grid(pipe, &grid);
         unsigned sampled_dispatches = 0;
         int rc = ps5_context_last_compute_status(pipe, &sampled_dispatches);
         unsigned correct = count_sampled(sample_words, sign, i & 1, kind);
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
      unsigned image_ok = count_image(image_data[kind], sign, kind);
      printf("[ps5-compute-render] phase=%u compute=%d/%u draw=%d/%u pixels=%u/64 image=%u/%u\n",
         phase, compute_rc, dispatches, draw_rc, draws, pixel_ok, image_ok, IMAGE_WORDS);
      fflush(stdout);
      if (draw_rc || draws != phase + 1 || pixel_ok != 64 || image_ok != IMAGE_WORDS)
         goto cleanup;
   }
   for (unsigned phase = 0; phase < 2; ++phase) {
      const float sign = phase ? 1 : -1;
      const struct pipe_constant_buffer cb = {.user_buffer = &sign, .buffer_size = sizeof(sign)};
      const struct pipe_image_view iv = {.resource = images[0], .format = image_formats[0],
         .access = PIPE_IMAGE_ACCESS_WRITE};
      const struct pipe_grid_info grid = {.work_dim = 1, .block = {16, 1, 1}, .grid = {1, 1, 1}};
      pipe->set_shader_images(pipe, MESA_SHADER_COMPUTE, 0, 1, 0, &iv);
      pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 0, 1, 0, &views[0]);
      pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 7, 1, 0, &views[0]);
      pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, 0, &cb);
      pipe->bind_compute_state(pipe, writers[0]);
      pipe->launch_grid(pipe, &grid);
      unsigned dispatches = 0;
      if (ps5_context_last_compute_status(pipe, &dispatches) || dispatches != 61 + phase * 13)
         goto cleanup;
      for (unsigned i = 0; i < 12; ++i) {
         const unsigned wrap = i / 4;
         void *state = wrap ? wrap_samplers[wrap - 1][i & 1] : i & 1 ? linear_sampler : sampler;
         const unsigned unit = i % 4 >= 2 ? 7 : 0;
         pipe->bind_sampler_states(pipe, MESA_SHADER_COMPUTE, unit, 1, &state);
         pipe->bind_compute_state(pipe, filtered_cs[wrap != 0][unit != 0]);
         memset(sample_words, 0xcd, sample_bytes);
         pipe->launch_grid(pipe, &grid);
         int rc = ps5_context_last_compute_status(pipe, &dispatches);
         unsigned correct = count_sampled(sample_words, sign, 2 + wrap * 2 + (i & 1), 0);
         printf("[ps5-compute-filtered] phase=%u case=%u rc=%d dispatches=%u words=%u/80 first=%08x next=%08x\n",
            phase, i, rc, dispatches, correct, sample_words[8], sample_words[12]);
         fflush(stdout);
         if (rc || dispatches != 62 + phase * 13 + i || correct != 80) goto cleanup;
      }
      unsigned image_ok = count_image(image_data[0], sign, 0);
      printf("[ps5-compute-filtered] phase=%u image=%u/%u\n", phase, image_ok, IMAGE_WORDS);
      if (image_ok != IMAGE_WORDS) goto cleanup;
   }
   if (run_mips(pipe, sample_words, sample_bytes)) goto cleanup;
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
      pipe->bind_sampler_states(pipe, MESA_SHADER_COMPUTE, 0, 1, &none);
      pipe->bind_sampler_states(pipe, MESA_SHADER_COMPUTE, 7, 1, &none);
      pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, 0, 1, NULL);
      pipe->set_sampler_views(pipe, MESA_SHADER_COMPUTE, 0, 0, 8, NULL);
      for (unsigned kind = 0; kind < 3; ++kind) {
         pipe_sampler_view_reference(&views[kind], NULL);
         if (writers[kind]) pipe->delete_compute_state(pipe, writers[kind]);
         if (fragments[kind]) pipe->delete_fs_state(pipe, fragments[kind]);
         for (unsigned i = 0; i < 4; ++i)
            if (sample_cs[kind][i]) pipe->delete_compute_state(pipe, sample_cs[kind][i]);
      }
      if (vs) pipe->delete_vs_state(pipe, vs);
      if (sampler) pipe->delete_sampler_state(pipe, sampler);
      if (linear_sampler) pipe->delete_sampler_state(pipe, linear_sampler);
      for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 2; ++j) {
         if (filtered_cs[i][j]) pipe->delete_compute_state(pipe, filtered_cs[i][j]);
         if (wrap_samplers[i][j]) pipe->delete_sampler_state(pipe, wrap_samplers[i][j]);
      }
      if (blend) pipe->delete_blend_state(pipe, blend);
      if (rasterizer) pipe->delete_rasterizer_state(pipe, rasterizer);
      if (depth) pipe->delete_depth_stencil_alpha_state(pipe, depth);
      pipe->destroy(pipe);
   }
   for (unsigned kind = 0; kind < 3; ++kind)
      pipe_resource_reference(&images[kind], NULL);
   pipe_resource_reference(&target, NULL);
   pipe_resource_reference(&sample_output, NULL);
   pipe_resource_reference(&render_pool, NULL);
   if (screen) screen->destroy(screen);
   psbc_shutdown();
   printf("[ps5-compute-render] result=%d\n", status);
   return status;
}
