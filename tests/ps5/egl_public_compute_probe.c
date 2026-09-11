// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

/* Internal native compute bring-up in the existing folder-app test runner.
 * The catalog prefix does not imply a public GL compute extension/context. */
#include <stdio.h>
#include <string.h>
#include "compiler/nir/nir_builder.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "ps5_screen.h"
#include "ps5_agc_package.h"

#define OUTPUT_WORDS 512
#define INPUT_WORDS (15 * 16)
#define IMAGE_WORDS (64 * 3) /* 17x3 R32_UINT with a 256-byte linear row. */
#define GUARD_WORD UINT32_C(0xcdcdcdcd)
#ifdef PS5_COMPUTE_PIPE_PROBE
#define OUTPUT_BINDING 15
#define PREFIX_WORDS 32
#else
#define OUTPUT_BINDING 0
#define PREFIX_WORDS 0
#endif
enum { CONTROL, GRID, SHARED, ATOMIC, FP32, FP64, BUFFER_RANGES, BUFFER_ALIAS,
       POST_BUFFER, UBO_RANGES, UBO_COPY, POST_UBO, INDIRECT_ARGS, INDIRECT,
       POST_INDIRECT, IMAGE_STORE, IMAGE_LOAD, IMAGE_SIZE, POST_IMAGE,
       SCRATCH, SCRATCH_GRID, POST_SCRATCH };
static const struct {
   const char *name;
   uint32_t local[3], groups[3];
   unsigned words;
} cases[] = {
   {"control", {16, 1, 1}, {1, 1, 1}, 16},
   {"grid-3d", {4, 2, 2}, {2, 3, 2}, 192},
   {"shared-cross-wave", {64, 1, 1}, {1, 1, 1}, 64},
   {"atomic-multi-group", {64, 1, 1}, {4, 1, 1}, 257},
   {"fp32-control", {16, 1, 1}, {1, 1, 1}, 16},
   {"fp64-precision-store", {16, 1, 1}, {1, 1, 1}, 32},
   {"buffer-ranges-15", {4, 1, 1}, {15, 1, 1}, 60},
   {"buffer-alias-consumer", {4, 1, 1}, {15, 1, 1}, 60},
   {"post-buffer-control", {16, 1, 1}, {1, 1, 1}, 16},
   {"ubo-ssbo-ranges-31", {4, 1, 1}, {15, 1, 1}, 60},
   {"ubo-copied-constants", {16, 1, 1}, {1, 1, 1}, 16},
   {"post-ubo-control", {16, 1, 1}, {1, 1, 1}, 16},
   {"indirect-arguments", {3, 1, 1}, {1, 1, 1}, 3},
   {"indirect-grid-consumer", {4, 2, 2}, {2, 3, 2}, 192},
   {"post-indirect-control", {16, 1, 1}, {1, 1, 1}, 16},
   {"image-store-r32ui", {16, 1, 1}, {1, 1, 1}, 16},
   {"image-load-r32ui", {16, 1, 1}, {1, 1, 1}, 16},
   {"image-size-r32ui", {16, 1, 1}, {1, 1, 1}, 16},
   {"post-image-control", {16, 1, 1}, {1, 1, 1}, 16},
   {"scratch-private-16", {16, 1, 1}, {1, 1, 1}, 16},
   {"scratch-private-128-grid", {64, 1, 1}, {4, 1, 1}, 256},
   {"post-scratch-control", {16, 1, 1}, {1, 1, 1}, 16},
};

