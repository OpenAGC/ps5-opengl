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
      buffers[i] = screen->resource_create(screen, &templ);
      if (!buffers[i])
         goto cleanup;
   }
   if (ps5_resource_info(buffers[0], &table, NULL, NULL) ||
       ps5_resource_info(buffers[1], &output, NULL, NULL))
      goto cleanup;
   memset(table, 0, 256);
   memset(output, 0xcd, 256);
   uint32_t *srd = table;
   srd[0] = (uintptr_t)output;
   srd[1] = (uintptr_t)output >> 32;
   srd[2] = 64;
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
   psbc_init();
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
      psbc_get_nir_options(PSBC_STAGE_COMPUTE), "native-compute-oracle");
   b.shader->info.workgroup_size[0] = 16;
   b.shader->info.workgroup_size[1] = b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = 16;
   nir_def *id = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
   nir_store_ssbo(&b, nir_iadd_imm(&b, nir_imul_imm(&b, id, 3), 17),
      nir_imm_int(&b, 0), nir_imul_imm(&b, id, 4), .align_mul = 4, .write_mask = 1);
   PsbcResult compile_rc = psbc_compile_nir(b.shader, &opts, &compiled);
   ralloc_free(b.shader);
   psbc_shutdown();
   printf("[ps5-compute] compiled rc=%d code=%zu stage=%u wave=%u lds=%u scratch=%u\n",
      compile_rc, compiled.machine_code_size, compiled.metadata.hardware_stage,
      compiled.metadata.compute_wave_size, compiled.metadata.compute_lds_bytes,
      compiled.metadata.scratch_bytes_per_wave);
   fflush(stdout);
   if (compile_rc != PSBC_RESULT_OK)
      goto cleanup;
   const uint32_t groups[3] = {1, 1, 1};
   int rc = ps5_agc_compute_execute(screen, &compiled, buffers[0], buffers + 1, 1, groups);
   unsigned correct = 0, guards = 0;
   for (unsigned i = 0; i < 16; ++i)
      correct += ((uint32_t *)output)[i] == 17 + 3 * i;
   for (unsigned i = 16; i < 64; ++i)
      guards += ((uint32_t *)output)[i] == UINT32_C(0xcdcdcdcd);
   status = rc || correct != 16 || guards != 48;
   printf("[ps5-compute] dispatch=%d correct=%u/16 guards=%u/48 first=%u last=%u status=%d\n",
      rc, correct, guards, ((uint32_t *)output)[0], ((uint32_t *)output)[15], status);
cleanup:
   psbc_free_output(&compiled);
   for (unsigned i = 0; i < 2; ++i)
      pipe_resource_reference(&buffers[i], NULL);
   screen->destroy(screen);
   printf("[ps5-compute] completed status=%d (internal probe; GL caps unchanged)\n", status);
   return status;
}
