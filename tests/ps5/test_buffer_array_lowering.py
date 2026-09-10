#!/usr/bin/env python3
# PS5 OpenGL - OpenGL implementation for PlayStation 5.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

"""Check stage-local buffer banks and real NIR/ACO compilation without a GPU."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PSBC = ROOT / "third_party/opengnm-psbc"
source = (PSBC / "libpsbc/psbc_compile.c").read_text()
start = source.index("struct gallium_buffer_state {")
lowering = source[start:source.index("/* === Mesa stage mapping", start)]
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "compiler/nir/nir_builder.h"
#include "psbc_compile.h"
''' + lowering + r'''
static PsbcCompileOptions options(PsbcStage stage) {
    return (PsbcCompileOptions) {
        .target=PSBC_TARGET_PS5, .stage=stage, .entrypoint="main",
        .optimise=true, .address32_hi=2, .gallium_buffer_arrays=true,
        .descriptor_binding_count=2,
        .descriptor_bindings={
            {.binding=PSBC_GALLIUM_UBO_ARRAY_BINDING(stage),
             .type=PSBC_DESCRIPTOR_UNIFORM_BUFFER, .array_size=15, .stride=16},
            {.binding=PSBC_GALLIUM_SSBO_ARRAY_BINDING(stage),
             .type=PSBC_DESCRIPTOR_STORAGE_BUFFER, .array_size=16,
             .offset=15*16, .stride=16}},
    };
}
static nir_def *resource(nir_builder *b, PsbcStage stage, nir_def *slot,
                         bool ubo, bool manual) {
    if (!manual) return slot;
    return nir_vulkan_resource_index(b, 3, 32, slot,
        .desc_set=0, .binding=ubo ? PSBC_GALLIUM_UBO_ARRAY_BINDING(stage)
                                 : PSBC_GALLIUM_SSBO_ARRAY_BINDING(stage),
        .desc_type=ubo ? nir_descriptor_type_uniform_buffer : nir_descriptor_type_storage_buffer,
        .resource_type=ubo ? nir_resource_type_uniform_buffer : nir_resource_type_read_write_storage_buffer);
}
static nir_shader *shader(PsbcStage stage, bool manual, bool dynamic) {
    nir_builder b = nir_builder_init_simple_shader((mesa_shader_stage)(stage-1),
        psbc_get_nir_options(stage), "buffer-bank-contract");
    b.shader->info.workgroup_size[0]=16;
    b.shader->info.workgroup_size[1]=b.shader->info.workgroup_size[2]=1;
    b.shader->info.num_ubos=15;
    b.shader->info.num_ssbos=16;
    nir_def *slot = dynamic ? nir_iand_imm(&b,
        nir_channel(&b, nir_load_workgroup_id(&b), 0), 1) : nir_imm_int(&b, 1);
    nir_def *u = resource(&b, stage, slot, true, manual);
    nir_def *s = resource(&b, stage, slot, false, manual);
    nir_def *zero=nir_imm_int(&b, 0), *one=nir_imm_int(&b, 1);
    nir_def *value = nir_load_ubo(&b, 1, 32, u, zero,
        .align_mul=4, .range=4);
    value=nir_iadd(&b, value, nir_load_ssbo(&b, 1, 32, s, zero, .align_mul=4));
    value=nir_iadd(&b, value, nir_get_ssbo_size(&b, 32, s));
    value=nir_iadd(&b, value, nir_ssbo_atomic(&b, 32, s, nir_imm_int(&b, 4), one,
        .atomic_op=nir_atomic_op_iadd));
    value=nir_iadd(&b, value, nir_ssbo_atomic_swap(&b, 32, s, nir_imm_int(&b, 8),
        zero, one, .atomic_op=nir_atomic_op_cmpxchg));
    nir_store_ssbo(&b, value, s, zero, .align_mul=4, .write_mask=1);
    nir_validate_shader(b.shader, "buffer bank input");
    return b.shader;
}
static void structural(void) {
    unsigned keys=0;
    for (PsbcStage stage=PSBC_STAGE_VERTEX; stage<=PSBC_STAGE_COMPUTE; ++stage) {
        PsbcCompileOptions opts=options(stage);
        nir_shader *nir=shader(stage, false, false);
        struct gallium_buffer_state state={.options=&opts, .valid=true};
        assert(nir_shader_instructions_pass(nir, lower_gallium_buffer_index,
            nir_metadata_control_flow, &state) && state.valid);
        nir_validate_shader(nir, "buffer bank output");
        unsigned ubos=0, ssbos=0;
        nir_foreach_function_impl(impl, nir) nir_foreach_block(block, impl)
        nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic) continue;
            nir_intrinsic_instr *intr=nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_vulkan_resource_index) continue;
            unsigned binding=nir_intrinsic_binding(intr);
            assert(binding<24 && nir_src_as_uint(intr->src[0])==1);
            if (binding==PSBC_GALLIUM_UBO_ARRAY_BINDING(stage)) ++ubos;
            else { assert(binding==PSBC_GALLIUM_SSBO_ARRAY_BINDING(stage)); ++ssbos; }
        }
        assert(ubos==1 && ssbos==5);
        unsigned bank_keys=(1u<<opts.descriptor_bindings[0].binding) |
                           (1u<<opts.descriptor_bindings[1].binding);
        assert(!(keys & bank_keys));
        keys |= bank_keys;
        assert(!nir_shader_instructions_pass(nir, lower_gallium_buffer_index,
            nir_metadata_control_flow, &state) && state.valid);
        ralloc_free(nir);
    }
}
static void compiled(bool dynamic) {
    PsbcCompileOptions opts=options(PSBC_STAGE_COMPUTE);
    nir_shader *scalar=shader(opts.stage, false, dynamic);
    nir_shader *manual=shader(opts.stage, true, dynamic);
    PsbcShaderOutput a={0}, b={0};
    assert(psbc_compile_nir(scalar, &opts, &a)==PSBC_RESULT_OK);
    opts.gallium_buffer_arrays=false;
    assert(psbc_compile_nir(manual, &opts, &b)==PSBC_RESULT_OK);
    assert(a.metadata.version==PSBC_SHADER_METADATA_VERSION && a.machine_code_size);
    assert(a.machine_code_size==b.machine_code_size &&
           !memcmp(a.machine_code, b.machine_code, a.machine_code_size));
    assert(a.metadata.descriptor_binding_count==2 && a.metadata.descriptor_set0_valid);
    assert(!a.metadata.scratch_valid);
    printf("buffer arrays: dynamic=%u code=%zu matches explicit resource tuples\n",
        dynamic, a.machine_code_size);
    psbc_free_output(&a); psbc_free_output(&b);
    ralloc_free(scalar); ralloc_free(manual);
}
static void invalid(void) {
    for (unsigned fault=0; fault<6; ++fault) {
        PsbcCompileOptions opts=options(PSBC_STAGE_COMPUTE);
        nir_shader *nir=shader(opts.stage, false, false);
        if (fault==0) {
            opts.gallium_buffer_arrays=false; /* Valid flat UBO, unsupported scalar SSBO. */
            opts.descriptor_bindings[0].binding=PSBC_GALLIUM_UBO_BINDING_BASE+1;
        }
        if (fault==1) opts.descriptor_binding_count=1;  /* Missing storage bank. */
        if (fault==2) opts.descriptor_bindings[1].type=PSBC_DESCRIPTOR_UNIFORM_BUFFER;
        if (fault==3) opts.descriptor_bindings[1].array_size=1; /* Slot 1 out of range. */
        if (fault==4) opts.descriptor_bindings[0].array_size=1;
        if (fault==5) opts.stage=PSBC_STAGE_FRAGMENT; /* Mismatched input stage. */
        PsbcShaderOutput out={0};
        assert(psbc_compile_nir(nir, &opts, &out)==
            (fault==5 ? PSBC_RESULT_UNSUPPORTED_STAGE : PSBC_RESULT_COMPILE_NIR));
        assert(!out.data && !out.machine_code && !out.size && !out.machine_code_size);
        ralloc_free(nir);
    }
}
int main(void) {
    psbc_init(); structural(); compiled(false); compiled(true); invalid(); psbc_shutdown();
}
'''
with tempfile.TemporaryDirectory() as directory:
    obj = str(Path(directory) / "buffers.o")
    executable = str(Path(directory) / "buffers")
    subprocess.run(["clang-18", "-std=gnu11", "-Wall", "-Werror",
        "-DHAVE_ENDIAN_H=1", "-DHAVE_FUNC_ATTRIBUTE_PACKED=1", "-DHAVE_PTHREAD=1",
        "-DHAVE_STRUCT_TIMESPEC=1", "-D_GNU_SOURCE",
        "-I", str(PSBC / "include/mesa"), "-I", str(PSBC / "include"),
        "-I", str(PSBC / "src"), "-I", str(PSBC / "libpsbc"),
        "-x", "c", "-c", "-o", obj, "-"], input=code, text=True, check=True)
    subprocess.run(["g++", "-o", executable, obj, str(PSBC / "libpsbc.a"),
        "-pthread", "-lm"], check=True)
    subprocess.run([executable], check=True, timeout=30)
print("PASS: six-stage buffer banks, SSBO intrinsic families, indexed arrays, reference codegen, clean failures")
