// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

/* Internal native compute bring-up in the existing folder-app test runner.
 * The catalog prefix does not imply a public GL compute extension/context. */
#include <stdio.h>
#include <string.h>
#include "compiler/nir/nir_builder.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "ps5_screen.h"
#include "ps5_agc_package.h"

#define OUTPUT_WORDS 512
#define GUARD_WORD UINT32_C(0xcdcdcdcd)
enum { CONTROL, GRID, SHARED, ATOMIC };
static const struct {
   const char *name;
   uint32_t local[3], groups[3];
   unsigned words;
} cases[] = {
   {"control", {16, 1, 1}, {1, 1, 1}, 16},
   {"grid-3d", {4, 2, 2}, {2, 3, 2}, 192},
   {"shared-cross-wave", {64, 1, 1}, {1, 1, 1}, 64},
   {"atomic-multi-group", {64, 1, 1}, {4, 1, 1}, 257},
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
   if (test == GRID) {
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
      value = nir_ssbo_atomic(&b, 32, nir_imm_int(&b, 0), nir_imm_int(&b, 256 * 4),
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
   nir_store_ssbo(&b, value, nir_imm_int(&b, 0), nir_imul_imm(&b, id, 4),
      .align_mul = 4, .write_mask = 1);
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
   struct pipe_resource templ = {
      .target = PIPE_BUFFER, .format = PIPE_FORMAT_R8_UNORM,
      .width0 = 256, .height0 = 1, .depth0 = 1, .array_size = 1,
      .usage = PIPE_USAGE_DEFAULT, .bind = PIPE_BIND_SHADER_BUFFER,
   };
   void *table = NULL, *output = NULL;
   for (unsigned i = 0; i < 2; ++i) {
      templ.width0 = i ? OUTPUT_WORDS * sizeof(uint32_t) : 256;
      buffers[i] = screen->resource_create(screen, &templ);
      if (!buffers[i])
         goto cleanup;
   }
   if (ps5_resource_info(buffers[0], &table, NULL, NULL) ||
       ps5_resource_info(buffers[1], &output, NULL, NULL))
      goto cleanup;
   memset(table, 0, 256);
   uint32_t *srd = table;
   srd[0] = (uintptr_t)output;
   srd[1] = (uintptr_t)output >> 32;
   srd[3] = UINT32_C(0x31016fac);
   PsbcCompileOptions opts = {
      .target = PSBC_TARGET_PS5, .stage = PSBC_STAGE_COMPUTE,
      .optimise = true, .address32_hi = (uintptr_t)table >> 32,
      .gallium_buffer_arrays = true, .descriptor_binding_count = 1,
      .descriptor_bindings = {{
         .binding = PSBC_GALLIUM_SSBO_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
         .type = PSBC_DESCRIPTOR_STORAGE_BUFFER, .array_size = 16, .stride = 16,
      }},
   };
   for (unsigned test = 0; test < ARRAY_SIZE(cases); ++test) {
      status = 1;
      memset(output, 0xcd, OUTPUT_WORDS * sizeof(uint32_t));
      if (test == ATOMIC)
         ((uint32_t *)output)[256] = 0;
      srd[2] = cases[test].words * sizeof(uint32_t);
      psbc_init();
      nir_shader *nir = create_probe_shader(test);
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
      unsigned correct = count_correct(test, output), guards = 0;
      for (unsigned i = cases[test].words; i < OUTPUT_WORDS; ++i)
         guards += ((uint32_t *)output)[i] == GUARD_WORD;
      status = rc || correct != cases[test].words || guards != OUTPUT_WORDS - cases[test].words;
      printf("[ps5-compute] case=%s dispatch=%d correct=%u/%u guards=%u/%u first=%u last=%u status=%d\n",
         cases[test].name, rc, correct, cases[test].words, guards, OUTPUT_WORDS - cases[test].words,
         ((uint32_t *)output)[0], ((uint32_t *)output)[cases[test].words - 1], status);
      fflush(stdout);
      if (status)
         goto cleanup;
      psbc_free_output(&compiled);
      memset(&compiled, 0, sizeof(compiled));
   }
cleanup:
   psbc_free_output(&compiled);
   for (unsigned i = 0; i < 2; ++i)
      pipe_resource_reference(&buffers[i], NULL);
   screen->destroy(screen);
   printf("[ps5-compute] completed status=%d (internal probe; GL caps unchanged)\n", status);
   return status;
}
