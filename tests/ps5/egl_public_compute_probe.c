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
#define GUARD_WORD UINT32_C(0xcdcdcdcd)
#ifdef PS5_COMPUTE_PIPE_PROBE
#define OUTPUT_BINDING 15
#define PREFIX_WORDS 32
#else
#define OUTPUT_BINDING 0
#define PREFIX_WORDS 0
#endif
enum { CONTROL, GRID, SHARED, ATOMIC, FP32, FP64 };
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
};

static nir_shader *create_probe_shader(unsigned test)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      psbc_get_nir_options(PSBC_STAGE_COMPUTE), cases[test].name);
   for (unsigned axis = 0; axis < 3; ++axis)
      b.shader->info.workgroup_size[axis] = cases[test].local[axis];
   b.shader->info.num_ssbos = 16;
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
   } else if (test == GRID) {
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
         if (test == GRID)
            expected += 7 * 2 + 11 * 3 + 13 * 2;
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

int main(void)
{
   int status = 1;
   struct pipe_screen *screen = ps5_screen_create();
   struct pipe_resource *buffers[2] = {0};
   PsbcShaderOutput compiled = {0};
   if (!screen)
      return 1;
#ifdef PS5_COMPUTE_PIPE_PROBE
   struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
   void *state = NULL;
   if (!pipe || !pipe->create_compute_state || !pipe->launch_grid || !pipe->set_shader_buffers)
      goto cleanup;
#endif
   struct pipe_resource templ = {
      .target = PIPE_BUFFER, .format = PIPE_FORMAT_R8_UNORM,
      .width0 = 256, .height0 = 1, .depth0 = 1, .array_size = 1,
      .usage = PIPE_USAGE_DEFAULT, .bind = PIPE_BIND_SHADER_BUFFER,
   };
   void *table = NULL, *output = NULL;
   for (unsigned i = 0; i < 2; ++i) {
      templ.width0 = i ? (OUTPUT_WORDS + PREFIX_WORDS) * sizeof(uint32_t) : 256;
      buffers[i] = screen->resource_create(screen, &templ);
      if (!buffers[i])
         goto cleanup;
   }
   if (ps5_resource_info(buffers[0], &table, NULL, NULL) ||
       ps5_resource_info(buffers[1], &output, NULL, NULL))
      goto cleanup;
   uint32_t *prefix = output;
   output = prefix + PREFIX_WORDS;
#ifdef PS5_COMPUTE_PIPE_PROBE
   struct pipe_resource *output_buffer = buffers[1];
#endif
   memset(table, 0, 256);
   uint32_t *srd = table;
   srd[0] = (uintptr_t)output;
   srd[1] = (uintptr_t)output >> 32;
   srd[3] = UINT32_C(0x31016fac);
#ifndef PS5_COMPUTE_PIPE_PROBE
   PsbcCompileOptions opts = {
      .target = PSBC_TARGET_PS5, .stage = PSBC_STAGE_COMPUTE,
      .optimise = true, .address32_hi = (uintptr_t)table >> 32,
      .gallium_buffer_arrays = true, .descriptor_binding_count = 1,
      .descriptor_bindings = {{
         .binding = PSBC_GALLIUM_SSBO_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
         .type = PSBC_DESCRIPTOR_STORAGE_BUFFER, .array_size = 16, .stride = 16,
      }},
   };
#endif
   for (unsigned test = 0; test < ARRAY_SIZE(cases); ++test) {
      status = 1;
      memset(prefix, 0xcd, (OUTPUT_WORDS + PREFIX_WORDS) * sizeof(uint32_t));
      if (test == ATOMIC)
         ((uint32_t *)output)[256] = 0;
      srd[2] = cases[test].words * sizeof(uint32_t);
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
      printf("[ps5-compute] case=%s pipe-created=1 wave=%u\n", cases[test].name, info.preferred_simd_size);
      fflush(stdout);
      if (info.preferred_simd_size != 32)
         goto cleanup;
      pipe->bind_compute_state(pipe, state);
      struct pipe_shader_buffer binding = {
         output_buffer, PREFIX_WORDS * sizeof(uint32_t), cases[test].words * sizeof(uint32_t)
      };
      pipe->set_shader_buffers(pipe, MESA_SHADER_COMPUTE, OUTPUT_BINDING, 1, &binding, 1);
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
         printf("[ps5-compute] pipe rejected-block/empty-grid checks passed\n");
      }
      pipe->launch_grid(pipe, &grid);
      int rc = ps5_context_last_compute_status(pipe, &dispatches);
      if (dispatches != test + 1)
         rc = -1;
#else
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
      int rc = ps5_agc_compute_execute(screen, &compiled, buffers[0], buffers + 1, 1, cases[test].groups);
#endif
      unsigned correct = count_correct(test, output), guards = 0;
      for (unsigned i = cases[test].words; i < OUTPUT_WORDS; ++i)
         guards += ((uint32_t *)output)[i] == GUARD_WORD;
      status = rc || correct != cases[test].words || guards != OUTPUT_WORDS - cases[test].words;
      unsigned prefix_correct = 0;
      for (unsigned i = 0; i < PREFIX_WORDS; ++i)
         prefix_correct += prefix[i] == GUARD_WORD;
      status |= prefix_correct != PREFIX_WORDS;
      printf("[ps5-compute] case=%s dispatch=%d correct=%u/%u guards=%u/%u first=%u last=%u status=%d\n",
         cases[test].name, rc, correct, cases[test].words, guards, OUTPUT_WORDS - cases[test].words,
         ((uint32_t *)output)[0], ((uint32_t *)output)[cases[test].words - 1], status);
      fflush(stdout);
#ifdef PS5_COMPUTE_PIPE_PROBE
      printf("[ps5-compute] pipe dispatches=%u slot=%u prefix=%u/%u status=%d\n",
         dispatches, OUTPUT_BINDING, prefix_correct, PREFIX_WORDS, status);
      pipe->delete_compute_state(pipe, state);
      state = NULL;
#endif
      if (status)
         goto cleanup;
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
   for (unsigned i = 0; i < 2; ++i)
      pipe_resource_reference(&buffers[i], NULL);
   screen->destroy(screen);
   printf("[ps5-compute] completed status=%d (internal probe; GL caps unchanged)\n", status);
   return status;
}
