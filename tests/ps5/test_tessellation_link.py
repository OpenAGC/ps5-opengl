#!/usr/bin/env python3
# PS5 OpenGL - OpenGL implementation for PlayStation 5.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

"""Linked RADV/ACO compiler and hull-package regression; no native execution."""
import hashlib
import os
import resource
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PSBC = ROOT / "third_party/opengnm-psbc"
LIB = PSBC / "libpsbc.a"


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 3] + "\n"


pipeline = (PSBC / "src/amd/vulkan/radv_pipeline_graphics.c").read_text()
compile_source = (PSBC / "libpsbc/psbc_compile.c").read_text()
util_source = (PSBC / "src/amd/common/ac_shader_util.c").read_text()
order = pipeline.index("static const mesa_shader_stage graphics_shader_order[]")
helpers = function(compile_source, "static void setup_ac_info(")
helpers += function(pipeline, "static void\nmerge_tess_info(")
helpers += pipeline[order:pipeline.index("};", order) + 2] + "\n"
for name in ("radv_graphics_shaders_fill_linked_vs_io_info",
             "radv_graphics_shaders_fill_linked_tcs_tes_io_info",
             "radv_graphics_shaders_fill_linked_tes_gs_io_info",
             "radv_graphics_shaders_fill_linked_io_info",
             "radv_graphics_shaders_link_varyings", "radv_fill_shader_info_ngg",
             "radv_declare_pipeline_args"):
    helpers += function(pipeline, "static void\n" + name + "(")
helpers += function(util_source, "static unsigned get_tcs_wg_output_mem_size(")

code = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psbc_compile.h"
#include "ps5_agc_package.h"
#include "compiler/nir/nir_serialize.h"
#include "util/blob.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_xfb_info.h"
#include "amd/vulkan/radv_shader.h"
#include "amd/vulkan/radv_shader_args.h"
#include "amd/vulkan/radv_pipeline_graphics.h"
#include "amd/vulkan/radv_pipeline.h"
#include "amd/vulkan/nir/radv_nir.h"
#include "amd/common/nir/ac_nir.h"
#include "amd/common/ac_shader_util.h"
#include "amd/common/sid.h"
''' + helpers + r'''