static nir_shader *create_probe_shader(unsigned test)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      psbc_get_nir_options(PSBC_STAGE_COMPUTE), cases[test].name);
   for (unsigned axis = 0; axis < 3; ++axis)
      b.shader->info.workgroup_size[axis] = cases[test].local[axis];
   b.shader->info.num_ssbos = 16;
   b.shader->info.num_ubos = test == UBO_RANGES ? 15 : test == UBO_COPY ? 1 : 0;
   b.shader->info.num_images = test >= IMAGE_STORE && test <= IMAGE_SIZE ? 8 : 0;
   nir_def *local = nir_load_local_invocation_id(&b);
   nir_def *id = nir_channel(&b, local, 0);
   nir_def *value;
   if (test == FP32 || test == FP64) {
      const unsigned bits = test == FP64 ? 64 : 32;
      b.fp_math_ctrl = nir_fp_no_fast_math;
      nir_def *n = nir_iadd_imm(&b, id, 1);
      nir_def *x = test == FP64 ? nir_u2f64(&b, n) : nir_u2f32(&b, n);
      x = nir_fmul(&b, x, nir_imm_floatN_t(&b, 0.25, bits));
      nir_def *large = nir_imm_floatN_t(&b, 1099511627776.0, bits); /* 2^40 */
      value = nir_fmul(&b, nir_fsub(&b, nir_fadd(&b, large, x), large), x);
   } else if (test >= IMAGE_STORE && test <= IMAGE_SIZE) {
      nir_def *zero = nir_imm_int(&b, 0), *slot = nir_imm_int(&b, 7);
      nir_def *coord = nir_vec4(&b, nir_iadd_imm(&b, id, 1), nir_imm_int(&b, 1), zero, zero);
      value = nir_iadd_imm(&b, nir_imul_imm(&b, id, 3), 17);
      if (test == IMAGE_STORE)
         nir_image_store(&b, slot, coord, zero, nir_vec4(&b, value, zero, zero, zero), zero,
            .image_dim = GLSL_SAMPLER_DIM_2D, .format = PIPE_FORMAT_R32_UINT, .src_type = nir_type_uint32);
      else if (test == IMAGE_LOAD)
         value = nir_iadd_imm(&b, nir_channel(&b, nir_image_load(&b, 4, 32, slot, coord, zero, zero,
            .image_dim = GLSL_SAMPLER_DIM_2D, .format = PIPE_FORMAT_R32_UINT, .dest_type = nir_type_uint32), 0), 100);
      else {
         nir_def *size = nir_image_size(&b, 2, 32, slot, zero,
            .image_dim = GLSL_SAMPLER_DIM_2D, .format = PIPE_FORMAT_R32_UINT);
         value = nir_iadd(&b, nir_channel(&b, size, 0), nir_imul_imm(&b, nir_channel(&b, size, 1), 100));
      }
   } else if (test == UBO_COPY) {
      value = nir_load_ubo(&b, 1, 32, nir_imm_int(&b, 0), nir_imul_imm(&b, id, 4),
         .align_mul = 4, .range = 64);
   } else if (test == BUFFER_RANGES || test == BUFFER_ALIAS || test == UBO_RANGES) {
      nir_def *group = nir_channel(&b, nir_load_workgroup_id(&b), 0);
      nir_def *global = nir_iadd(&b, id, nir_imul_imm(&b, group, 4));
      /* Workgroup-uniform dynamic block index; all fifteen ranges are live. */
      nir_def *slot = test != BUFFER_ALIAS ? group : nir_imm_int(&b, 0);
      slot = nir_iadd_imm(&b, slot, OUTPUT_BINDING == 0 ? 1 : 0);
      nir_def *offset = nir_imul_imm(&b, test != BUFFER_ALIAS ? id : global, 4);
      value = nir_load_ssbo(&b, 1, 32, slot, offset, .align_mul = 4);
      if (test == UBO_RANGES) {
         nir_def *constant = nir_load_ubo(&b, 1, 32, group, offset,
            .align_mul = 4, .range = 16);
         value = nir_iadd(&b, constant, nir_imul_imm(&b, value, 3));
      }
      if (test == BUFFER_ALIAS)
         value = nir_iadd_imm(&b, nir_imul_imm(&b, value, 5), 2);
      id = global;
   } else if (test == INDIRECT_ARGS) {
      value = nir_bcsel(&b, nir_ieq_imm(&b, id, 1), nir_imm_int(&b, 3), nir_imm_int(&b, 2));
   } else if (test == GRID || test == INDIRECT) {
      nir_def *group = nir_load_workgroup_id(&b);
      nir_def *count = nir_load_num_workgroups(&b);
      nir_def *global[3];
      for (unsigned axis = 0; axis < 3; ++axis)
         global[axis] = nir_iadd(&b, nir_channel(&b, local, axis),
            nir_imul_imm(&b, nir_channel(&b, group, axis), cases[test].local[axis]));
      id = nir_iadd(&b, global[0], nir_iadd(&b,
         nir_imul_imm(&b, global[1], 8), nir_imul_imm(&b, global[2], 48)));
      value = nir_iadd_imm(&b, nir_imul_imm(&b, id, 3), 17);
      const unsigned weights[3] = {7, 11, 13};
      for (unsigned axis = 0; axis < 3; ++axis)
         value = nir_iadd(&b, value,
            nir_imul_imm(&b, nir_channel(&b, count, axis), weights[axis]));
   } else if (test == SCRATCH || test == SCRATCH_GRID) {
      const unsigned bytes = test == SCRATCH ? 16 : 128;
      b.shader->scratch_size = bytes;
      id = nir_iadd(&b, id, nir_imul_imm(&b,
         nir_channel(&b, nir_load_workgroup_id(&b), 0), cases[test].local[0]));
      nir_def *offset = nir_imul_imm(&b, nir_iand_imm(&b, id, bytes / 4 - 1), 4);
      value = nir_iadd_imm(&b, nir_imul_imm(&b, id, 3), 17);
      nir_store_scratch(&b, value, offset, .align_mul = 4, .write_mask = 1);
      value = nir_load_scratch(&b, 1, 32, offset, .align_mul = 4);
   } else if (test == ATOMIC) {
      id = nir_iadd(&b, id, nir_imul_imm(&b,
         nir_channel(&b, nir_load_workgroup_id(&b), 0), 64));
      value = nir_ssbo_atomic(&b, 32, nir_imm_int(&b, OUTPUT_BINDING), nir_imm_int(&b, 256 * 4),
         nir_imm_int(&b, 1), .atomic_op = nir_atomic_op_iadd);
   } else {
      value = nir_iadd_imm(&b, nir_imul_imm(&b, id, 3), 17);
      if (test == SHARED) {
         b.shader->info.shared_size = 256;
         nir_store_shared(&b, value, nir_imul_imm(&b, id, 4), .align_mul = 4, .write_mask = 1);
         nir_barrier(&b, .execution_scope = SCOPE_WORKGROUP, .memory_scope = SCOPE_WORKGROUP,
            .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_mem_shared);
         value = nir_load_shared(&b, 1, 32,
            nir_imul_imm(&b, nir_ixor(&b, id, nir_imm_int(&b, 32)), 4), .align_mul = 4);
      }
   }
   const unsigned stride = value->bit_size / 8;
   nir_store_ssbo(&b, value, nir_imm_int(&b, OUTPUT_BINDING), nir_imul_imm(&b, id, stride),
      .align_mul = stride, .write_mask = 1);
   nir_validate_shader(b.shader, "native compute probe");
   return b.shader;
}

