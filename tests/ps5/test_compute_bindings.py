#!/usr/bin/env python3
# PS5 OpenGL - OpenGL implementation for PlayStation 5.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run actual Gallium CS binding/dispatch validation with native submission mocked."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MESA = ROOT / "third_party/mesa-26.2.0"
source = (ROOT / "src/gallium/ps5/ps5_screen.c").read_text()
begin = source.index("static void\nps5_set_shader_buffers(")
functions = source[begin:source.index("\n#endif", begin)]
sampler_at = source.index("static bool\nps5_texture_descriptor_wrap(")
sampler_helpers = source[sampler_at:source.index("static bool\nps5_float_is_finite(", sampler_at)]
extent_at = source.index("static unsigned\nps5_linear_mip_storage_extent(")
extent_helper = source[extent_at:source.index("static bool\nps5_packed_depth_sample_layout(", extent_at)]
image_at = source.index("static int\nps5_resource_linear_image_descriptor(")
image_descriptor = source[image_at:source.index("\nstruct pipe_resource *", image_at)]
array_at = source.index("static bool\nps5_compute_image_array_resource(")
array_layout = source[array_at:source.index("static unsigned\nps5_texture_format_size(", array_at)]
barrier_at = source.index("static void\nps5_memory_barrier(")
barrier = source[barrier_at:source.index("static bool\nps5_draw_primitive(", barrier_at)]
fragment_at = source.index("static unsigned\nps5_shader_storage_count(")
fragment = source[fragment_at:source.index("static bool\nps5_prepare_constant(", fragment_at)]
constant_at = source.index("static bool\nps5_prepare_constant(")
constant = source[constant_at:source.index("static bool\nps5_prepare_texture(", constant_at)]
assert "context->base.memory_barrier = ps5_memory_barrier;" in source
code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "psbc_compile.h"
#include "ps5_agc_package.h"
#include "amd/common/amdgfxregs.h"
#define PS5_ENABLE_UBO_CANDIDATE 1
#define PS5_MAX_CONSTANT_BUFFERS 13
#define PS5_CONSTANT_DATA_OFFSET 2048
#define PS5_DESCRIPTOR_STORAGE_BYTES (2048+2*PS5_MAX_CONSTANT_BUFFER_SIZE)
#define PS5_FRAGMENT_UBO_OFFSET 512
#define PS5_TEXTURE_DESCRIPTOR_BYTES 1536
#define PS5_DIRECT_ALIGNMENT 16384
#define PS5_GEOMETRY_CONSTANT_SLOT 2
#define PS5_COMPUTE_STORAGE_SLOTS 16
#define PS5_COMPUTE_CONSTANT_SLOTS 15
#define PS5_COMPUTE_BUFFER_SLOTS 31
#define PS5_COMPUTE_IMAGE_SLOTS 8
#define PS5_COMPUTE_TEXTURE_SLOTS PS5_AGC_COMPUTE_MAX_TEXTURES
#define PS5_COMPUTE_TEXTURE_OFFSET (31*16+8*32)
#define PS5_COMPUTE_DESCRIPTOR_BYTES (PS5_COMPUTE_TEXTURE_OFFSET+PS5_COMPUTE_TEXTURE_SLOTS*48)
#define PS5_MAX_TEXTURE_2D_SIZE 8192
#define PS5_MAX_CONSTANT_BUFFER_SIZE 0x4000u
struct ps5_resource {
    struct pipe_resource base; uint8_t *data;
    size_t size, render_staging_size, depth_staging_size, layer_stride;
    unsigned level_stride[PIPE_MAX_TEXTURE_LEVELS];
    size_t level_offset[PIPE_MAX_TEXTURE_LEVELS];
};
struct ps5_compute_shader { PsbcShaderOutput output; unsigned textures, filtered_textures, texture_lod[PS5_COMPUTE_TEXTURE_SLOTS], array_textures; };
struct ps5_sampler_state { struct pipe_sampler_state base; };
struct test_nir { struct { unsigned num_ssbos, num_images, num_ubos, num_textures; bool first_ubo_is_default_ubo; } info; };
struct test_variant { PsbcShaderOutput output; };
struct ps5_shader { struct test_nir *nir; struct test_variant *active; };
static unsigned ps5_shader_texture_count(const struct ps5_shader *shader) { return shader->nir->info.num_textures; }
struct ps5_constant_state { bool valid, copied; unsigned size, offset; struct pipe_resource *buffer; };
struct ps5_context {
    struct pipe_context base;
    struct ps5_compute_shader *cs;
    struct pipe_resource *compute_descriptors;
    struct pipe_shader_buffer compute_buffers[31];
    struct pipe_shader_buffer fragment_buffers[16];
    bool fragment_bindings_invalid;
    struct ps5_shader *fs;
    struct ps5_shader *gs;
    struct ps5_constant_state constants[3][13];
    struct pipe_resource *descriptor_storage[2];
    struct pipe_image_view compute_images[8];
    struct pipe_image_view fragment_images[8];
    bool fragment_images_invalid;
    struct pipe_sampler_view *compute_views[PS5_COMPUTE_TEXTURE_SLOTS];
    bool compute_views_invalid;
    uint32_t compute_samplers[PS5_COMPUTE_TEXTURE_SLOTS][4];
    unsigned compute_sampler_mask;
    bool compute_samplers_invalid;
    bool compute_images_invalid;
    bool compute_bindings_invalid;
    uint32_t compute_constants_invalid;
    int last_compute_status;
    unsigned dispatches;
};
static unsigned submitted, destroyed;
static unsigned with_images;
static bool with_sampled;
static unsigned sampled_count=8;
static uint32_t expected_sampler[4];
static unsigned with_filtered;
static bool multi, with_constants, fail_upload;
static bool fail_info;
static struct ps5_resource *upload_resource;
static bool ps5_texture_descriptor_format(enum pipe_format f,uint32_t *word) {
    assert(f==PIPE_FORMAT_R32_UINT || f==PIPE_FORMAT_R32_SINT || f==PIPE_FORMAT_R32_FLOAT);
    *word=f==PIPE_FORMAT_R32_UINT ? 0x1400000 : f==PIPE_FORMAT_R32_SINT ? 0x1500000 : 0x1600000;
    return true;
}
''' + extent_helper + array_layout + image_descriptor + r'''
static int ps5_resource_info(struct pipe_resource *base, void **address, size_t *size, size_t *allocation) {
    (void)allocation;
    if (fail_info) return -1;
    *address=((struct ps5_resource *)base)->data;
    *size=base->width0;
    return 0;
}
static bool fragment_mode;
static unsigned fragment_drains;
static void ps5_draw_batch_drain(void) { ++fragment_drains; }
static void ps5_flush_gpu_data(const void *address, size_t size) { assert(address && (fragment_mode ? size==64 || size==256 || size==512 || size==768 || size==PS5_DESCRIPTOR_STORAGE_BYTES : size==12)); }
static bool ps5_uses_merged_geometry_metadata(const struct ps5_context *c, const struct ps5_shader *s, const PsbcShaderMetadata *m) { return false; }
static unsigned ps5_texture_count(const struct ps5_context *c, const struct ps5_shader *s, const PsbcShaderMetadata *m) { return s->nir->info.num_textures; }
static unsigned ps5_constant_state_binding(const struct ps5_shader *s, unsigned i) { return i+!s->nir->info.first_ubo_is_default_ubo; }
static size_t ps5_copied_constant_offset(unsigned slot) { return 2048+(slot==2 ? PS5_MAX_CONSTANT_BUFFER_SIZE : 0); }
void u_upload_data_ref(struct u_upload_mgr *upload, unsigned minimum, unsigned size,
    unsigned alignment, const void *data, unsigned *offset, struct pipe_resource **buffer) {
    assert(upload && !minimum && alignment==16 && size<=64 && upload_resource);
    if (fail_upload) return;
    *offset=32;
    memcpy(upload_resource->data+32,data,size);
    pipe_resource_reference(buffer,&upload_resource->base);
}
void u_upload_unmap(struct u_upload_mgr *upload) { assert(upload); }
static void destroy(struct pipe_screen *s, struct pipe_resource *r) {
    assert(s && r && !r->reference.count); ++destroyed;
}
int ps5_agc_compute_execute(struct pipe_screen *s, const PsbcShaderOutput *shader,
    struct pipe_resource *table, struct pipe_resource *const *buffers, unsigned count,
    const uint32_t groups[3]) {
    assert(s && shader && groups[0]==2 && groups[1]==1 && groups[2]==1);
    if(with_sampled) {
        assert(count==sampled_count);
        const struct ps5_resource *t=(const struct ps5_resource *)table;
        for(unsigned i=0;i<sampled_count;++i) {
            uint32_t expected[12]={0};
            assert(!ps5_resource_sampled_image_descriptor(buffers[i],0,0,expected));
            if (with_filtered & (1u<<i))
                memcpy(expected+8,expected_sampler,16);
            assert(!memcmp(t->data+PS5_COMPUTE_TEXTURE_OFFSET+i*48,expected,48));
        }
        ++submitted; return 0;
    }
    if(with_images) {
        assert(count==with_images);
        struct ps5_resource *t=(struct ps5_resource *)table;
        for(unsigned i=0;i<8;++i) {
            uint32_t expected[8]={0};
            if(with_images==39 || i==7)
                assert(!ps5_resource_storage_image_descriptor(buffers[count-1],0,expected));
            assert(!memcmp(t->data+31*16+i*32,expected,32));
        }
        ++submitted; return 0;
    }
    assert(count==(with_constants ? 31 : multi ? 16 : 1));
    struct ps5_resource *t=(struct ps5_resource *)table, *b=(struct ps5_resource *)buffers[0];
    if (multi) {
        assert(b->base.reference.count==(with_constants ? 30 : 15) && !destroyed);
        for (unsigned i=0; i<16; ++i) {
            struct ps5_resource *r=(struct ps5_resource *)buffers[i];
            const uint32_t *d=(uint32_t *)t->data+i*4;
            const uintptr_t address=(uintptr_t)r->data+(i==15 ? 16 : i*16);
            assert(i==15 ? r->base.reference.count==1 : r==b);
            assert(d[0]==(uint32_t)address && d[1]==address>>32);
            assert(d[2]==(i==15 ? 64 : 16) && d[3]==0x31016fac);
        }
        for (unsigned i=0; i<15; ++i) {
            const uint32_t *d=(uint32_t *)t->data+(16+i)*4;
            if (with_constants) {
                const uintptr_t address=(uintptr_t)b->data+i*16;
                assert(buffers[16+i]==&b->base);
                assert(d[0]==(uint32_t)address && d[1]==address>>32);
                assert(d[2]==16 && d[3]==0x31016fac);
            } else assert(!(d[0]|d[1]|d[2]|d[3]));
        }
        ++submitted; return 0;
    }
    uint32_t *srd=(uint32_t *)t->data+15*4;
    assert(b->base.reference.count==1 && !destroyed);
    assert(srd[0]==(uint32_t)(uintptr_t)(b->data+16) && srd[1]==(uintptr_t)b->data>>32);
    assert(srd[2]==64 && srd[3]==0x31016fac);
    for (unsigned i=0; i<15*4; ++i) assert(((uint32_t *)t->data)[i]==0);
    ++submitted; return 0;
}
static unsigned barrier_flushes;
static void ps5_flush(struct pipe_context *context, struct pipe_fence_handle **fence, unsigned flags) {
    assert(context && !fence && !flags); ++barrier_flushes;
}
''' + barrier + fragment + constant + '\n#define PS5_ENABLE_BORDER_COLOR_CANDIDATE 1\n' + sampler_helpers + functions + r'''
static void fragment_contract(void) {
    struct pipe_screen screen={.resource_destroy=destroy};
    uint32_t words[64]={0}, descriptors[128]={0}, userdata[16]={0};
    struct ps5_resource data={.base={.screen=&screen,.target=PIPE_BUFFER,.width0=256},.data=(void *)words,.size=256};
    struct ps5_resource table={.data=(void *)descriptors,.size=512};
    pipe_reference_init(&data.base.reference,1);
    struct test_nir nir={.info.num_ssbos=16};
    struct test_variant variant={0};
    struct ps5_shader shader={&nir,&variant};
    PsbcShaderMetadata *m=&variant.output.metadata;
    m->address32_hi=(uintptr_t)descriptors>>32;
    m->descriptor_set0_valid=true; m->descriptor_binding_count=1;
    m->descriptor_bindings[0]=(PsbcDescriptorBinding){.binding=PSBC_GALLIUM_SSBO_ARRAY_BINDING(PSBC_STAGE_FRAGMENT),
        .type=PSBC_DESCRIPTOR_STORAGE_BUFFER,.array_size=16,.stride=16};
    struct ps5_context c={.base.screen=&screen,.fs=&shader,.descriptor_storage[1]=&table.base};
    struct pipe_shader_buffer bindings[16];
    for(unsigned i=0;i<16;++i) bindings[i]=(struct pipe_shader_buffer){&data.base,16,64};
    fragment_mode=true;
    assert(!ps5_prepare_fragment_storage(&c,userdata,16)); /* Missing bank. */
    ps5_set_shader_buffers(&c.base,MESA_SHADER_FRAGMENT,0,16,bindings,65535);
    assert(fragment_drains==1 && data.base.reference.count==17 && !c.compute_buffers[0].buffer);
    assert(ps5_prepare_fragment_storage(&c,userdata,16));
    for(unsigned i=0;i<16;++i) {
        assert(descriptors[i*4]==(uint32_t)(uintptr_t)((uint8_t *)words+16));
        assert(descriptors[i*4+2]==64 && descriptors[i*4+3]==0x31016fac);
    }
    assert(userdata[0]==(uint32_t)(uintptr_t)descriptors);
    /* Actual uniform preparation must preserve storage banks and copied data. */
    uint8_t mixed_table[PS5_DESCRIPTOR_STORAGE_BYTES]; memset(mixed_table,0xab,sizeof(mixed_table));
    table.data=mixed_table; table.size=sizeof(mixed_table);
    m->address32_hi=(uintptr_t)mixed_table>>32;
    m->descriptor_binding_count=2;
    m->descriptor_bindings[1]=(PsbcDescriptorBinding){.binding=PSBC_GALLIUM_UBO_ARRAY_BINDING(PSBC_STAGE_FRAGMENT),
        .type=PSBC_DESCRIPTOR_UNIFORM_BUFFER,.array_size=13,.offset=512,.stride=16};
    nir.info.num_ubos=13; nir.info.first_ubo_is_default_ubo=true;
    for(unsigned i=0;i<13;++i) c.constants[1][i]=(struct ps5_constant_state){.valid=true,.size=64,.buffer=&data.base};
    c.constants[1][0].copied=true;
    assert(ps5_prepare_fragment_storage(&c,userdata,16));
    uint8_t preserved[512]; memcpy(preserved,mixed_table,512);
    assert(ps5_prepare_constant(&c,&shader,1,userdata,16,NULL));
    assert(!memcmp(preserved,mixed_table,512));
    for(unsigned i=0;i<13;++i) {
        const uint32_t *d=(const uint32_t *)(mixed_table+512+i*16);
        assert(d[0]==(uint32_t)(uintptr_t)(i ? (void *)words : mixed_table+2048) && d[2]==64);
    }
    for(unsigned i=720;i<sizeof(mixed_table);++i) assert(mixed_table[i]==0xab);
    c.constants[1][12].valid=false;
    assert(!ps5_prepare_constant(&c,&shader,1,userdata,16,NULL));
    c.constants[1][12].valid=true;
    m->descriptor_bindings[1].offset=496;
    assert(!ps5_prepare_constant(&c,&shader,1,userdata,16,NULL));
    nir.info.num_ubos=0; m->descriptor_binding_count=1;
    table.data=(void *)descriptors; table.size=sizeof(descriptors);
    m->address32_hi=(uintptr_t)descriptors>>32;
    bindings[15].buffer_size=UINT32_MAX;
    ps5_set_shader_buffers(&c.base,MESA_SHADER_FRAGMENT,0,16,bindings,65535);
    assert(c.fragment_bindings_invalid && data.base.reference.count==17 && fragment_drains==1);
    assert(!ps5_prepare_fragment_storage(&c,userdata,16));
    bindings[15].buffer_size=64;
    ps5_set_shader_buffers(&c.base,MESA_SHADER_FRAGMENT,0,16,bindings,65535);
    const PsbcShaderMetadata good=*m;
    for(unsigned fault=0;fault<5;++fault) {
        *m=good;
        if(fault==0) m->descriptor_set0_valid=false;
        if(fault==1) m->descriptor_set0_user_data_dword=16;
        if(fault==2) m->descriptor_bindings[0].offset=16;
        if(fault==3) m->descriptor_bindings[0].array_size=15;
        if(fault==4) m->address32_hi^=1;
        assert(!ps5_prepare_fragment_storage(&c,userdata,16));
    }
    *m=good;
    ps5_set_shader_buffers(&c.base,MESA_SHADER_FRAGMENT,0,16,NULL,0);
    assert(data.base.reference.count==1 && !ps5_prepare_fragment_storage(&c,userdata,16));
    nir.info.num_ssbos=0; nir.info.num_images=8;
    m->descriptor_binding_count=2;
    m->descriptor_bindings[1]=(PsbcDescriptorBinding){.binding=PSBC_GALLIUM_IMAGE_ARRAY_BINDING(PSBC_STAGE_FRAGMENT),
        .type=PSBC_DESCRIPTOR_STORAGE_IMAGE,.array_size=8,.offset=256,.stride=32};
    _Alignas(256) uint8_t pixels[768];
    struct ps5_resource image={.base={.screen=&screen,.target=PIPE_TEXTURE_2D,
        .format=PIPE_FORMAT_R32_UINT,.width0=17,.height0=3,.depth0=1,.array_size=1,
        .bind=PIPE_BIND_SHADER_IMAGE|PIPE_BIND_SAMPLER_VIEW},.data=pixels,.size=768,.level_stride={256}};
    pipe_reference_init(&image.base.reference,1);
    struct pipe_image_view views[8];
    for(unsigned i=0;i<8;++i) views[i]=(struct pipe_image_view){.resource=&image.base,
        .format=PIPE_FORMAT_R32_UINT,.access=PIPE_IMAGE_ACCESS_READ_WRITE};
    assert(!ps5_prepare_fragment_storage(&c,userdata,16));
    ps5_set_shader_images(&c.base,MESA_SHADER_FRAGMENT,0,8,0,views);
    assert(image.base.reference.count==9 && !c.compute_images[0].resource);
    assert(ps5_prepare_fragment_storage(&c,userdata,16));
    for(unsigned i=0;i<8;++i) assert(descriptors[64+i*8]==(uint32_t)((uintptr_t)pixels>>8));
    views[7].u.tex.level=1;
    ps5_set_shader_images(&c.base,MESA_SHADER_FRAGMENT,0,8,0,views);
    assert(c.fragment_images_invalid && image.base.reference.count==9);
    assert(!ps5_prepare_fragment_storage(&c,userdata,16));
    views[7].u.tex.level=0;
    ps5_set_shader_images(&c.base,MESA_SHADER_FRAGMENT,0,8,0,views);
    m->descriptor_bindings[1].offset=128;
    assert(!ps5_prepare_fragment_storage(&c,userdata,16));
    m->descriptor_bindings[1].offset=256;
    ps5_set_shader_images(&c.base,MESA_SHADER_FRAGMENT,0,0,8,NULL);
    assert(image.base.reference.count==1 && !ps5_prepare_fragment_storage(&c,userdata,16));
    fragment_mode=false;
}
int main(void) {
    fragment_contract();
    assert(PS5_COMPUTE_TEXTURE_SLOTS==16 && PS5_AGC_COMPUTE_MAX_RESOURCES==55);
    struct pipe_screen screen={.resource_destroy=destroy}, other_screen={0};
    struct pipe_context barrier_context={0};
    ps5_memory_barrier(&barrier_context,0);
    assert(!barrier_flushes);
    for(unsigned mask=1;mask<=PIPE_BARRIER_ALL;++mask) {
        ps5_memory_barrier(&barrier_context,mask);
        assert(barrier_flushes==mask);
    }
    uint8_t table_data[PS5_COMPUTE_DESCRIPTOR_BYTES], output[256];
    struct ps5_resource table={.data=table_data};
    struct ps5_resource buffer={.base={.screen=&screen,.target=PIPE_BUFFER,.width0=256},.data=output};
    pipe_reference_init(&buffer.base.reference, 1);
    struct ps5_compute_shader cs={0};
    cs.output.metadata.compute_workgroup_size[0]=16;
    cs.output.metadata.compute_workgroup_size[1]=cs.output.metadata.compute_workgroup_size[2]=1;
    struct ps5_context context={.base={.screen=&screen},.cs=&cs,.compute_descriptors=&table.base};
    struct pipe_shader_buffer binding={&buffer.base,16,64};
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,15,1,&binding,1);
    assert(!context.compute_bindings_invalid && buffer.base.reference.count==2);
    struct pipe_resource *caller=&buffer.base;
    pipe_resource_reference(&caller,NULL);
    assert(buffer.base.reference.count==1 && !destroyed);
    /* Rebind while the binding itself owns the last reference. */
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,15,1,&binding,1);
    assert(buffer.base.reference.count==1 && !destroyed);
    const struct pipe_grid_info good={.work_dim=1,.block={16,1,1},.grid={2,1,1}};
    ps5_launch_grid(&context.base,&good);
    assert(!context.last_compute_status && context.dispatches==1 && submitted==1);
    struct pipe_shader_buffer pair[2]={binding,binding};
    pair[1].buffer_size=UINT32_MAX;
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,14,2,pair,3);
    assert(context.compute_bindings_invalid && !context.compute_buffers[14].buffer);
    assert(buffer.base.reference.count==1 && !destroyed);
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,15,1,&binding,1);
    for (unsigned fault=0; fault<8; ++fault) {
        struct pipe_shader_buffer bad=binding;
        if (fault==0) bad.buffer_offset=UINT32_MAX;
        if (fault==1) bad.buffer_offset=1;
        if (fault==2) bad.buffer_size=UINT32_MAX;
        if (fault==3) bad.buffer_size=0;
        if (fault==4) buffer.base.target=PIPE_TEXTURE_2D;
        if (fault==5) buffer.base.screen=&other_screen;
        unsigned start=fault==6 ? 16 : 15, count=fault==7 ? UINT32_MAX : 1;
        ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,start,count,&bad,1);
        assert(context.compute_bindings_invalid && buffer.base.reference.count==1 && !destroyed);
        assert(context.compute_buffers[15].buffer_offset==16 && context.compute_buffers[15].buffer_size==64);
        ps5_launch_grid(&context.base,&good);
        assert(context.last_compute_status<0 && submitted==1);
        buffer.base.target=PIPE_BUFFER; buffer.base.screen=&screen;
        ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,15,1,&binding,1);
    }
    for (unsigned fault=0; fault<8; ++fault) {
        struct pipe_grid_info bad=good;
        if (fault==0) bad.block[0]=17;
        if (fault==1) bad.grid_base[0]=1;
        if (fault==2) bad.last_block[0]=8;
        if (fault==3) bad.grid[0]=65536;
        if (fault==4) bad.variable_shared_mem=1024;
        if (fault==5) { bad.indirect=&buffer.base; bad.indirect_offset=1; }
        if (fault==6) bad.num_globals=1;
        if (fault==7) bad.work_dim=4;
        ps5_launch_grid(&context.base,&bad);
        assert(context.last_compute_status<0 && submitted==1);
    }
    struct pipe_grid_info empty=good; empty.grid[1]=0;
    ps5_launch_grid(&context.base,&empty);
    assert(!context.last_compute_status && submitted==1);
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,15,1,NULL,0);
    assert(!context.compute_buffers[15].buffer && destroyed==1);
    /* Fifteen nonoverlapping ranges of one retained resource plus output. */
    destroyed=0; multi=true;
    uint8_t input[256];
    struct ps5_resource ranges={.base={.screen=&screen,.target=PIPE_BUFFER,.width0=256},.data=input};
    pipe_reference_init(&ranges.base.reference,1);
    pipe_reference_init(&buffer.base.reference,1);
    struct pipe_shader_buffer bindings[16];
    for (unsigned i=0; i<15; ++i)
        bindings[i]=(struct pipe_shader_buffer){&ranges.base,i*16,16};
    bindings[15]=binding;
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,0,16,bindings,1u<<15);
    caller=&ranges.base; pipe_resource_reference(&caller,NULL);
    caller=&buffer.base; pipe_resource_reference(&caller,NULL);
    ps5_launch_grid(&context.base,&good);
    assert(!context.last_compute_status && submitted==2 && context.dispatches==2);
    for (unsigned i=0; i<15; ++i) {
        struct pipe_constant_buffer cb={.buffer=&ranges.base,.buffer_offset=i*16,.buffer_size=16};
        ps5_set_compute_constant_buffer(&context.base,i,&cb);
    }
    with_constants=true;
    ps5_launch_grid(&context.base,&good);
    assert(!context.last_compute_status && submitted==3 && context.dispatches==3);
    for (unsigned fault=0; fault<7; ++fault) {
        struct pipe_constant_buffer cb={.buffer=&ranges.base,.buffer_offset=224,.buffer_size=16};
        if (fault==0) cb.buffer_offset=UINT32_MAX;
        if (fault==1) cb.buffer_offset=1;
        if (fault==2) cb.buffer_size=UINT32_MAX;
        if (fault==3) cb.buffer_size=0;
        if (fault==4) ranges.base.target=PIPE_TEXTURE_2D;
        if (fault==5) ranges.base.screen=&other_screen;
        if (fault==6) cb.user_buffer=input; /* Ambiguous resource plus user pointer. */
        ps5_set_compute_constant_buffer(&context.base,14,&cb);
        assert(context.compute_constants_invalid==(1u<<14) && ranges.base.reference.count==30);
        assert(context.compute_buffers[30].buffer_offset==224 && context.compute_buffers[30].buffer_size==16);
        ranges.base.target=PIPE_BUFFER; ranges.base.screen=&screen;
        const struct pipe_constant_buffer other={.buffer=&ranges.base,.buffer_size=16};
        ps5_set_compute_constant_buffer(&context.base,0,&other);
        assert(context.compute_constants_invalid==(1u<<14));
        ps5_launch_grid(&context.base,&good);
        assert(context.last_compute_status<0 && submitted==3);
        cb=(struct pipe_constant_buffer){.buffer=&ranges.base,.buffer_offset=224,.buffer_size=16};
        ps5_set_compute_constant_buffer(&context.base,14,&cb);
        assert(!context.compute_constants_invalid);
    }
    for (unsigned i=0; i<15; ++i)
        ps5_set_compute_constant_buffer(&context.base,i,NULL);
    assert(ranges.base.reference.count==15);
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,0,16,NULL,0);
    assert(destroyed==2);
    /* The binding must own a copy after the caller changes stack constants. */
    uint8_t upload_data[256], constants[64]; memset(constants,0x57,sizeof(constants));
    struct ps5_resource uploaded={.base={.screen=&screen,.target=PIPE_BUFFER,.width0=256},.data=upload_data};
    pipe_reference_init(&uploaded.base.reference,1);
    upload_resource=&uploaded;
    context.base.const_uploader=(void *)(uintptr_t)1;
    const struct pipe_constant_buffer user={.user_buffer=constants,.buffer_size=sizeof(constants)};
    ps5_set_compute_constant_buffer(&context.base,0,&user);
    assert(!context.compute_constants_invalid && uploaded.base.reference.count==2);
    assert(context.compute_buffers[16].buffer_offset==32 && context.compute_buffers[16].buffer_size==64);
    memset(constants,0,sizeof(constants));
    for (unsigned i=32; i<96; ++i) assert(upload_data[i]==0x57);
    fail_upload=true;
    ps5_set_compute_constant_buffer(&context.base,0,&user);
    assert(context.compute_constants_invalid==1 && uploaded.base.reference.count==2);
    assert(context.compute_buffers[16].buffer==&uploaded.base);
    ps5_launch_grid(&context.base,&good);
    assert(context.last_compute_status<0 && submitted==3);
    fail_upload=false;
    ps5_set_compute_constant_buffer(&context.base,0,&user);
    assert(!context.compute_constants_invalid && uploaded.base.reference.count==2);
    ps5_set_compute_constant_buffer(&context.base,0,NULL);
    assert(uploaded.base.reference.count==1);
    caller=&uploaded.base; pipe_resource_reference(&caller,NULL);
    assert(destroyed==3);
    /* Indirect dimensions replace the caller's direct grid, after validation. */
    multi=with_constants=false; destroyed=0;
    pipe_reference_init(&buffer.base.reference,1);
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,15,1,&binding,1);
    caller=&buffer.base; pipe_resource_reference(&caller,NULL);
    const uint32_t dimensions[3]={2,1,1};
    memcpy(output+16,dimensions,sizeof(dimensions));
    struct pipe_grid_info indirect=good;
    memset(indirect.grid,0,sizeof(indirect.grid));
    indirect.indirect=&buffer.base; indirect.indirect_offset=16;
    ps5_launch_grid(&context.base,&indirect);
    assert(!context.last_compute_status && submitted==4 && context.dispatches==4);
    for (unsigned fault=0; fault<7; ++fault) {
        struct pipe_grid_info bad=indirect;
        if (fault==0) bad.indirect_offset=UINT32_MAX;
        if (fault==1) bad.indirect_offset=17;
        if (fault==2) bad.indirect_offset=248; /* Eight bytes are not a command. */
        if (fault==3) buffer.base.target=PIPE_TEXTURE_2D;
        if (fault==4) buffer.base.screen=&other_screen;
        if (fault==5) fail_info=true;
        if (fault==6) { uint32_t large=65536; memcpy(output+16,&large,4); }
        ps5_launch_grid(&context.base,&bad);
        assert(context.last_compute_status<0 && submitted==4 && buffer.base.reference.count==1);
        buffer.base.target=PIPE_BUFFER; buffer.base.screen=&screen; fail_info=false;
        memcpy(output+16,dimensions,sizeof(dimensions));
    }
    memset(output+16,0,4);
    ps5_launch_grid(&context.base,&indirect);
    assert(!context.last_compute_status && submitted==4);
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,15,1,NULL,0);
    assert(destroyed==1);
    /* Image descriptor, atomic binding updates, ownership and all 39 resources. */
    destroyed=0;
    _Alignas(256) uint8_t pixels[768];
    struct ps5_resource image={.base={.screen=&screen,.target=PIPE_TEXTURE_2D,
        .format=PIPE_FORMAT_R32_UINT,.width0=17,.height0=3,.depth0=1,.array_size=1,
        .bind=PIPE_BIND_SHADER_IMAGE|PIPE_BIND_SAMPLER_VIEW},.data=pixels,.size=sizeof(pixels),.level_stride={256}};
    uint32_t descriptor[8];
    assert(!ps5_resource_storage_image_descriptor(&image.base,0,descriptor));
    assert(descriptor[0]==(uint32_t)((uintptr_t)pixels>>8));
    assert(descriptor[2]==(4u|(2u<<14)|0x80000000u));
    assert(descriptor[3]==0x90000204 && descriptor[4]==63 && descriptor[5]==0x400000);
    _Alignas(256) uint8_t mip_pixels[3840];
    struct ps5_resource mip=image;
    mip.data=mip_pixels; mip.size=sizeof(mip_pixels);
    mip.base.width0=16; mip.base.height0=8; mip.base.last_level=3;
    const size_t offsets[4]={1792,768,256,0};
    for(unsigned level=0;level<4;++level) {mip.level_offset[level]=offsets[level];mip.level_stride[level]=256;}
    assert(!ps5_resource_sampled_image_descriptor(&mip.base,1,2,descriptor));
    assert(descriptor[3]==0x90021204 && descriptor[4]==0 && descriptor[5]==0x400030);
    pipe_reference_init(&mip.base.reference,1);
    for(unsigned level=0;level<4;++level) {
        assert(!ps5_resource_storage_image_descriptor(&mip.base,level,descriptor));
        assert(descriptor[3]==(0x90000204u|(level<<12)|(level<<16)));
        assert(descriptor[5]==0x400030);
        struct pipe_image_view mv={.resource=&mip.base,.format=mip.base.format,.access=PIPE_IMAGE_ACCESS_WRITE};
        mv.u.tex.level=level;
        ps5_set_shader_images(&context.base,MESA_SHADER_COMPUTE,0,1,0,&mv);
        assert(!context.compute_images_invalid && context.compute_images[0].u.tex.level==level && mip.base.reference.count==2);
        ps5_set_shader_images(&context.base,MESA_SHADER_COMPUTE,0,0,1,NULL);
        assert(mip.base.reference.count==1);
    }
    assert(ps5_resource_storage_image_descriptor(&mip.base,4,descriptor)<0);
    struct ps5_resource layered=mip;
    _Alignas(256) uint8_t array_pixels[3*3840];
    layered.data=array_pixels;
    layered.base.target=PIPE_TEXTURE_2D_ARRAY; layered.base.array_size=3;
    layered.size=sizeof(array_pixels); layered.layer_stride=3840;
    assert(ps5_compute_image_array_resource(&layered.base));
    for(unsigned fault=0;fault<7;++fault) {
        struct pipe_resource bad=layered.base;
        if(fault==0) bad.target=PIPE_TEXTURE_3D;
        if(fault==1) bad.array_size=9;
        if(fault==2) bad.nr_samples=4;
        if(fault==3) bad.nr_storage_samples=4;
        if(fault==4) bad.bind|=PIPE_BIND_RENDER_TARGET;
        if(fault==5) bad.format=PIPE_FORMAT_R16_FLOAT;
        if(fault==6) bad.bind=PIPE_BIND_SHADER_IMAGE;
        assert(!ps5_compute_image_array_resource(&bad));
    }
    assert(!ps5_resource_sampled_image_descriptor(&layered.base,1,2,descriptor));
    assert(descriptor[3]==0xd0021204 && descriptor[4]==2);
    for(unsigned fault=0;fault<4;++fault) {
        struct ps5_resource bad=layered;
        if(fault==0) bad.size--;
        if(fault==1) bad.layer_stride++;
        if(fault==2) bad.base.array_size=9;
        if(fault==3) bad.base.array_size=0;
        assert(ps5_resource_storage_image_descriptor(&bad.base,1,descriptor)<0);
    }
    for(unsigned fault=0;fault<12;++fault) {
        struct ps5_resource bad=mip;
        if(fault<4) bad.level_offset[fault]+=256;
        if(fault>=4 && fault<8) bad.level_stride[fault-4]+=256;
        if(fault==8) bad.size--;
        if(fault==9) bad.base.last_level=PIPE_MAX_TEXTURE_LEVELS;
        assert(ps5_resource_sampled_image_descriptor(&bad.base,fault==10 ? 3 : 1,fault==11 ? 4 : 2,descriptor)<0);
    }
    const enum pipe_format scalar_formats[]={PIPE_FORMAT_R32_UINT,PIPE_FORMAT_R32_SINT,PIPE_FORMAT_R32_FLOAT};
    for(unsigned i=0;i<3;++i) {
        struct ps5_resource typed=image; typed.base.format=scalar_formats[i];
        assert(!ps5_resource_storage_image_descriptor(&typed.base,0,descriptor));
        assert((descriptor[1]&0x3ff00000u)==(0x1400000u+i*0x100000u));
        pipe_reference_init(&typed.base.reference,1);
        struct pipe_image_view v={.resource=&typed.base,.format=typed.base.format,.access=PIPE_IMAGE_ACCESS_READ_WRITE};
        ps5_set_shader_images(&context.base,MESA_SHADER_COMPUTE,7,1,0,&v);
        assert(!context.compute_images_invalid && typed.base.reference.count==2);
        ps5_set_shader_images(&context.base,MESA_SHADER_COMPUTE,7,0,1,NULL);
        assert(!context.compute_images_invalid && typed.base.reference.count==1);
    }
    for(unsigned fault=0;fault<15;++fault) {
        struct ps5_resource bad=image;
        if(fault==0) bad.base.target=PIPE_BUFFER;
        if(fault==1) bad.base.format=PIPE_FORMAT_R16_FLOAT;
        if(fault==2) bad.base.width0=0;
        if(fault==3) bad.base.height0=8193;
        if(fault==4) bad.base.depth0=2;
        if(fault==5) bad.base.array_size=2;
        if(fault==6) bad.base.last_level=1;
        if(fault==7) bad.base.nr_samples=4;
        if(fault==8) bad.base.bind=PIPE_BIND_SAMPLER_VIEW;
        if(fault==9) bad.base.bind|=PIPE_BIND_RENDER_TARGET;
        if(fault==10) bad.render_staging_size=1;
        if(fault==11) bad.depth_staging_size=1;
        if(fault==12) bad.data++;
        if(fault==13) bad.level_stride[0]=128;
        if(fault==14) bad.size=767;
        assert(ps5_resource_storage_image_descriptor(&bad.base,0,descriptor)<0);
    }
    pipe_reference_init(&image.base.reference,1);
    struct pipe_image_view view={.resource=&image.base,.format=PIPE_FORMAT_R32_UINT,.access=PIPE_IMAGE_ACCESS_READ_WRITE};
    ps5_set_shader_images(&context.base,MESA_SHADER_COMPUTE,7,1,0,&view);
    caller=&image.base; pipe_resource_reference(&caller,NULL);
    assert(image.base.reference.count==1 && !context.compute_images_invalid);
    for(unsigned fault=0;fault<7;++fault) {
        struct pipe_image_view bad=view;
        if(fault==0) bad.format=PIPE_FORMAT_R32_FLOAT;
        if(fault==1) bad.access=0;
        if(fault==2) bad.access|=PIPE_IMAGE_ACCESS_TEX2D_FROM_BUFFER;
        if(fault==3) bad.u.tex.level=1;
        if(fault==4) bad.u.tex.last_layer=1;
        if(fault==5) bad.u.tex.single_layer_view=true;
        if(fault==6) image.base.screen=&other_screen;
        struct pipe_image_view pair[2]={view,bad};
        ps5_set_shader_images(&context.base,MESA_SHADER_COMPUTE,6,2,0,pair);
        assert(context.compute_images_invalid && !context.compute_images[6].resource && image.base.reference.count==1);
        ps5_launch_grid(&context.base,&good);
        assert(context.last_compute_status<0 && submitted==4);
        image.base.screen=&screen;
        ps5_set_shader_images(&context.base,MESA_SHADER_COMPUTE,7,1,0,&view);
    }
    with_images=1;
    ps5_launch_grid(&context.base,&good);
    assert(!context.last_compute_status && submitted==5);
    struct pipe_image_view views[8]; for(unsigned i=0;i<8;++i) views[i]=view;
    ps5_set_shader_images(&context.base,MESA_SHADER_COMPUTE,0,8,0,views);
    assert(image.base.reference.count==8);
    pipe_reference_init(&buffer.base.reference,1);
    struct pipe_shader_buffer all[16]; for(unsigned i=0;i<16;++i) all[i]=binding;
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,0,16,all,0xffff);
    for(unsigned i=0;i<15;++i) {
        struct pipe_constant_buffer cb={.buffer=&buffer.base,.buffer_size=16};
        ps5_set_compute_constant_buffer(&context.base,i,&cb);
    }
    caller=&buffer.base; pipe_resource_reference(&caller,NULL);
    assert(buffer.base.reference.count==31);
    with_images=39;
    ps5_launch_grid(&context.base,&good);
    assert(!context.last_compute_status && submitted==6 && context.dispatches==6);
    ps5_set_shader_images(&context.base,MESA_SHADER_COMPUTE,0,0,8,NULL);
    assert(!context.compute_images_invalid && destroyed==1);
    ps5_set_shader_buffers(&context.base,MESA_SHADER_COMPUTE,0,16,NULL,0);
    for(unsigned i=0;i<15;++i) ps5_set_compute_constant_buffer(&context.base,i,NULL);
    assert(destroyed==2);
    /* Sampled views are retained, validated atomically, and cannot be missing. */
    struct pipe_sampler_view sampled={.texture=&image.base,.target=PIPE_TEXTURE_2D,
        .format=PIPE_FORMAT_R32_UINT,.swizzle_r=PIPE_SWIZZLE_X,.swizzle_g=PIPE_SWIZZLE_Y,
        .swizzle_b=PIPE_SWIZZLE_Z,.swizzle_a=PIPE_SWIZZLE_W};
    pipe_reference_init(&sampled.reference,1);
    pipe_reference_init(&image.base.reference,1);
    struct pipe_sampler_view *sampled_views[8];
    for(unsigned i=0;i<8;++i) sampled_views[i]=&sampled;
    ps5_set_compute_sampler_views(&context.base,0,8,0,sampled_views);
    assert(!context.compute_views_invalid && sampled.reference.count==9);
    cs.textures=255; with_sampled=true; with_images=0;
    unsigned before_sampled=submitted;
    ps5_launch_grid(&context.base,&good);
    assert(!context.last_compute_status && submitted==before_sampled+1);
    cs.array_textures=1;
    ps5_launch_grid(&context.base,&good);
    assert(context.last_compute_status<0 && submitted==before_sampled+1);
    cs.array_textures=0;
    for(unsigned fault=0;fault<7;++fault) {
        struct pipe_sampler_view bad=sampled;
        if(fault==0) bad.u.tex.first_level=1;
        if(fault==1) bad.u.tex.last_layer=1;
        if(fault==2) bad.swizzle_a=PIPE_SWIZZLE_1;
        if(fault==3) bad.format=PIPE_FORMAT_R32_FLOAT;
        if(fault==4) bad.target=PIPE_TEXTURE_2D_ARRAY;
        if(fault==5) image.base.screen=&other_screen;
        struct pipe_sampler_view *pair[2]={&sampled,&bad};
        ps5_set_compute_sampler_views(&context.base,fault==6 ? 16 : 0,2,0,pair);
        assert(context.compute_views_invalid && sampled.reference.count==9);
        ps5_launch_grid(&context.base,&good);
        assert(context.last_compute_status<0 && submitted==before_sampled+1);
        image.base.screen=&screen;
        ps5_set_compute_sampler_views(&context.base,0,8,0,sampled_views);
    }
    ps5_set_compute_sampler_views(&context.base,7,0,1,NULL);
    assert(sampled.reference.count==8);
    ps5_launch_grid(&context.base,&good);
    assert(context.last_compute_status<0 && submitted==before_sampled+1);
    ps5_set_compute_sampler_views(&context.base,0,0,8,NULL);
    assert(sampled.reference.count==1 && !context.compute_views_invalid);
    ps5_set_compute_sampler_views(&context.base,0,8,0,sampled_views);
    cs.filtered_textures=with_filtered=255;
    unsigned before_filter=submitted;
    ps5_launch_grid(&context.base,&good); /* Missing sampler must not submit. */
    assert(context.last_compute_status<0 && submitted==before_filter);
    image.base.format=sampled.format=PIPE_FORMAT_R32_FLOAT;
    struct ps5_sampler_state sampler={.base={.wrap_s=PIPE_TEX_WRAP_CLAMP_TO_EDGE,
        .wrap_t=PIPE_TEX_WRAP_CLAMP_TO_EDGE,.wrap_r=PIPE_TEX_WRAP_CLAMP_TO_EDGE,
        .min_mip_filter=PIPE_TEX_MIPFILTER_NONE}};
    void *states[8]; for(unsigned i=0;i<8;++i) states[i]=&sampler;
    const unsigned wraps[3]={PIPE_TEX_WRAP_REPEAT,PIPE_TEX_WRAP_MIRROR_REPEAT,PIPE_TEX_WRAP_CLAMP_TO_EDGE};
    for(unsigned wrap=0;wrap<3;++wrap) for(unsigned filter=0;filter<4;++filter) {
        sampler.base.wrap_s=sampler.base.wrap_t=sampler.base.wrap_r=wraps[wrap];
        expected_sampler[0]=wrap*0x49;
        sampler.base.min_img_filter=filter&1 ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
        sampler.base.mag_img_filter=filter&2 ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
        ps5_set_compute_sampler_states(&context.base,0,8,states);
        expected_sampler[2]=((filter&1)!=0)<<22 | ((filter&2)!=0)<<20;
        assert(!context.compute_samplers_invalid && context.compute_sampler_mask==255);
        ps5_launch_grid(&context.base,&good);
        assert(!context.last_compute_status && submitted==++before_filter);
    }
    struct ps5_sampler_state valid=sampler;
    sampler.base.min_mip_filter=PIPE_TEX_MIPFILTER_LINEAR;
    sampler.base.max_lod=3;
    ps5_set_compute_sampler_states(&context.base,0,8,states);
    assert(!context.compute_samplers_invalid && context.compute_samplers[7][1]==0x300000 &&
        context.compute_samplers[7][2]==0x08500000);
    cs.texture_lod[7]=1;
    ps5_launch_grid(&context.base,&good); /* Even valid sampler state cannot exceed the view. */
    assert(context.last_compute_status<0 && submitted==before_filter);
    cs.texture_lod[7]=0;
    for(unsigned fault=0;fault<12;++fault) {
        sampler=valid;
        if(fault==0) sampler.base.wrap_s=PIPE_TEX_WRAP_REPEAT;
        if(fault==1) sampler.base.wrap_t=PIPE_TEX_WRAP_REPEAT;
        if(fault==2) sampler.base.wrap_r=PIPE_TEX_WRAP_REPEAT;
        if(fault==3) sampler.base.compare_mode=1;
        if(fault==4) sampler.base.unnormalized_coords=1;
        if(fault==5) sampler.base.max_anisotropy=2;
        if(fault==6) sampler.base.min_mip_filter=3;
        if(fault==7) sampler.base.min_lod=1;
        if(fault==8) sampler.base.max_lod=16;
        if(fault==9) sampler.base.lod_bias=1;
        ps5_set_compute_sampler_states(&context.base,fault==10 ? 16 : 0,8,fault==11 ? NULL : states);
        assert(context.compute_samplers_invalid && context.compute_sampler_mask==255);
        ps5_launch_grid(&context.base,&good);
        assert(context.last_compute_status<0 && submitted==before_filter);
    }
    sampler=valid;
    ps5_set_compute_sampler_states(&context.base,0,8,states);
    image.base.format=sampled.format=PIPE_FORMAT_R32_UINT;
    ps5_launch_grid(&context.base,&good);
    assert(context.last_compute_status<0 && submitted==before_filter);
    for(unsigned i=0;i<8;++i) states[i]=NULL;
    ps5_set_compute_sampler_states(&context.base,0,8,states);
    assert(!context.compute_sampler_mask && !context.compute_samplers_invalid);
    ps5_set_compute_sampler_views(&context.base,0,0,8,NULL);
    cs.filtered_textures=with_filtered=0;
    cs.textures=0; with_sampled=false;
    /* All sixteen bindings are retained and copied, including the upper half. */
    struct pipe_sampler_view *all_views[16]; void *all_states[16];
    image.base.format=sampled.format=PIPE_FORMAT_R32_FLOAT;
    for(unsigned i=0;i<16;++i) { all_views[i]=&sampled; all_states[i]=&sampler; }
    ps5_set_compute_sampler_views(&context.base,0,16,0,all_views);
    ps5_set_compute_sampler_states(&context.base,0,16,all_states);
    assert(!context.compute_views_invalid && !context.compute_samplers_invalid && sampled.reference.count==17);
    cs.textures=cs.filtered_textures=with_filtered=65535;
    with_sampled=true; sampled_count=16;
    ps5_launch_grid(&context.base,&good);
    assert(!context.last_compute_status && context.compute_sampler_mask==65535);
    ps5_set_compute_sampler_views(&context.base,0,0,16,NULL);
    for(unsigned i=0;i<16;++i) all_states[i]=NULL;
    ps5_set_compute_sampler_states(&context.base,0,16,all_states);
    assert(sampled.reference.count==1 && !context.compute_sampler_mask);
}
'''
with tempfile.TemporaryDirectory() as directory:
    formats = Path(directory) / "util/format"
    formats.mkdir(parents=True)
    with (formats / "u_format_gen.h").open("w") as generated:
        subprocess.run([sys.executable, str(MESA / "src/util/format/u_format_table.py"),
            str(MESA / "src/util/format/u_format.yaml"), "--enums"], stdout=generated, check=True)
    executable = str(Path(directory) / "compute-bindings")
    subprocess.run(["clang-18", "-std=gnu11", "-O2", "-Wall", "-Werror",
        "-DHAVE_ENDIAN_H=1", "-DHAVE_FUNC_ATTRIBUTE_PACKED=1", "-D_GNU_SOURCE",
        "-I", str(MESA / "include"), "-I", str(MESA / "src"),
        "-I", str(MESA / "src/gallium/include"), "-I", str(MESA / "src/gallium/auxiliary"),
        "-I", directory,
        "-I", str(ROOT / "third_party/opengnm-psbc/libpsbc"),
        "-I", str(ROOT / "third_party/opengnm-psbc/src"),
        "-I", str(ROOT / "src/platform"),
        "-x", "c", "-o", executable, "-"], input=code, text=True, check=True)
    subprocess.run([executable], check=True, timeout=10)
print("PASS: Gallium 39-resource bindings, image descriptors/lifetime, upload failure, direct/indirect guards and unbind")
