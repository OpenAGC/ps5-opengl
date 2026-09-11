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
#include "util/format/u_format.h"
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
int main(void) {
    psbc_init();
    for(unsigned kind=0;kind<4;++kind) {
        enum pipe_format format=kind<3 ? image_formats[kind] : PIPE_FORMAT_R8G8B8A8_UNORM;
        const uint32_t expected_r32[6]={4,0,0,1,0,1}, expected_rgba[6]={4,5,6,7,0,1};
        for(unsigned channel=0;channel<6;++channel) {
            uint32_t selector=99;
            assert(ps5_texture_descriptor_swizzle(channel,format,&selector));
            assert(selector==(kind<3 ? expected_r32[channel] : expected_rgba[channel]));
        }
        uint32_t selector=99;
        assert(!ps5_texture_descriptor_swizzle(6,format,&selector) && selector==99);
    }
    assert(expected_red(0,1,-1)==0xbe800000 && expected_red(0,16,1)==0x40800000);
    assert(expected_red(1,1,-1)==0x80000008 && expected_red(1,16,1)==0x10000035);
    assert(expected_red(2,1,-1)==(uint32_t)-1007 && expected_red(2,16,1)==1112);
    PsbcCompileOptions options = {.target=PSBC_TARGET_PS5, .stage=PSBC_STAGE_COMPUTE,
        .optimise=true, .address32_hi=2, .gallium_buffer_arrays=true,
        .descriptor_binding_count=3, .descriptor_bindings={
          {.binding=PSBC_GALLIUM_SSBO_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
           .type=PSBC_DESCRIPTOR_STORAGE_BUFFER, .array_size=16, .stride=16},
          {.binding=PSBC_GALLIUM_UBO_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
           .type=PSBC_DESCRIPTOR_UNIFORM_BUFFER, .array_size=15, .stride=16, .offset=256},
          {.binding=PSBC_GALLIUM_IMAGE_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
           .type=PSBC_DESCRIPTOR_STORAGE_IMAGE, .array_size=8, .stride=32, .offset=496}}};
    for(unsigned kind=0;kind<3;++kind) compile(build_compute(kind), &options);
    /* Sampled descriptors coexist with the three existing compute banks.
     * This is compiler/package coverage, not native sampler qualification. */
    for(unsigned unit=0;unit<=7;unit+=7) for(unsigned test=0;test<6;++test) {
        nir_shader *checked=compute_sample(test,unit);
        unsigned used=0, filtered=0;
        assert(ps5_compute_texture_usage(checked,&used,&filtered));
        assert(used==(1u<<unit) && filtered==(test>=4 ? 1u<<unit : 0));
        ralloc_free(checked);
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
    for(unsigned fault=0;fault<7;++fault) {
        nir_shader *checked=compute_sample(fault>=5 ? 4 : 0,0);
        nir_tex_instr *tex=NULL;
        nir_foreach_block(block,nir_shader_get_entrypoint(checked))
            nir_foreach_instr(instr,block)
                if(instr->type==nir_instr_type_tex) tex=nir_instr_as_tex(instr);
        assert(tex);
        if(fault==0) tex->texture_index=8;
        if(fault==1) tex->is_array=true;
        if(fault==2) tex->sampler_dim=GLSL_SAMPLER_DIM_3D;
        if(fault==3) tex->src[1].src_type=nir_tex_src_texture_offset;
        if(fault==5) tex->sampler_index=1;
        if(fault==6) tex->dest_type=nir_type_uint32;
        if(fault==4) {
            nir_builder b=nir_builder_create(nir_shader_get_entrypoint(checked));
            b.cursor=nir_before_instr(&tex->instr);
            nir_src_rewrite(&tex->src[1].src,nir_imm_int(&b,1));
        }
        unsigned used=0, filtered=0;
        assert(!ps5_compute_texture_usage(checked,&used,&filtered));
        ralloc_free(checked);
    }
    options = (PsbcCompileOptions){.target=PSBC_TARGET_PS5, .stage=PSBC_STAGE_VERTEX,
        .optimise=true, .address32_hi=2, .ngg=true, .primitive_type=6};
    compile(build_vertex(), &options);
    options = (PsbcCompileOptions){.target=PSBC_TARGET_PS5, .stage=PSBC_STAGE_FRAGMENT,
        .optimise=true, .address32_hi=2, .spi_shader_col_format=4,
        .descriptor_binding_count=1, .descriptor_bindings={
          {.binding=0, .type=PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER, .array_size=1, .stride=48}}};
    nir_shader *fs = build_fragment(0);
    /* Gallium selects descriptors from these masks, not the tex instructions.
     * gather_info alone does not populate them for hand-built indexed NIR. */
    assert(fs->info.num_textures == 1 && BITSET_TEST(fs->info.textures_used, 0) &&
           BITSET_TEST(fs->info.textures_used_by_txf, 0));
    compile(fs, &options);
    for(unsigned kind=1;kind<3;++kind) compile(build_fragment(kind), &options);
    /* An incomplete legacy descriptor layout must return an error, not enter
     * RADV's unrelated bindless-heap path and abort the native application. */
    for (unsigned test=0; test<8; ++test) {
        fs = build_fragment(0);
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
    for (unsigned kind=0;kind<3;++kind) for (unsigned phase=0; phase<4; ++phase) {
        float sign=phase&1 ? 1 : -1;
        for(unsigned size_query=0;size_query<(kind==0 ? 8u : 2u);++size_query) {
            uint32_t sampled[80]; memset(sampled,0xcd,sizeof(sampled));
            for(unsigned i=0;i<16;++i) {
                if(size_query==1) {
                    const uint32_t size[4]={17,3,0,1};
                    memcpy(sampled+8+i*4,size,16);
                } else {
                    const uint32_t value[4]={expected_red(kind,i+1,sign),0,0,kind==0 ? 0x3f800000u : 1};
                    memcpy(sampled+8+i*4,value,16);
                    if(size_query>=2) {
                        const uint32_t bits[2][2]={{0x3f800000,0x40400000},{0x3f700000,0x40440000}};
                        sampled[8+i*4]=bits[size_query&1][(i&1)^(size_query==4 || size_query==5)] ^
                            (sign<0 ? 0x80000000u : 0);
                    }
                }
            }
            assert(count_sampled(sampled,sign,size_query,kind)==80);
            if(size_query!=1) assert(count_sampled(sampled,-sign,size_query,kind)==64);
            sampled[0]=0; sampled[71]^=1;
            assert(count_sampled(sampled,sign,size_query,kind)==78);
        }
        uint32_t words[IMAGE_WORDS];
        uint8_t pixels[8*256];
        memset(words,0xcd,sizeof(words));
        memset(pixels,0xcd,sizeof(pixels));
        for(unsigned i=65;i<81;++i) {
            words[i]=expected_red(kind,i-64,sign);
        }
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
            uint8_t *p=pixels+y*256+4*x;
            p[0]=sign<0 ? 255 : 0; p[1]=sign>0 ? 255 : 0; p[2]=0; p[3]=255;
        }
        assert(count_image(words,sign,kind)==IMAGE_WORDS && count_pixels(pixels,256,sign)==64);
        assert(count_image(words,-sign,kind)==IMAGE_WORDS-16 && count_pixels(pixels,256,-sign)==0);
        words[0]=0; words[65]=0;
        assert(count_image(words,sign,kind)==IMAGE_WORDS-2);
        pixels[0]^=255; pixels[7*256+7*4+3]=0;
        assert(count_pixels(pixels,256,sign)==62);
    }
}
'''
driver = (ROOT / "src/gallium/ps5/ps5_screen.c").read_text()
swizzle_start = driver.index("static bool\nps5_texture_descriptor_swizzle(")
swizzle = driver[swizzle_start:driver.index("static bool\nps5_texture_descriptor_wrap(",swizzle_start)]
code = code.replace("static void compile(",swizzle+"static void compile(",1)
usage_start = driver.index("static bool\nps5_compute_texture_usage(")
usage = driver[usage_start:driver.index("/* Internal compute bring-up", usage_start)]
code = code.replace("static nir_shader *compute_sample(",
    "#define PS5_COMPUTE_TEXTURE_SLOTS 8\n" + usage + "static nir_shader *compute_sample(", 1)
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
print("PASS: CS/VS/FS packages; 12 compute-sampling layouts and 12 missing-layout rejections; readback/guard oracles (host only)")