static nir_variable *varying(nir_builder *b, nir_variable_mode mode,
                            const struct glsl_type *type, unsigned slot, bool patch) {
    nir_variable *v=nir_variable_create(b->shader,mode,type,"linked-value");
    v->data.location=slot; v->data.patch=patch;
    return v;
}
static nir_deref_instr *element(nir_builder *b,nir_variable *v,nir_def *i) {
    return nir_build_deref_array(b,nir_build_deref_var(b,v),i);
}
static nir_shader *build(mesa_shader_stage stage, bool cross,
                        const struct radv_compiler_info *ci) {
    nir_builder b=nir_builder_init_simple_shader(stage,&ci->nir_options[stage],"linked-tess-host");
    if(stage==MESA_SHADER_VERTEX) {
        nir_def *id=nir_u2f32(&b,nir_iadd(&b,nir_load_vertex_id_zero_base(&b),
                                         nir_load_first_vertex(&b)));
        nir_def *p=nir_vec4(&b,id,nir_fmul(&b,id,id),nir_fsin(&b,id),nir_imm_float(&b,1));
        nir_store_var(&b,varying(&b,nir_var_shader_out,glsl_vec4_type(),VARYING_SLOT_POS,false),p,15);
    } else if(stage==MESA_SHADER_GEOMETRY) {
        b.shader->info.gs.input_primitive=MESA_PRIM_TRIANGLES;
        b.shader->info.gs.output_primitive=MESA_PRIM_TRIANGLE_STRIP;
        b.shader->info.gs.vertices_in=3;
        b.shader->info.gs.vertices_out=3;
        b.shader->info.gs.invocations=1;
        b.shader->info.gs.active_stream_mask=1;
        nir_def *zero=nir_imm_int(&b,0);
        for(unsigned i=0;i<3;++i) {
            nir_def *p=nir_load_per_vertex_input(&b,4,32,nir_imm_int(&b,i),zero,
                .dest_type=nir_type_float32,
                .io_semantics={.location=VARYING_SLOT_POS,.num_slots=1});
            nir_store_output(&b,p,zero,.src_type=nir_type_float32,
                .io_semantics={.location=VARYING_SLOT_POS,.num_slots=1});
            nir_emit_vertex(&b,0);
        }
        nir_end_primitive(&b,0);
    } else {
        b.shader->info.tess._primitive_mode=TESS_PRIMITIVE_TRIANGLES;
        b.shader->info.tess.spacing=TESS_SPACING_EQUAL;
        b.shader->info.tess.ccw=true;
        nir_variable *in=varying(&b,nir_var_shader_in,glsl_array_type(glsl_vec4_type(),3,0),VARYING_SLOT_POS,false);
        if(stage==MESA_SHADER_TESS_CTRL) {
            b.shader->info.tess.tcs_vertices_out=3;
            nir_def *id=nir_load_invocation_id(&b);
            nir_def *index=cross ? nir_umod(&b,nir_iadd_imm(&b,id,1),nir_imm_int(&b,3)) : id;
            nir_def *p=nir_load_deref(&b,element(&b,in,index));
            nir_variable *out=varying(&b,nir_var_shader_out,glsl_array_type(glsl_vec4_type(),3,0),VARYING_SLOT_POS,false);
            nir_store_deref(&b,element(&b,out,id),p,15);
            nir_push_if(&b,nir_ieq_imm(&b,id,0));
            if(cross) {
                nir_def *q=nir_load_deref(&b,element(&b,in,nir_imm_int(&b,2)));
                nir_store_var(&b,varying(&b,nir_var_shader_out,glsl_float_type(),VARYING_SLOT_PATCH0,true),
                              nir_fmul(&b,nir_channel(&b,q,0),nir_channel(&b,q,1)),1);
            }
            for(unsigned inner=0;inner<2;++inner) {
                unsigned count=inner ? 2 : 4;
                nir_variable *tf=varying(&b,nir_var_shader_out,glsl_array_type(glsl_float_type(),count,0),
                    inner ? VARYING_SLOT_TESS_LEVEL_INNER : VARYING_SLOT_TESS_LEVEL_OUTER,true);
                for(unsigned i=0;i<count;++i)
                    nir_store_deref(&b,element(&b,tf,nir_imm_int(&b,i)),nir_imm_float(&b,2),1);
            }
            nir_pop_if(&b,NULL);
        } else {
            nir_def *coord=nir_load_tess_coord(&b), *p=nir_imm_vec4(&b,0,0,0,0);
            for(unsigned i=0;i<3;++i) {
                nir_def *v=nir_load_deref(&b,element(&b,in,nir_imm_int(&b,i)));
                p=nir_fadd(&b,p,nir_fmul(&b,v,nir_channel(&b,coord,i)));
            }
            if(cross) {
                nir_def *q=nir_load_var(&b,varying(&b,nir_var_shader_in,glsl_float_type(),VARYING_SLOT_PATCH0,true));
                p=nir_fadd(&b,p,nir_vec4(&b,q,nir_imm_float(&b,0),nir_imm_float(&b,0),nir_imm_float(&b,0)));
            }
            nir_store_var(&b,varying(&b,nir_var_shader_out,glsl_vec4_type(),VARYING_SLOT_POS,false),p,15);
        }
    }
    nir_validate_shader(b.shader,"builder");
    return b.shader;
}
static void dump(nir_shader *nir,const char *phase) {
    char name[100]; snprintf(name,sizeof(name),"%u-%s.nir",nir->info.stage,phase);
    FILE *f=fopen(name,"w"); assert(f); nir_print_shader(nir,f); fclose(f);
}
static unsigned count(nir_shader *nir,nir_intrinsic_op op) {
    unsigned n=0;
    nir_foreach_function_impl(impl,nir) nir_foreach_block(block,impl) nir_foreach_instr(instr,block)
        if(instr->type==nir_instr_type_intrinsic && nir_instr_as_intrinsic(instr)->intrinsic==op) ++n;
    return n;
}
static uint32_t context_value(const PsbcShaderMetadata *metadata, uint16_t offset) {
    for(unsigned i=0;i<metadata->context_register_count;++i)
        if(metadata->context_registers[i].offset==offset)
            return metadata->context_registers[i].value;
    assert(!"missing context register"); return 0;
}
static unsigned ring_access(nir_shader *nir, nir_intrinsic_op access, nir_intrinsic_op ring) {
    unsigned n=0;
    nir_foreach_function_impl(impl,nir) nir_foreach_block(block,impl) nir_foreach_instr(instr,block) {
        if(instr->type!=nir_instr_type_intrinsic) continue;
        nir_intrinsic_instr *i=nir_instr_as_intrinsic(instr);
        if(i->intrinsic!=access) continue;
        nir_instr *desc=nir_def_instr(i->src[access==nir_intrinsic_store_buffer_amd ? 1 : 0].ssa);
        if(desc->type==nir_instr_type_intrinsic && nir_instr_as_intrinsic(desc)->intrinsic==ring) ++n;
    }
    return n;
}
/* Ask the actual ABI lowering for constants in both producer/consumer contexts.
 * The witness is not compiled and has no hardware effect. */
static void abi_constants(const struct radv_compiler_info *ci,const struct radv_graphics_state_key *gfx,
                          const struct radv_shader_stage *stage,unsigned values[3]) {
    struct radv_shader_stage witness=*stage;
    nir_builder b=nir_builder_init_simple_shader(stage->stage,&ci->nir_options[stage->stage],"abi-witness");
    b.shader->info.tess=stage->nir->info.tess;
    nir_def *stride=stage->stage==MESA_SHADER_TESS_EVAL ? nir_imm_int(&b,0) : nir_load_lshs_vertex_stride_amd(&b);
    nir_def *attr=stage->stage==MESA_SHADER_VERTEX ? nir_imm_int(&b,0) : nir_load_tcs_mem_attrib_stride(&b);
    nir_def *patch=stage->stage==MESA_SHADER_VERTEX ? nir_imm_int(&b,0) : nir_load_hs_out_patch_data_offset_amd(&b);
    nir_store_output(&b,nir_vec3(&b,stride,attr,patch),nir_imm_int(&b,0),.write_mask=7,
                     .io_semantics={.location=VARYING_SLOT_VAR0,.num_slots=1});
    witness.nir=b.shader;
    NIR_PASS(_,b.shader,radv_nir_lower_abi,ci->ac->gfx_level,&witness,gfx,ci->hw.address32_hi);
    NIR_PASS(_,b.shader,nir_opt_constant_folding);
    nir_foreach_function_impl(impl,b.shader) nir_foreach_block(block,impl) nir_foreach_instr(instr,block) {
        if(instr->type!=nir_instr_type_intrinsic) continue;
        nir_intrinsic_instr *i=nir_instr_as_intrinsic(instr);
        if(i->intrinsic!=nir_intrinsic_store_output) continue;
        assert(nir_src_is_const(i->src[0]));
        for(unsigned c=0;c<3;++c) values[c]=nir_src_as_const_value(i->src[0])[c].u32;
    }
    dump(b.shader,"abi-witness"); ralloc_free(b.shader);
}

