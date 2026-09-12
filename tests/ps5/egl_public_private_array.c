// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "ps5_screen.h"
#include "ps5_agc_package.h"
#include "private_array_fixture.h"
#ifndef PS5_PRIVATE_BUFFER_TEST
#define PS5_PRIVATE_BUFFER_TEST 0
#endif
#if PS5_PRIVATE_BUFFER_TEST
#include "private_buffer_fixture.h"
#endif

#define COUNT 1024u
#define GUARDS 32u
#define SENTINEL UINT32_C(0xcdcdcdcd)
#define MAX_PRIVATE_WORDS (PS5_PRIVATE_BUFFER_TEST?1024u:32u)

int main(void) {
    int status=1;
    struct pipe_screen *screen=ps5_screen_create();
    struct pipe_resource *resources[4]={0};
    void *memory[4]={0};
    PsbcShaderOutput compiled={0};
    if (!screen) return 1;
    psbc_init();
    const unsigned sizes[]={16*16, (COUNT+2*GUARDS)*4, (3*COUNT+2*GUARDS)*4,
        (COUNT*(MAX_PRIVATE_WORDS+2)+2*GUARDS)*4};
    for (unsigned i=0;i<3+PS5_PRIVATE_BUFFER_TEST;++i) {
        struct pipe_resource templ={.target=PIPE_BUFFER,.format=PIPE_FORMAT_R8_UNORM,
            .width0=sizes[i],.height0=1,.depth0=1,.array_size=1,
            .usage=PIPE_USAGE_DEFAULT,.bind=PIPE_BIND_SHADER_BUFFER};
        resources[i]=screen->resource_create(screen,&templ);
        if (!resources[i] || ps5_resource_info(resources[i],&memory[i],NULL,NULL)) goto done;
    }
    PsbcCompileOptions opts={.target=PSBC_TARGET_PS5,.stage=PSBC_STAGE_COMPUTE,
        .optimise=true,.address32_hi=(uintptr_t)memory[0]>>32,.gallium_buffer_arrays=true,
        .descriptor_binding_count=1,.descriptor_bindings={{
            .binding=PSBC_GALLIUM_SSBO_ARRAY_BINDING(PSBC_STAGE_COMPUTE),
            .type=PSBC_DESCRIPTOR_STORAGE_BUFFER,.array_size=16,.stride=16}}};
    uint32_t *table=memory[0], *output=(uint32_t *)memory[1]+GUARDS;
    uint32_t *input=(uint32_t *)memory[2]+GUARDS;
    memset(table,0,sizes[0]);
    for (unsigned slot=0;slot<2;++slot) {
        uintptr_t address=(uintptr_t)memory[slot+1]+GUARDS*4;
        table[slot*4]=address; table[slot*4+1]=address>>32;
        table[slot*4+2]=COUNT*4*(slot?3:1); table[slot*4+3]=UINT32_C(0x31016fac);
    }
    const unsigned cases[]={4,8,16,32,
#if PS5_PRIVATE_BUFFER_TEST
        64,256,1024,
#endif
        0};
    for (unsigned test=0;test<sizeof(cases)/sizeof(cases[0]);++test) {
        const unsigned words=cases[test];
        memset(memory[1],0xcd,sizes[1]); memset(memory[2],0xcd,sizes[2]);
#if PS5_PRIVATE_BUFFER_TEST
        memset(memory[3],0xcd,sizes[3]);
        uintptr_t private_address=(uintptr_t)memory[3]+GUARDS*4;
        table[8]=private_address; table[9]=private_address>>32;
        table[10]=COUNT*(words+2)*4; table[11]=UINT32_C(0x31016fac);
#endif
        for (unsigned i=0;i<COUNT;++i) {
            input[i*3]=words?i%words:0;
            input[i*3+1]=words?(i/words+17*i)%words:0;
            input[i*3+2]=UINT32_C(0xfffff000)+i*UINT32_C(2654435761);
        }
        nir_builder b;
        if (words) {
            b=private_array_fixture(words);
#if PS5_PRIVATE_BUFFER_TEST
            private_buffer_lower(b.shader);
#else
            private_array_lower(b.shader);
#endif
        } else {
            b=nir_builder_init_simple_shader(MESA_SHADER_COMPUTE,
                psbc_get_nir_options(PSBC_STAGE_COMPUTE),"private-array-post-control");
            b.shader->info.workgroup_size[0]=16;
            b.shader->info.workgroup_size[1]=b.shader->info.workgroup_size[2]=1;
            b.shader->info.num_ssbos=1;
            nir_def *id=nir_iadd(&b,nir_channel(&b,nir_load_local_invocation_id(&b),0),
                nir_imul_imm(&b,nir_channel(&b,nir_load_workgroup_id(&b),0),16));
            nir_store_ssbo(&b,nir_iadd_imm(&b,nir_imul_imm(&b,id,3),17),
                nir_imm_int(&b,0),nir_imul_imm(&b,id,4),.align_mul=4,.write_mask=1);
        }
        int rc=psbc_compile_nir(b.shader,&opts,&compiled);
        ralloc_free(b.shader);
        printf("[ps5-private-array] buffer=%u bytes=%u compile=%d scratch=%u code=%zu\n",
            PS5_PRIVATE_BUFFER_TEST,words*4,rc,compiled.metadata.scratch_bytes_per_wave,compiled.machine_code_size);
        fflush(stdout);
        if (rc || compiled.metadata.scratch_valid || compiled.metadata.scratch_bytes_per_wave ||
            compiled.metadata.scratch_size_per_thread) goto done;
        const uint32_t groups[]={COUNT/16,1,1};
        rc=ps5_agc_compute_execute(screen,&compiled,resources[0],resources+1,
            2+PS5_PRIVATE_BUFFER_TEST,groups);
        unsigned correct=0, guards=0, unchanged=0;
        for (unsigned i=0;i<COUNT;++i) {
            const uint32_t seed=UINT32_C(0xfffff000)+i*UINT32_C(2654435761);
            const unsigned w=words?i%words:0, r=words?(i/words+17*i)%words:0;
            const uint32_t expected=words?(r==w?seed+991:seed^(17+37*r)):i*3+17;
            correct+=output[i]==expected;
            unchanged+=input[i*3]==w && input[i*3+1]==r && input[i*3+2]==seed;
        }
        for (unsigned i=0;i<GUARDS;++i) {
            guards+=((uint32_t *)memory[1])[i]==SENTINEL;
            guards+=output[COUNT+i]==SENTINEL;
            guards+=((uint32_t *)memory[2])[i]==SENTINEL;
            guards+=input[3*COUNT+i]==SENTINEL;
        }
        printf("[ps5-private-array] bytes=%u rc=%d correct=%u/%u input=%u/%u guards=%u/%u\n",
            words*4,rc,correct,COUNT,unchanged,COUNT,guards,4*GUARDS);
        fflush(stdout);
        if (rc || correct!=COUNT || unchanged!=COUNT || guards!=4*GUARDS) goto done;
#if PS5_PRIVATE_BUFFER_TEST
        unsigned private_correct=0;
        const unsigned used=COUNT*(words+2);
        for (unsigned i=0;i<sizes[3]/4;++i) {
            uint32_t expected=SENTINEL;
            if (words && i>=GUARDS && i-GUARDS<used) {
                unsigned lane=(i-GUARDS)/(words+2), word=(i-GUARDS)%(words+2);
                if (word>0 && word<=words) {
                    uint32_t seed=UINT32_C(0xfffff000)+lane*UINT32_C(2654435761);
                    expected=word-1==lane%words?seed+991:seed^(17+37*(word-1));
                }
            }
            private_correct+=((uint32_t *)memory[3])[i]==expected;
        }
        printf("[ps5-private-buffer] bytes=%u storage-and-guards=%u/%u\n",
            words*4,private_correct,sizes[3]/4);
        fflush(stdout);
        if (private_correct!=sizes[3]/4) goto done;
#endif
        psbc_free_output(&compiled); memset(&compiled,0,sizeof(compiled));
    }
    status=0;
done:
    psbc_free_output(&compiled);
    for (unsigned i=0;i<4;++i) pipe_resource_reference(&resources[i],NULL);
    screen->destroy(screen); psbc_shutdown();
    printf("[ps5-private-array] completed status=%d\n",status);
    return status;
}
