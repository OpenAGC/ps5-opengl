#!/usr/bin/env python3
# PS5 OpenGL - OpenGL implementation for PlayStation 5.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

"""Compile the native compute/render shaders and challenge their readback oracle."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PSBC = ROOT / "third_party/opengnm-psbc"
source = (ROOT / "tests/ps5/egl_public_compute_render.c").read_text()
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "compiler/nir/nir_builder.h"
#include "ps5_agc_package.h"
''' + source[source.index("#define IMAGE_WORDS"):source.index("int main(void)")] + r'''
static void compile(nir_shader *nir, PsbcCompileOptions *options) {
    nir_validate_shader(nir, "compute-render input");
    PsbcShaderOutput out = {0};
    assert(psbc_compile_nir(nir, options, &out) == PSBC_RESULT_OK);
    assert(out.machine_code_size && !out.metadata.scratch_size_per_thread);
    uint8_t *package = NULL;
    size_t size = 0;
    assert(!ps5_agc_package_build(&out, 0, &package, &size) && size);
    printf("stage=%u code=%zu package=%zu\n", options->stage, out.machine_code_size, size);
    free(package); psbc_free_output(&out); ralloc_free(nir);
}
static nir_shader *compute_sample(unsigned test, unsigned unit) {
    nir_builder b=nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
        psbc_get_nir_options(PSBC_STAGE_COMPUTE),"compute-sampled-input");
    b.shader->info.workgroup_size[0]=16;
    b.shader->info.workgroup_size[1]=b.shader->info.workgroup_size[2]=1;
    b.shader->info.num_ssbos=1;
    b.shader->info.num_textures=unit+1;
    BITSET_SET(b.shader->info.textures_used,unit);
    nir_def *id=nir_channel(&b,nir_load_local_invocation_id(&b),0);
    nir_tex_instr *tex=nir_tex_instr_create(b.shader,test==3 ? 1 : 2);
    tex->op=test==3 ? nir_texop_txs : test==4 ? nir_texop_txl : nir_texop_txf;
    tex->sampler_dim=GLSL_SAMPLER_DIM_2D;
    tex->texture_index=tex->sampler_index=unit;
    tex->coord_components=test==3 ? 0 : 2;
    tex->dest_type=test==1 ? nir_type_uint32 :
                   test==2 || test==3 ? nir_type_int32 : nir_type_float32;
    if(test==3) {
        tex->src[0]=nir_tex_src_for_ssa(nir_tex_src_lod,nir_imm_int(&b,0));
    } else {
        tex->src[0]=nir_tex_src_for_ssa(nir_tex_src_coord,test==4 ?
            nir_imm_vec2(&b,0.25,0.5) : nir_vec2(&b,id,nir_imm_int(&b,1)));
        tex->src[1]=nir_tex_src_for_ssa(nir_tex_src_lod,test==4 ?
            nir_imm_float(&b,0) : nir_imm_int(&b,0));
    }
    nir_def_init(&tex->instr,&tex->def,test==3 ? 2 : 4,32);
    nir_builder_instr_insert(&b,&tex->instr);
    nir_def *value=test==3 ? nir_vec4(&b,nir_channel(&b,&tex->def,0),
        nir_channel(&b,&tex->def,1),nir_imm_int(&b,0),nir_imm_int(&b,1)) : &tex->def;
    nir_store_ssbo(&b,value,nir_imm_int(&b,0),nir_imul_imm(&b,id,16),
        .write_mask=0xf,.align_mul=16);
    return b.shader;
}
int main(void) {
    psbc_init();
    PsbcCompileOptions options = {.target=PSBC_TARGET_PS5, .stage=PSBC_STAGE_COMPUTE,
        .optimise=true, .address32_hi=2, .gallium_buffer_arrays=true,
        .descriptor_binding_count=3, .descriptor_bindings={
          {.binding=PSBC_GALLIUM_SSBO_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
           .type=PSBC_DESCRIPTOR_STORAGE_BUFFER, .array_size=16, .stride=16},
          {.binding=PSBC_GALLIUM_UBO_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
           .type=PSBC_DESCRIPTOR_UNIFORM_BUFFER, .array_size=15, .stride=16, .offset=256},
          {.binding=PSBC_GALLIUM_IMAGE_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
           .type=PSBC_DESCRIPTOR_STORAGE_IMAGE, .array_size=8, .stride=32, .offset=496}}};
    compile(build_compute(), &options);
    /* Sampled descriptors coexist with the three existing compute banks.
     * This is compiler/package coverage, not native sampler qualification. */
    for(unsigned unit=0;unit<=7;unit+=7) for(unsigned test=0;test<5;++test) {
        PsbcCompileOptions sampled=options;
        sampled.descriptor_binding_count=4;
        sampled.descriptor_bindings[3]=(PsbcDescriptorBinding){.binding=unit,
            .type=PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER,.array_size=1,
            .stride=48,.offset=752+unit*48};
        compile(compute_sample(test,unit),&sampled);
        nir_shader *missing=compute_sample(test,unit);
        PsbcShaderOutput rejected={0};
        assert(psbc_compile_nir(missing,&options,&rejected)!=PSBC_RESULT_OK);
        assert(!rejected.machine_code);
        psbc_free_output(&rejected); ralloc_free(missing);
    }
    options = (PsbcCompileOptions){.target=PSBC_TARGET_PS5, .stage=PSBC_STAGE_VERTEX,
        .optimise=true, .address32_hi=2, .ngg=true, .primitive_type=6};
    compile(build_vertex(), &options);
    options = (PsbcCompileOptions){.target=PSBC_TARGET_PS5, .stage=PSBC_STAGE_FRAGMENT,
        .optimise=true, .address32_hi=2, .spi_shader_col_format=4,
        .descriptor_binding_count=1, .descriptor_bindings={
          {.binding=0, .type=PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER, .array_size=1, .stride=48}}};
    nir_shader *fs = build_fragment();
    /* Gallium selects descriptors from these masks, not the tex instructions.
     * gather_info alone does not populate them for hand-built indexed NIR. */
    assert(fs->info.num_textures == 1 && BITSET_TEST(fs->info.textures_used, 0) &&
           BITSET_TEST(fs->info.textures_used_by_txf, 0));
    compile(fs, &options);
    /* An incomplete legacy descriptor layout must return an error, not enter
     * RADV's unrelated bindless-heap path and abort the native application. */
    for (unsigned test=0; test<8; ++test) {
        fs = build_fragment();
        nir_tex_instr *tex = NULL;
        nir_foreach_block(block, nir_shader_get_entrypoint(fs))
            nir_foreach_instr(instr, block)
                if (instr->type == nir_instr_type_tex) tex=nir_instr_as_tex(instr);
        assert(tex);
        PsbcCompileOptions candidate = options;
        if (test == 0) candidate.descriptor_binding_count=0;
        if (test == 1) candidate.descriptor_bindings[0].binding=1;
        if (test == 2) {
            candidate.descriptor_bindings[0].type=PSBC_DESCRIPTOR_UNIFORM_BUFFER;
            candidate.descriptor_bindings[0].stride=16;
        }
        if (test == 3) tex->texture_index=64;
        if (test == 4 || test == 5) {
            nir_builder b=nir_builder_create(nir_shader_get_entrypoint(fs));
            b.cursor=nir_before_instr(&tex->instr);
            tex->op=nir_texop_txl;
            tex->sampler_index=7;
            nir_src_rewrite(&tex->src[0].src,nir_imm_vec2(&b,0.25,0.5));
            nir_src_rewrite(&tex->src[1].src,nir_imm_float(&b,0));
            if (test == 5) {
                candidate.descriptor_binding_count=2;
                candidate.descriptor_bindings[1]=candidate.descriptor_bindings[0];
                candidate.descriptor_bindings[1].binding=7;
                candidate.descriptor_bindings[1].offset=48;
            }
        }
        if (test == 6) tex->sampler_index=7; /* txf doesn't consume a sampler. */
        if (test == 7) tex->texture_index=candidate.descriptor_bindings[0].binding=63;
        nir_validate_shader(fs,"legacy layout guard input");
        PsbcShaderOutput output = {0};
        PsbcResult result=psbc_compile_nir(fs,&candidate,&output);
        assert((result == PSBC_RESULT_OK) == (test >= 5));
        assert((output.machine_code != NULL) == (test >= 5));
        psbc_free_output(&output); ralloc_free(fs);
    }
    psbc_shutdown();
    for (unsigned phase=0; phase<4; ++phase) {
        float sign=phase&1 ? 1 : -1;
        uint32_t words[IMAGE_WORDS];
        uint8_t pixels[8*256];
        memset(words,0xcd,sizeof(words));
        memset(pixels,0xcd,sizeof(pixels));
        for(unsigned i=65;i<81;++i) {
            float value=(i-64)*0.25f*sign;
            memcpy(&words[i],&value,4);
        }
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
            uint8_t *p=pixels+y*256+4*x;
            p[0]=sign<0 ? 255 : 0; p[1]=sign>0 ? 255 : 0; p[2]=0; p[3]=255;
        }
        assert(count_image(words,sign)==IMAGE_WORDS && count_pixels(pixels,256,sign)==64);
        assert(count_image(words,-sign)==IMAGE_WORDS-16 && count_pixels(pixels,256,-sign)==0);
        words[0]=0; words[65]=0;
        assert(count_image(words,sign)==IMAGE_WORDS-2);
        pixels[0]^=255; pixels[7*256+7*4+3]=0;
        assert(count_pixels(pixels,256,sign)==62);
    }
}
'''
with tempfile.TemporaryDirectory() as directory:
    obj = str(Path(directory) / "compute-render.o")
    package = str(Path(directory) / "package.o")
    executable = str(Path(directory) / "compute-render")
    subprocess.run(["clang-18", "-std=gnu11", "-Wall", "-Werror",
        "-DHAVE_ENDIAN_H=1", "-DHAVE_FUNC_ATTRIBUTE_PACKED=1", "-DHAVE_PTHREAD=1",
        "-DHAVE_STRUCT_TIMESPEC=1", "-D_GNU_SOURCE",
        "-I", str(PSBC / "include/mesa"), "-I", str(PSBC / "include"),
        "-I", str(PSBC / "src"), "-I", str(PSBC / "libpsbc"),
        "-I", str(ROOT / "src/platform"), "-x", "c", "-c", "-o", obj, "-"],
        input=code, text=True, check=True)
    subprocess.run(["clang-18", "-std=c11", "-Wall", "-Werror",
        "-I", str(PSBC / "libpsbc"), "-c", str(ROOT / "src/platform/ps5_agc_package.c"),
        "-o", package], check=True)
    subprocess.run(["g++", "-o", executable, obj, package, str(PSBC / "libpsbc.a"),
        "-pthread", "-lm"], check=True)
    subprocess.run([executable], check=True, timeout=30)
print("PASS: CS/VS/FS packages; 10 compute-sampling layouts and 10 missing-layout rejections; readback/guard oracles (host only)")