/* Public API checks are additional to, not a replacement for, the extracted
 * RADV reference below. They intentionally require a refreshed host archive. */
static PsbcResult checked_compile(nir_shader** inputs,
                                  const PsbcTessellationCompileOptions* options,
                                  PsbcTessellationOutput* out) {
    struct blob before[3], after[3];
    const struct nir_shader_compiler_options* pointers[3];
    for (unsigned i=0;i<3;++i) {
        blob_init(&before[i]);
        nir_serialize(&before[i],inputs[i],false);
        pointers[i]=inputs[i]->options;
    }
    PsbcResult result=psbc_compile_nir_tessellation_pipeline(inputs[0],inputs[1],inputs[2],NULL,options,out);
    for (unsigned i=0;i<3;++i) {
        assert(inputs[i]->options==pointers[i]); /* Never dereference a stale options pointer. */
        blob_init(&after[i]);
        nir_serialize(&after[i],inputs[i],false);
        assert(before[i].size==after[i].size);
        assert(!memcmp(before[i].data,after[i].data,before[i].size));
        blob_finish(&before[i]); blob_finish(&after[i]);
    }
    return result;
}
static void expect_empty(const PsbcTessellationOutput* out) {
    const PsbcTessellationOutput empty={0};
    assert(!memcmp(out,&empty,sizeof(empty)));
}
static int type_size_vec4(const struct glsl_type* type,bool bindless) {
    return glsl_count_attribute_slots(type,false);
}
static void api_tests(const struct radv_compiler_info* ci,bool cross,
                      PsbcTessellationOutput* reference) {
    nir_shader* inputs[]={build(MESA_SHADER_VERTEX,cross,ci),
                         build(MESA_SHADER_TESS_CTRL,cross,ci),
                         build(MESA_SHADER_TESS_EVAL,cross,ci)};
    PsbcTessellationCompileOptions options={
        .input_patch_vertices=3,.offchip_workgroup_capacity_dwords=8192,.address32_hi=2
    };
    PsbcTessellationOutput out={0};
    assert(psbc_compile_nir_tessellation_pipeline(NULL,inputs[1],inputs[2],NULL,&options,&out)
           ==PSBC_RESULT_INVALID_ARGUMENT);
    expect_empty(&out);
    assert(checked_compile(inputs,NULL,&out)==PSBC_RESULT_INVALID_ARGUMENT);
    expect_empty(&out);
    assert(checked_compile(inputs,&options,NULL)==PSBC_RESULT_INVALID_ARGUMENT);
    nir_shader* gs=build(MESA_SHADER_GEOMETRY,false,ci);
    PsbcResult gs_result=psbc_compile_nir_tessellation_pipeline(
        inputs[0],inputs[1],inputs[2],gs,&options,&out);
    printf("four-stage result=%d %s\n",gs_result,psbc_result_string(gs_result));
    assert(gs_result==PSBC_RESULT_OK);
    assert(out.tes.metadata.source_stage==PSBC_STAGE_GEOMETRY);
    assert(out.tes.metadata.hardware_stage==PSBC_HW_STAGE_NGG);
    assert(context_value(&out.tes.metadata,0x2ab));
    assert(out.runtime.final_offchip_layout_valid);
    assert(out.runtime.final_offchip_layout_user_data_dword<
           out.tes.metadata.user_sgpr_count);
    assert(out.runtime.final_offchip_layout);
    psbc_free_tessellation_output(&out); ralloc_free(gs);
    mesa_shader_stage stage=inputs[0]->info.stage;
    inputs[0]->info.stage=MESA_SHADER_FRAGMENT;
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_UNSUPPORTED_STAGE);
    expect_empty(&out); inputs[0]->info.stage=stage;
    nir_function* unused=nir_function_create(inputs[0],"unused");
    nir_validate_shader(inputs[0],"valid unused function declaration");
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_INVALID_ARGUMENT);
    expect_empty(&out); exec_node_remove(&unused->node);
    for (unsigned n=0;n<2;++n) {
        options.input_patch_vertices=n ? 33 : 0;
        assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_INVALID_ARGUMENT);
        expect_empty(&out);
    }
    options.input_patch_vertices=3;
    const uint32_t capacities[]={0,1,UINT32_MAX};
    for (unsigned n=0;n<3;++n) {
        options.offchip_workgroup_capacity_dwords=capacities[n];
        assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_INVALID_ARGUMENT);
        expect_empty(&out);
    }
    options.offchip_workgroup_capacity_dwords=8192;
    struct shader_info tcs_info=inputs[1]->info, tes_info=inputs[2]->info;
    inputs[2]->info.tess.tcs_vertices_out=4; /* Contradictory modes: no RADV assertion. */
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_INVALID_ARGUMENT);
    expect_empty(&out); inputs[2]->info=tes_info;
    inputs[1]->info.tess.spacing=inputs[2]->info.tess.spacing=TESS_SPACING_FRACTIONAL_EVEN;
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_OK);
    assert(G_028B6C_PARTITIONING(out.runtime.tf_param)==V_028B6C_PART_FRAC_EVEN);
    psbc_free_tessellation_output(&out); inputs[1]->info=tcs_info; inputs[2]->info=tes_info;
    inputs[2]->info.tess.point_mode=true;
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_OK);
    assert(G_028B6C_TOPOLOGY(out.runtime.tf_param)==V_028B6C_OUTPUT_POINT);
    psbc_free_tessellation_output(&out); inputs[2]->info=tes_info;
    inputs[1]->info.tess._primitive_mode=inputs[2]->info.tess._primitive_mode=TESS_PRIMITIVE_QUADS;
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_OK);
    assert(G_028B6C_TYPE(out.runtime.tf_param)==V_028B6C_TESS_QUAD);
    psbc_free_tessellation_output(&out); inputs[1]->info=tcs_info; inputs[2]->info=tes_info;
    inputs[1]->info.tess._primitive_mode=inputs[2]->info.tess._primitive_mode=TESS_PRIMITIVE_ISOLINES;
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_OK);
    assert(G_028B6C_TYPE(out.runtime.tf_param)==V_028B6C_TESS_ISOLINE);
    assert(G_028A6C_OUTPRIM_TYPE(context_value(&out.tes.metadata,0x29b))==V_028A6C_LINESTRIP);
    psbc_free_tessellation_output(&out); inputs[1]->info=tcs_info; inputs[2]->info=tes_info;
    inputs[1]->info.tess.ccw=inputs[2]->info.tess.ccw=false;
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_OK);
    assert(G_028B6C_TOPOLOGY(out.runtime.tf_param)==V_028B6C_OUTPUT_TRIANGLE_CW);
    psbc_free_tessellation_output(&out); inputs[1]->info=tcs_info; inputs[2]->info=tes_info;
    inputs[0]->info.num_ubos=1;
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_INVALID_ARGUMENT);
    expect_empty(&out); inputs[0]->info.num_ubos=0;
    /* A resource declaration with stale/zero resource counts must also fail. */
    nir_variable* resource=nir_variable_create(inputs[0],nir_var_uniform,glsl_float_type(),"unsupported-uniform");
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_INVALID_ARGUMENT);
    expect_empty(&out); exec_node_remove(&resource->node);
    nir_variable* attr=nir_variable_create(inputs[0],nir_var_shader_in,glsl_vec4_type(),"unsupported-attribute");
    attr->data.location=VERT_ATTRIB_GENERIC0;
    assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_INVALID_ARGUMENT);
    expect_empty(&out); exec_node_remove(&attr->node);

    /* Mesa hands Gallium lowered I/O; the compiler accepts the exact
     * tessellation intrinsics rather than attempting a lossy round-trip. */
    nir_shader* lowered[3];
    for (unsigned i=0;i<3;++i) {
        lowered[i]=nir_shader_clone(NULL,inputs[i]); assert(lowered[i]);
        nir_lower_io(lowered[i],nir_var_shader_in|nir_var_shader_out,type_size_vec4,0);
        nir_lower_io_to_scalar(lowered[i],nir_var_shader_in|nir_var_shader_out,NULL,NULL);
        nir_opt_dce(lowered[i]);
        nir_opt_vectorize_io(lowered[i],nir_var_shader_in|nir_var_shader_out,false);
        nir_remove_dead_variables(lowered[i],nir_var_shader_in|nir_var_shader_out,NULL);
        lowered[i]->info.io_lowered=true;
        nir_shader_gather_info(lowered[i],nir_shader_get_entrypoint(lowered[i]));
    }
    PsbcResult lowered_result=checked_compile(lowered,&options,&out);
    printf("lowered result=%d\n",lowered_result);
    assert(lowered_result==PSBC_RESULT_OK);
    psbc_free_tessellation_output(&out); expect_empty(&out);
    for (unsigned i=0;i<3;++i) ralloc_free(lowered[i]);

    /* Actual merge rules: TCS-only and TES-only mode declarations both work.
     * OutputVertices may also arrive only in TES. The borrowed shader retains
     * its original, unmerged fields/options on success and on every failure. */
    for (unsigned modes=0;modes<3;++modes) {
        inputs[1]->info=tcs_info; inputs[2]->info=tes_info;
        if (modes==1) {
            inputs[1]->info.tess.spacing=TESS_SPACING_UNSPECIFIED;
            inputs[1]->info.tess._primitive_mode=TESS_PRIMITIVE_UNSPECIFIED;
            inputs[1]->info.tess.ccw=false;
            inputs[1]->info.tess.tcs_vertices_out=0;
            inputs[2]->info.tess.tcs_vertices_out=3;
        } else if (modes==2) {
            inputs[2]->info.tess.spacing=TESS_SPACING_UNSPECIFIED;
            inputs[2]->info.tess._primitive_mode=TESS_PRIMITIVE_UNSPECIFIED;
            inputs[2]->info.tess.ccw=false;
        }
        PsbcResult result=checked_compile(inputs,&options,&out);
        printf("public API %s modes=%u: %s\n",cross ? "cross" : "same",modes,psbc_result_string(result));
        assert(result==PSBC_RESULT_OK);
        PsbcShaderOutput* pair[]={&out.hs,&out.tes};
        assert(out.hs.machine_code!=out.tes.machine_code);
        for (unsigned i=0;i<2;++i) {
            assert(pair[i]->machine_code && pair[i]->machine_code_size);
            assert(!pair[i]->data && !pair[i]->size);
            assert(pair[i]->metadata.source_stage==(i ? PSBC_STAGE_TESS_EVAL : PSBC_STAGE_TESS_CTRL));
            assert(pair[i]->metadata.hardware_stage==(i ? PSBC_HW_STAGE_NGG : PSBC_HW_STAGE_HULL));
            uint8_t* package=NULL; size_t size=0;
            if (i) {
                assert(pair[i]->metadata.linkage_valid);
                assert(pair[i]->metadata.context_register_count);
                assert(pair[i]->metadata.shader_register_count==6);
                assert(G_028B54_LS_EN(pair[i]->metadata.linkage_stages_en.value)==V_028B54_LS_STAGE_ON);
                assert(G_028B54_HS_EN(pair[i]->metadata.linkage_stages_en.value));
                assert(G_028B54_DYNAMIC_HS(pair[i]->metadata.linkage_stages_en.value));
                assert(G_028B54_ES_EN(pair[i]->metadata.linkage_stages_en.value)==V_028B54_ES_STAGE_DS);
                assert(G_028B54_PRIMGEN_EN(pair[i]->metadata.linkage_stages_en.value));
                assert(G_03096C_VERT_GRP_SIZE(pair[i]->metadata.linkage_ge_cntl.value)==0);
                printf("TES metadata stages=%08x ge=%08x context=%u shader=%u ngg-lds=%u/%u\n",
                       pair[i]->metadata.linkage_stages_en.value,
                       pair[i]->metadata.linkage_ge_cntl.value,
                       pair[i]->metadata.context_register_count,
                       pair[i]->metadata.shader_register_count,
                       pair[i]->metadata.ngg_lds_layout_valid,
                       pair[i]->metadata.ngg_lds_layout);
                assert(ps5_agc_package_build(pair[i],0,&package,&size)==0);
                uint64_t sections=0, header_at=0;
                memcpy(&sections,package+40,8);
                memcpy(&header_at,package+sections+2*64+24,8);
                const uint8_t *header=package+header_at;
                assert(header[90]==2);
                free(package);
            } else {
                assert(!pair[i]->metadata.linkage_valid);
                assert(!pair[i]->metadata.context_register_count);
                assert(pair[i]->metadata.shader_register_count==2);
                assert(pair[i]->metadata.base_vertex_valid);
                assert(pair[i]->metadata.is_indexed_draw_valid);
                assert(pair[i]->metadata.base_vertex_user_data_dword <
                       pair[i]->metadata.user_sgpr_count);
                assert(pair[i]->metadata.shader_registers[0].offset==0x148);
                assert(pair[i]->metadata.shader_registers[1].offset==0x10a);
                assert(ps5_agc_package_build(pair[i],0,&package,&size)==0);
                uint64_t sections=0, header_at=0;
                memcpy(&sections,package+40,8);
                memcpy(&header_at,package+sections+2*64+24,8);
                const uint8_t *header=package+header_at;
                assert(header[90]==3 && header[91]==0 && header[92]==2);
                assert(!memcmp(header+96,pair[i]->metadata.shader_registers,
                               2*sizeof(PsbcRegisterWrite)));
                free(package);
            }
        }
        assert(out.runtime.valid && out.runtime.input_patch_vertices==3);
        assert(out.runtime.output_patch_vertices==3 && out.runtime.num_patches==64);
        assert(out.runtime.lds_bytes==4112);
        assert((out.runtime.hs_rsrc2 & S_00B42C_LDS_SIZE_GFX10(10))==
               S_00B42C_LDS_SIZE_GFX10(10));
        assert(out.runtime.ls_hs_config==
               (S_028B58_NUM_PATCHES(64)|S_028B58_HS_NUM_INPUT_CP(3)|
                S_028B58_HS_NUM_OUTPUT_CP(3)));
        assert(out.runtime.tf_param==
               (S_028B6C_TYPE(V_028B6C_TESS_TRIANGLE)|
                S_028B6C_PARTITIONING(V_028B6C_PART_INTEGER)|
                S_028B6C_TOPOLOGY(V_028B6C_OUTPUT_TRIANGLE_CCW)));
        assert(out.runtime.hs_ring_offsets_sgpr==0 &&
               out.runtime.tes_ring_offsets_sgpr==0);
        assert(out.runtime.hs_ring_offsets_register==0x102 &&
               out.runtime.tes_ring_offsets_register==0x082);
        assert(!out.runtime.final_offchip_layout_valid);
        assert(out.runtime.offchip_ring_bytes_per_workgroup==32768);
        assert(out.runtime.tess_factor_ring_bytes_per_workgroup==1024);
        PsbcTessellationOutput owned=out;
        assert(checked_compile(inputs,&options,&out)==PSBC_RESULT_INVALID_ARGUMENT);
        assert(!memcmp(&out,&owned,sizeof(out)));
        if (!modes) { *reference=out; memset(&out,0,sizeof(out)); }
        psbc_free_tessellation_output(&out); expect_empty(&out);
        psbc_free_tessellation_output(&out); expect_empty(&out);
    }
    psbc_free_tessellation_output(NULL);
    for (unsigned i=0;i<3;++i) ralloc_free(inputs[i]);
    puts("PASS public linked tessellation API: ownership, rejection, HS/TES packaging");
}

