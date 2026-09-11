#!/usr/bin/env python3
# PS5 OpenGL - OpenGL implementation for PlayStation 5.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later

"""Linked RADV/ACO compiler regression; no native execution or ABI claim."""
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
        nir_def *id=nir_u2f32(&b,nir_load_vertex_id_zero_base(&b));
        nir_def *p=nir_vec4(&b,id,nir_fmul(&b,id,id),nir_fsin(&b,id),nir_imm_float(&b,1));
        nir_store_var(&b,varying(&b,nir_var_shader_out,glsl_vec4_type(),VARYING_SLOT_POS,false),p,15);
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
int main(int argc,char **argv) {
    assert(argc==2); setvbuf(stdout,NULL,_IONBF,0);
    bool cross=!strcmp(argv[1],"cross");
    puts("HOST MODEL ONLY: capacity=8192 dwords, address32_hi=2; no native ABI proposal");
    psbc_init();
    struct ac_compiler_info ac={0}; setup_ac_info(&ac,GFX10_3);
    ac.hs_offchip_workgroup_dw_size=8192; /* Hypothetical host capacity; see ac_fill_tess_info. */
    struct radv_compiler_info ci={.ac=&ac};
    ci.key.family=ci.debug.family=CHIP_NAVI21;
    ci.key.ge_wave_size=64; ci.key.ps_wave_size=32; ci.key.use_ngg=true;
    ci.hw.address32_hi=2; radv_get_nir_options(&ci);
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
    assert(radv_select_hw_stage(&hs->info,ac.gfx_level)==AC_HW_HULL_SHADER);
    assert(radv_select_hw_stage(&tes->info,ac.gfx_level)==AC_HW_NEXT_GEN_GEOMETRY_SHADER);
    printf("PASS %s: linked merged2 HS=%u bytes TES/NGG=%u bytes; compiler-model only\n",argv[1],hb->code_size,tb->code_size);
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
        status = run(command + ["-c", str(cfile), "-o", str(obj)], attempt / "compile.log", 45, attempt)
        if not status:
            status = run(["g++", "-o", str(executable), str(obj), str(LIB), "-pthread", "-lm"],
                         attempt / "link.log", 45, attempt)
        if not status:
            for variant in ("same", "cross"):
                directory = attempt / variant
                directory.mkdir()
                status |= run([str(executable), variant], directory / "run.log", 30, directory)
        assert digest(LIB) == before, "host archive changed during probe"
        print(f"RESULT exit={status}; archive unchanged (host only)", flush=True)
        sys.exit(1 if status else 0)