static unsigned count_correct(unsigned test, const uint32_t *output)
{
   unsigned correct = 0;
   bool seen[256] = {false};
   for (unsigned i = 0; i < cases[test].words; ++i) {
      if (test == ATOMIC && i < 256) {
         const uint32_t value = output[i];
         if (value < 256 && !seen[value]) {
            seen[value] = true;
            ++correct;
         }
      } else {
         uint32_t expected = 17 + 3 * (test == SHARED ? (i ^ 32) : i);
         if (test == GRID || test == INDIRECT)
            expected += 7 * 2 + 11 * 3 + 13 * 2;
         if (test == INDIRECT_ARGS)
            expected = i == 1 ? 3 : 2;
         if (test == IMAGE_LOAD)
            expected += 100;
         if (test == IMAGE_SIZE)
            expected = 317;
         if (test == BUFFER_RANGES || test == BUFFER_ALIAS) {
            expected = 1000 + 101 * (i / 4) + 7 * (i % 4);
            if (test == BUFFER_ALIAS)
               expected = 5 * expected + 2;
         }
         if (test == UBO_RANGES)
            expected = 5414 + 202 * (i / 4) + 28 * (i % 4);
         if (test == UBO_COPY)
            expected = 700 + 11 * i;
         if (test == ATOMIC)
            expected = 256;
         if (test == FP32)
            expected = 0; /* FP32 loses x when it is added to 2^40. */
         if (test == FP64) {
            const double n = i / 2 + 1;
            const double square = n * n / 16.0; /* Dyadic, exactly representable. */
            uint64_t bits;
            memcpy(&bits, &square, sizeof(bits));
            expected = bits >> (32 * (i & 1));
         }
         correct += output[i] == expected;
      }
   }
   return correct;
}