int main(int argc,char **argv) {
    assert(argc==2); setvbuf(stdout,NULL,_IONBF,0);
    bool cross=!strcmp(argv[1],"cross");
    puts("HOST MODEL: capacity=8192 dwords, address32_hi=2; fixed runtime contract");
    psbc_init();
    struct ac_compiler_info ac={0}; setup_ac_info(&ac,GFX10_3);
    ac.hs_offchip_workgroup_dw_size=8192; /* Hypothetical host capacity; see ac_fill_tess_info. */
    struct radv_compiler_info ci={.ac=&ac};
    ci.key.family=ci.debug.family=CHIP_NAVI21;
    ci.key.ge_wave_size=64; ci.key.ps_wave_size=32; ci.key.use_ngg=true;
    ci.hw.address32_hi=2; radv_get_nir_options(&ci);
    PsbcTessellationOutput reference={0};
    api_tests(&ci,cross,&reference);
    struct radv_graphics_state_key gfx={0}; gfx.ts.patch_control_points=3;
    struct radv_shader_stage stages[MESA_VULKAN_SHADER_STAGES]={0};
    const mesa_shader_stage ids[]={MESA_SHADER_VERTEX,MESA_SHADER_TESS_CTRL,MESA_SHADER_TESS_EVAL};
    const mesa_shader_stage next[]={MESA_SHADER_TESS_CTRL,MESA_SHADER_TESS_EVAL,MESA_SHADER_FRAGMENT};
    VkShaderStageFlagBits active=0;
    for(unsigned i=0;i<3;++i) {
        struct radv_shader_stage *s=&stages[ids[i]];
        s->stage=ids[i]; s->next_stage=next[i]; s->entrypoint="main";
        s->internal_nir=build(ids[i],cross,&ci);
        const struct radv_spirv_to_nir_options options={.lower_view_index_to_zero=true};
        s->nir=radv_shader_spirv_to_nir(&ci,s,&options,false); assert(s->nir);
        ralloc_free(s->internal_nir); s->internal_nir=NULL;
        NIR_PASS(_,s->nir,nir_normalize_sin_cos);
        radv_optimize_nir(s->nir,false);
        NIR_PASS(_,s->nir,ac_nir_lower_indirect_derefs);
        NIR_PASS(_,s->nir,nir_lower_vars_to_ssa);
        radv_nir_shader_info_init(ids[i],next[i],&s->info);
        active|=1<<ids[i];
    }
    struct radv_shader_stage *vs=&stages[MESA_SHADER_VERTEX], *hs=&stages[MESA_SHADER_TESS_CTRL], *tes=&stages[MESA_SHADER_TESS_EVAL];
    merge_tess_info(&tes->nir->info,&hs->nir->info);
    radv_fill_shader_info_ngg(&ci,stages,active);
    for(unsigned i=0;i<3;++i) {
        nir_shader *n=stages[ids[i]].nir;
        radv_nir_lower_io(n);
        NIR_PASS(_,n,nir_lower_io_to_scalar,nir_var_shader_in|nir_var_shader_out,NULL,NULL);
        NIR_PASS(_,n,nir_opt_copy_prop); NIR_PASS(_,n,nir_opt_constant_folding);
    }
    puts("link-varyings"); radv_graphics_shaders_link_varyings(stages,ac.gfx_level);
    for(unsigned i=0;i<3;++i) {
        struct radv_shader_stage *s=&stages[ids[i]];
        nir_validate_shader(s->nir,"linked IO"); dump(s->nir,"linked");
        radv_nir_shader_info_pass(&ci,s->nir,&s->layout,&s->key,&gfx,RADV_PIPELINE_GRAPHICS,false,&s->info);
    }
    radv_nir_shader_info_link(&ci,&gfx,stages);
    assert(vs->info.vs.as_ls && !vs->info.is_ngg && !hs->info.is_ngg && tes->info.is_ngg);
    assert(vs->info.outputs_linked && hs->info.inputs_linked && hs->info.outputs_linked && tes->info.inputs_linked);
    assert(!vs->info.merged_shader_compiled_separately && !hs->info.merged_shader_compiled_separately);
    assert(vs->info.vs.num_linked_outputs && vs->info.vs.num_linked_outputs==hs->info.tcs.num_linked_inputs);
    assert(hs->info.num_tess_patches && hs->info.num_tess_patches==tes->info.num_tess_patches);
    assert(hs->info.tcs.tcs_vertices_out==3 && tes->info.tes.tcs_vertices_out==3);
    assert(hs->info.tcs.lds_size && hs->info.tcs.lds_size<=ac.lds_size_per_workgroup);
    assert(hs->info.tcs.tes_inputs_read==tes->nir->info.inputs_read);
    assert(hs->info.tcs.tes_patch_inputs_read==tes->nir->info.patch_inputs_read);
    assert(hs->info.vs.tcs_in_out_eq && hs->info.workgroup_size==vs->info.workgroup_size);
    printf("linked slots=%u/%u tes=%u patch=%u patches=%u lds=%u wg=%u temp=%llx lds_mask=%llx\n",
           vs->info.vs.num_linked_outputs,hs->info.tcs.num_linked_inputs,tes->info.tes.num_linked_inputs,
           tes->info.tes.num_linked_patch_inputs,hs->info.num_tess_patches,hs->info.tcs.lds_size,
           hs->info.workgroup_size,(unsigned long long)hs->info.vs.tcs_inputs_via_temp,(unsigned long long)hs->info.vs.tcs_inputs_via_lds);
    assert(cross ? hs->info.vs.tcs_inputs_via_lds!=0 : hs->info.vs.tcs_inputs_via_temp!=0);
    assert(cross ? tes->info.tes.num_linked_patch_inputs!=0 : tes->info.tes.num_linked_patch_inputs==0);
    unsigned footprint=get_tcs_wg_output_mem_size(3,hs->info.tcs.io_info.highest_remapped_vram_output,
        hs->info.tcs.io_info.highest_remapped_vram_patch_output,hs->info.num_tess_patches);
    assert(footprint && footprint<=ac.hs_offchip_workgroup_dw_size*4);
    struct radv_shader_debug_info debug[MESA_VULKAN_SHADER_STAGES]={0};
    radv_declare_pipeline_args(&ci,stages,&gfx,active,debug);
    assert(!memcmp(&vs->args,&hs->args,sizeof(vs->args)));
    assert(!hs->args.ac.tcs_offchip_layout.used && !tes->args.ac.tcs_offchip_layout.used);
    assert(hs->args.ac.ring_offsets.used && tes->args.ac.ring_offsets.used);
    assert(hs->args.ac.args[hs->args.ac.ring_offsets.arg_index].offset==0 &&
           tes->args.ac.args[tes->args.ac.ring_offsets.arg_index].offset==0);
    assert(hs->args.num_user_sgprs==2 && tes->args.num_user_sgprs==1);
    assert(tes->args.ac.args[tes->args.ngg_lds_layout.arg_index].offset==8);
    printf("ring ABI hs arg=%u off=%u users=%u tes arg=%u off=%u users=%u ngg-layout arg=%u off=%u ud=%d\n",
           hs->args.ac.ring_offsets.arg_index,
           hs->args.ac.args[hs->args.ac.ring_offsets.arg_index].offset,
           hs->args.num_user_sgprs,
           tes->args.ac.ring_offsets.arg_index,
           tes->args.ac.args[tes->args.ac.ring_offsets.arg_index].offset,
           tes->args.num_user_sgprs,
           tes->args.ngg_lds_layout.arg_index,
           tes->args.ac.args[tes->args.ngg_lds_layout.arg_index].offset,
           tes->args.user_sgprs_locs.shader_data[AC_UD_NGG_LDS_LAYOUT].sgpr_idx);
    assert(hs->args.ac.merged_wave_info.used && hs->args.ac.tcs_factor_offset.used);
    unsigned abi[3][3]={{0}};
    for(unsigned i=0;i<3;++i) abi_constants(&ci,&gfx,&stages[ids[i]],abi[i]);
    assert(abi[0][0] && abi[0][0]==abi[1][0]);
    assert(abi[1][1] && !(abi[1][1]&255) && abi[1][1]==abi[2][1]);
    assert(abi[1][2] && abi[1][2]==abi[2][2]);
    printf("actual ABI constants: LS/HS stride=%u HS/TES attribute=%u patch_offset=%u footprint=%u\n",
           abi[0][0],abi[1][1],abi[1][2],footprint);
    for(unsigned i=0;i<3;++i) {
        struct radv_shader_stage io=stages[ids[i]];
        io.nir=nir_shader_clone(NULL,io.nir);
        assert(radv_nir_lower_io_to_mem(&ci,&io));
        nir_validate_shader(io.nir,"actual tess IO to memory"); dump(io.nir,"io-memory");
        if(i==0 && cross) assert(count(io.nir,nir_intrinsic_store_shared));
        if(i==1) {
            assert(ring_access(io.nir,nir_intrinsic_store_buffer_amd,nir_intrinsic_load_ring_tess_offchip_amd));
            assert(ring_access(io.nir,nir_intrinsic_store_buffer_amd,nir_intrinsic_load_ring_tess_factors_amd));
            if(cross) assert(count(io.nir,nir_intrinsic_load_shared));
        }
        if(i==2) assert(ring_access(io.nir,nir_intrinsic_load_buffer_amd,nir_intrinsic_load_ring_tess_offchip_amd));
        ralloc_free(io.nir);
    }
    puts("postprocess");
    for(unsigned i=0;i<3;++i) {
        struct radv_shader_stage *s=&stages[ids[i]];
        radv_postprocess_nir(&ci,&gfx,s); nir_validate_shader(s->nir,"postprocess"); dump(s->nir,"final");
        /* ac_nir filter_load_tcs_per_vertex_input intentionally retains the
         * same-invocation temp path; ACO load_input_from_temps consumes it.
         * Only the linked TCS temp-mask locations with constant IO offsets
         * are allowed to remain. LDS/TES inputs must have been lowered. */
        if(i==1 && !cross) {
            assert(count(s->nir,nir_intrinsic_load_per_vertex_input));
            nir_foreach_function_impl(impl,s->nir) nir_foreach_block(block,impl) nir_foreach_instr(instr,block) {
                if(instr->type!=nir_instr_type_intrinsic) continue;
                nir_intrinsic_instr *input=nir_instr_as_intrinsic(instr);
                if(input->intrinsic!=nir_intrinsic_load_per_vertex_input) continue;
                assert(s->info.vs.tcs_in_out_eq);
                assert(s->info.vs.tcs_inputs_via_temp & BITFIELD64_BIT(nir_intrinsic_io_semantics(input).location));
                assert(nir_src_is_const(*nir_get_io_offset_src(input)));
            }
        } else assert(!count(s->nir,nir_intrinsic_load_per_vertex_input));
        assert(!count(s->nir,nir_intrinsic_store_per_vertex_output));
        assert(!count(s->nir,nir_intrinsic_load_ring_tess_offchip_amd));
        assert(!count(s->nir,nir_intrinsic_load_ring_tess_factors_amd));
    }
    assert(cross ? !count(vs->nir,nir_intrinsic_store_output) : count(vs->nir,nir_intrinsic_store_output)>0);
    gfx10_get_ngg_info(&ci,&tes->info,NULL,&tes->info.ngg_info);
    tes->info.nir_shared_size=tes->info.ngg_info.lds_size;
    nir_shader *merged[]={vs->nir,hs->nir};
    puts("ACO merged VS+TCS count=2");
    struct radv_shader_binary *hs_binary=radv_shader_nir_to_asm(&ci,hs,merged,2,&gfx); assert(hs_binary);
    puts("ACO TES count=1 NGG");
    struct radv_shader_binary *tes_binary=radv_shader_nir_to_asm(&ci,tes,&tes->nir,1,&gfx); assert(tes_binary);
    struct radv_shader_binary_legacy *hb=(void *)hs_binary, *tb=(void *)tes_binary;
    assert(hb->code_size && hb->exec_size && tb->code_size && tb->exec_size);
    assert(reference.hs.machine_code_size==hb->code_size);
    assert(reference.tes.machine_code_size==tb->code_size);
    assert(!memcmp(reference.hs.machine_code,hb->data+hb->stats_size,hb->code_size));
    assert(!memcmp(reference.tes.machine_code,tb->data+tb->stats_size,tb->code_size));
    psbc_free_tessellation_output(&reference);
    puts("PASS public API matches extracted RADV reference code byte-for-byte");
    assert(radv_select_hw_stage(&hs->info,ac.gfx_level)==AC_HW_HULL_SHADER);
    assert(radv_select_hw_stage(&tes->info,ac.gfx_level)==AC_HW_NEXT_GEN_GEOMETRY_SHADER);
    printf("PASS %s: linked merged2 HS=%u bytes TES/NGG=%u bytes; runtime state pinned\n",argv[1],hb->code_size,tb->code_size);
    free(hs_binary); free(tes_binary);
    for(unsigned i=0;i<3;++i) ralloc_free(stages[ids[i]].nir);
    psbc_shutdown(); return 0;
}
'''


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(command, log, timeout, cwd):
    with log.open("w") as stream:
        stream.write("command: " + repr(command) + "\n")
        stream.flush()
        try:
            result = subprocess.run(command, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=timeout, env={**os.environ, "NIR_DEBUG": "validate"})
            status = result.returncode
        except subprocess.TimeoutExpired:
            status = 124
        stream.write(f"\nexit={status}\n")
    print(str(log) + "\n" + "\n".join(log.read_text().splitlines()[1:]), flush=True)
    return status


if __name__ == "__main__":
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix="linked-tess-") as directory:
        attempt = Path(directory)
        print("Linked tessellation compiler regression (host only)", flush=True)
        before = digest(LIB)
        (attempt / "archive.sha256").write_text(before + "  " + str(LIB) + "\n")
        cfile = attempt / "probe.c"
        cfile.write_text(code)
        obj, executable = attempt / "probe.o", attempt / "probe"
        command = ["clang-18", "-std=gnu11", "-O1", "-g", "-Wall", "-Werror",
                   "-DHAVE_ENDIAN_H=1", "-DHAVE_FUNC_ATTRIBUTE_PACKED=1", "-DHAVE_PTHREAD=1",
                   "-DHAVE_STRUCT_TIMESPEC=1", "-DHAVE_FUNC_ATTRIBUTE_UNUSED=1", "-D_GNU_SOURCE"]
        for include in ("include/mesa", "include", "src", "libpsbc", "src/amd/vulkan",
                        "src/amd/common", "src/compiler/nir", "src/amd/compiler", "src/compiler",
                        "src/amd/common/nir", "src/vulkan/runtime", "src/vulkan/util", "../Vulkan-Headers/include"):
            command += ["-I", str(PSBC / include)]
        command += ["-I", str(ROOT / "src/platform")]
        status = run(command + ["-c", str(cfile), "-o", str(obj)], attempt / "compile.log", 45, attempt)
        package_obj = attempt / "package.o"
        if not status:
            status = run(command + ["-c", str(ROOT / "src/platform/ps5_agc_package.c"),
                                    "-o", str(package_obj)], attempt / "package-compile.log", 45, attempt)
        if not status:
            status = run(["g++", "-o", str(executable), str(obj), str(package_obj), str(LIB), "-pthread", "-lm"],
                         attempt / "link.log", 45, attempt)
        if not status:
            for variant in ("same", "cross"):
                directory = attempt / variant
                directory.mkdir()
                status |= run([str(executable), variant], directory / "run.log", 30, directory)
        assert digest(LIB) == before, "host archive changed during probe"
        print(f"RESULT exit={status}; archive unchanged (host only)", flush=True)
        sys.exit(1 if status else 0)