static unsigned count_image_correct(const uint32_t *pixels)
{
   unsigned correct = 0;
   for (unsigned i = 0; i < IMAGE_WORDS; ++i) {
      const uint32_t expected = i >= 65 && i < 81 ? 17 + 3 * (i - 65) : GUARD_WORD;
      correct += pixels[i] == expected;
   }
   return correct;
}

int main(void)
{
   int status = 1;
   struct pipe_screen *screen = ps5_screen_create();
   struct pipe_resource *buffers[4] = {0};
   PsbcShaderOutput compiled = {0};
   if (!screen)
      return 1;
#ifdef PS5_COMPUTE_PIPE_PROBE
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   void *state = NULL;
   if (!pipe || !pipe->create_compute_state || !pipe->launch_grid ||
       !pipe->set_shader_buffers || !pipe->set_constant_buffer || !pipe->set_shader_images)
      goto cleanup;
#endif
   struct pipe_resource templ = {
      .target = PIPE_BUFFER, .format = PIPE_FORMAT_R8_UNORM,
      .width0 = 256, .height0 = 1, .depth0 = 1, .array_size = 1,
      .usage = PIPE_USAGE_DEFAULT, .bind = PIPE_BIND_SHADER_BUFFER,
   };
   void *table = NULL, *output = NULL, *input_data = NULL;
   for (unsigned i = 0; i < 3; ++i) {
      templ.width0 = i == 2 ? (INPUT_WORDS + 16) * sizeof(uint32_t) :
         i == 1 ? (OUTPUT_WORDS + PREFIX_WORDS) * sizeof(uint32_t) : 31 * 16 + 8 * 32;
      buffers[i] = screen->resource_create(screen, &templ);
      if (!buffers[i])
         goto cleanup;
   }
   templ = (struct pipe_resource){
      .target = PIPE_TEXTURE_2D, .format = PIPE_FORMAT_R32_UINT,
      .width0 = 17, .height0 = 3, .depth0 = 1, .array_size = 1,
      .bind = PIPE_BIND_SHADER_IMAGE | PIPE_BIND_SAMPLER_VIEW,
   };
   struct pipe_resource *image_buffer = buffers[3] = screen->resource_create(screen, &templ);
   void *image_data = NULL;
   size_t image_size = 0;
   if (!image_buffer || ps5_resource_info(image_buffer, &image_data, &image_size, NULL) ||
       !image_data || image_size != IMAGE_WORDS * sizeof(uint32_t))
      goto cleanup;
   memset(image_data, 0xcd, image_size);
   if (ps5_resource_info(buffers[0], &table, NULL, NULL) ||
       ps5_resource_info(buffers[1], &output, NULL, NULL) ||
       ps5_resource_info(buffers[2], &input_data, NULL, NULL))
      goto cleanup;
   uint32_t *prefix = output;
   output = prefix + PREFIX_WORDS;
   struct pipe_resource *output_buffer = buffers[1];
   uint32_t *input = input_data;
   memset(input + INPUT_WORDS, 0, 16 * sizeof(uint32_t));
   for (unsigned i = 0; i < INPUT_WORDS; ++i)
      input[i] = i % 16 >= 4 && i % 16 < 8 ?
         1000 + 101 * (i / 16) + 7 * (i % 16 - 4) : GUARD_WORD;
#ifndef PS5_COMPUTE_PIPE_PROBE
   PsbcCompileOptions opts = {
      .target = PSBC_TARGET_PS5, .stage = PSBC_STAGE_COMPUTE,
      .optimise = true, .address32_hi = (uintptr_t)table >> 32,
      .gallium_buffer_arrays = true, .descriptor_binding_count = 3,
      .descriptor_bindings = {{
         .binding = PSBC_GALLIUM_SSBO_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
         .type = PSBC_DESCRIPTOR_STORAGE_BUFFER, .array_size = 16, .stride = 16,
      }, {
         .binding = PSBC_GALLIUM_UBO_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
         .type = PSBC_DESCRIPTOR_UNIFORM_BUFFER, .array_size = 15, .stride = 16,
         .offset = 16 * 16,
      }, {
         .binding = PSBC_GALLIUM_IMAGE_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
         .type = PSBC_DESCRIPTOR_STORAGE_IMAGE, .array_size = 8, .stride = 32,
         .offset = 31 * 16,
      }},
   };
#endif
   /* Scratch cases remain in the host compile/rejection checks, never this
    * native batch. The shared package guard must stay enabled until fixed. */
   for (unsigned test = 0; test < SCRATCH; ++test) {
      status = 1;
      if (test != BUFFER_ALIAS && test != INDIRECT) /* Keep the GPU producer's result. */
         memset(prefix, 0xcd, (OUTPUT_WORDS + PREFIX_WORDS) * sizeof(uint32_t));
      if (test == ATOMIC)
         ((uint32_t *)output)[256] = 0;
      struct pipe_shader_buffer bindings[31] = {0};
      uint32_t user_constants[16];
      for (unsigned i = 0; i < ARRAY_SIZE(user_constants); ++i)
         user_constants[i] = 700 + 11 * i;
      bindings[OUTPUT_BINDING] = (struct pipe_shader_buffer){
         output_buffer, PREFIX_WORDS * sizeof(uint32_t), cases[test].words * sizeof(uint32_t)
      };
      if (test == BUFFER_RANGES || test == UBO_RANGES) {
         for (unsigned i = 0; i < 15; ++i)
            bindings[i + (OUTPUT_BINDING == 0)] = (struct pipe_shader_buffer){
               buffers[2], (i * 16 + 4) * sizeof(uint32_t), 4 * sizeof(uint32_t)
            };
      } else if (test == BUFFER_ALIAS) {
         bindings[OUTPUT_BINDING == 0 ? 1 : 0] = bindings[OUTPUT_BINDING];
      }
      if (test == UBO_RANGES) {
         for (unsigned i = 0; i < 15; ++i)
            bindings[16 + i] = (struct pipe_shader_buffer){
               buffers[2], ((14 - i) * 16 + 4) * sizeof(uint32_t), 4 * sizeof(uint32_t)
            };
      } else if (test == UBO_COPY) {
         bindings[16] = (struct pipe_shader_buffer){
            buffers[2], INPUT_WORDS * sizeof(uint32_t), sizeof(user_constants)
         };
      }
      psbc_init();
      nir_shader *nir = create_probe_shader(test);
#ifdef PS5_COMPUTE_PIPE_PROBE
      struct pipe_compute_state cso = {.ir_type = PIPE_SHADER_IR_NIR, .prog = nir};
      state = pipe->create_compute_state(pipe, &cso); /* Consumes nir. */
      psbc_shutdown();
      if (!state)
         goto cleanup;
      struct pipe_compute_state_object_info info = {0};
      pipe->get_compute_state_info(pipe, state, &info);
      printf("[ps5-compute] case=%s pipe-created=1 wave=%u private=%u\n",
         cases[test].name, info.preferred_simd_size, info.private_memory);
      fflush(stdout);
      if (info.preferred_simd_size != 32 || info.private_memory)
         goto cleanup;
      pipe->bind_compute_state(pipe, state);
      if (test == IMAGE_STORE) {
         const struct pipe_image_view view = {.resource = image_buffer,
            .format = PIPE_FORMAT_R32_UINT, .access = PIPE_IMAGE_ACCESS_READ_WRITE};
         pipe->set_shader_images(pipe, MESA_SHADER_COMPUTE, 7, 1, 0, &view);
      }
      pipe->set_shader_buffers(pipe, MESA_SHADER_COMPUTE, 0, 16, bindings, 1u << OUTPUT_BINDING);
      for (unsigned i = 0; i < 15; ++i) {
         const struct pipe_shader_buffer *bound = &bindings[16 + i];
         struct pipe_constant_buffer cb = {
            .buffer = bound->buffer, .buffer_offset = bound->buffer_offset,
            .buffer_size = bound->buffer_size,
         };
         if (test == UBO_COPY && i == 0)
            cb = (struct pipe_constant_buffer){
               .user_buffer = user_constants, .buffer_size = sizeof(user_constants)
            };
         pipe->set_constant_buffer(pipe, MESA_SHADER_COMPUTE, i, &cb);
      }
      memset(user_constants, 0, sizeof(user_constants)); /* Upload must have copied. */
      /* The binding now owns the last reference; rebinding must preserve it. */
      pipe_resource_reference(&buffers[1], NULL);
      struct pipe_grid_info grid = {.work_dim = 3};
      memcpy(grid.block, cases[test].local, sizeof(grid.block));
      memcpy(grid.grid, cases[test].groups, sizeof(grid.grid));
      unsigned dispatches = 0;
      if (test == CONTROL) {
         ++grid.block[0];
         pipe->launch_grid(pipe, &grid);
         if (ps5_context_last_compute_status(pipe, &dispatches) >= 0 || dispatches)
            goto cleanup;
         --grid.block[0];
         grid.grid[0] = 0;
         pipe->launch_grid(pipe, &grid);
         if (ps5_context_last_compute_status(pipe, &dispatches) || dispatches)
            goto cleanup;
         grid.grid[0] = cases[test].groups[0];
         grid.indirect = buffers[2];
         grid.indirect_offset = INPUT_WORDS * sizeof(uint32_t);
         pipe->launch_grid(pipe, &grid);
         if (ps5_context_last_compute_status(pipe, &dispatches) || dispatches)
            goto cleanup;
         grid.indirect = NULL;
         printf("[ps5-compute] pipe rejected-block/empty-direct/empty-indirect checks passed\n");
      }
      if (test == INDIRECT) {
         memset(grid.grid, 0, sizeof(grid.grid)); /* Must read the GPU-written command. */
         grid.indirect = output_buffer;
         grid.indirect_offset = PREFIX_WORDS * sizeof(uint32_t);
      }
      pipe->launch_grid(pipe, &grid);
      int rc = ps5_context_last_compute_status(pipe, &dispatches);
      if (dispatches != test + 1)
         rc = -1;
#else
      memcpy(input + INPUT_WORDS, user_constants, sizeof(user_constants));
      memset(user_constants, 0, sizeof(user_constants));
      memset(table, 0, 31 * 16 + 8 * 32);
      if (test >= IMAGE_STORE && ps5_resource_storage_image_descriptor(image_buffer,
            (uint32_t *)table + 31 * 4 + 7 * 8))
         goto cleanup;
      for (unsigned i = 0; i < ARRAY_SIZE(bindings); ++i) {
         const struct pipe_shader_buffer *bound = &bindings[i];
         if (!bound->buffer)
            continue;
         const uintptr_t address = (uintptr_t)(bound->buffer == output_buffer ?
            (void *)prefix : input_data) + bound->buffer_offset;
         uint32_t *srd = (uint32_t *)table + i * 4;
         srd[0] = address;
         srd[1] = address >> 32;
         srd[2] = bound->buffer_size;
         srd[3] = UINT32_C(0x31016fac);
      }
      PsbcResult compile_rc = psbc_compile_nir(nir, &opts, &compiled);
      ralloc_free(nir);
      psbc_shutdown();
      printf("[ps5-compute] case=%s compiled rc=%d code=%zu wave=%u lds=%u grid=%u scratch=%u\n",
         cases[test].name, compile_rc, compiled.machine_code_size,
         compiled.metadata.compute_wave_size, compiled.metadata.compute_lds_bytes,
         compiled.metadata.compute_grid_size_valid, compiled.metadata.scratch_bytes_per_wave);
      fflush(stdout);
      if (compile_rc != PSBC_RESULT_OK || (test == SHARED && compiled.metadata.compute_wave_size != 32))
         goto cleanup;
      uint32_t direct_groups[3];
      memcpy(direct_groups, test == INDIRECT ? output : cases[test].groups, sizeof(direct_groups));
      int rc = ps5_agc_compute_execute(screen, &compiled, buffers[0], buffers + 1, 3, direct_groups);
#endif
      unsigned correct = count_correct(test, output), guards = 0;
      for (unsigned i = cases[test].words; i < OUTPUT_WORDS; ++i)
         guards += ((uint32_t *)output)[i] == GUARD_WORD;
      status = rc || correct != cases[test].words || guards != OUTPUT_WORDS - cases[test].words;
      unsigned prefix_correct = 0;
      for (unsigned i = 0; i < PREFIX_WORDS; ++i)
         prefix_correct += prefix[i] == GUARD_WORD;
      status |= prefix_correct != PREFIX_WORDS;
      unsigned input_correct = 0;
      for (unsigned i = 0; i < INPUT_WORDS; ++i) {
         const uint32_t expected = i % 16 >= 4 && i % 16 < 8 ?
            1000 + 101 * (i / 16) + 7 * (i % 16 - 4) : GUARD_WORD;
         input_correct += input[i] == expected;
      }
      status |= input_correct != INPUT_WORDS;
      if (test >= IMAGE_STORE) {
         unsigned image_correct = count_image_correct(image_data);
         status |= image_correct != IMAGE_WORDS;
         printf("[ps5-compute] image-pixels-and-padding=%u/%u status=%d\n", image_correct, IMAGE_WORDS, status);
      }
      printf("[ps5-compute] case=%s dispatch=%d correct=%u/%u guards=%u/%u first=%u last=%u status=%d\n",
         cases[test].name, rc, correct, cases[test].words, guards, OUTPUT_WORDS - cases[test].words,
         ((uint32_t *)output)[0], ((uint32_t *)output)[cases[test].words - 1], status);
      fflush(stdout);
      printf("[ps5-compute] input-and-guards=%u/%u\n", input_correct, INPUT_WORDS);
#ifdef PS5_COMPUTE_PIPE_PROBE
      printf("[ps5-compute] pipe dispatches=%u slot=%u prefix=%u/%u status=%d\n",
         dispatches, OUTPUT_BINDING, prefix_correct, PREFIX_WORDS, status);
      pipe->delete_compute_state(pipe, state);
      state = NULL;
#endif
      if (status)
         goto cleanup;
#ifdef PS5_COMPUTE_PIPE_PROBE
      if (test == IMAGE_STORE)
         pipe_resource_reference(&buffers[3], NULL); /* Validated binding owns the later reads. */
#endif
      psbc_free_output(&compiled);
      memset(&compiled, 0, sizeof(compiled));
   }
cleanup:
#ifdef PS5_COMPUTE_PIPE_PROBE
   if (pipe) {
      if (state)
         pipe->delete_compute_state(pipe, state);
      pipe->destroy(pipe);
   }
#endif
   psbc_free_output(&compiled);
   for (unsigned i = 0; i < ARRAY_SIZE(buffers); ++i)
      pipe_resource_reference(&buffers[i], NULL);
   screen->destroy(screen);
   printf("[ps5-compute] completed status=%d (internal probe; GL caps unchanged; scratch excluded)\n", status);
   return status;
}
