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
image_at = source.index("int\nps5_resource_storage_image_descriptor(")
image_descriptor = source[image_at:source.index("\nstruct pipe_resource *", image_at)]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_inlines.h"
#include "psbc_compile.h"
#define PS5_COMPUTE_STORAGE_SLOTS 16
#define PS5_COMPUTE_CONSTANT_SLOTS 15
#define PS5_COMPUTE_BUFFER_SLOTS 31
#define PS5_COMPUTE_IMAGE_SLOTS 8
#define PS5_COMPUTE_DESCRIPTOR_BYTES (31*16+8*32)
#define PS5_MAX_TEXTURE_2D_SIZE 8192
#define PS5_MAX_CONSTANT_BUFFER_SIZE 0x4000u
struct ps5_resource {
    struct pipe_resource base; uint8_t *data;
    size_t size, render_staging_size, depth_staging_size;
    unsigned level_stride[1];
};
struct ps5_compute_shader { PsbcShaderOutput output; };
struct ps5_context {
    struct pipe_context base;
    struct ps5_compute_shader *cs;
    struct pipe_resource *compute_descriptors;
    struct pipe_shader_buffer compute_buffers[31];
    struct pipe_image_view compute_images[8];
    bool compute_images_invalid;
    bool compute_bindings_invalid;
    uint32_t compute_constants_invalid;
    int last_compute_status;
    unsigned dispatches;
};
static unsigned submitted, destroyed;
static unsigned with_images;
static bool multi, with_constants, fail_upload;
static bool fail_info;
static struct ps5_resource *upload_resource;
static bool ps5_texture_descriptor_format(enum pipe_format f,uint32_t *word) {
    assert(f==PIPE_FORMAT_R32_UINT); *word=0x1400000; return true;
}
''' + image_descriptor + r'''
static int ps5_resource_info(struct pipe_resource *base, void **address, size_t *size, size_t *allocation) {
    (void)allocation;
    if (fail_info) return -1;
    *address=((struct ps5_resource *)base)->data;
    *size=base->width0;
    return 0;
}
static void ps5_flush_gpu_data(const void *address, size_t size) { assert(address && size==12); }
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
static int ps5_agc_compute_execute(struct pipe_screen *s, const PsbcShaderOutput *shader,
    struct pipe_resource *table, struct pipe_resource *const *buffers, unsigned count,
    const uint32_t groups[3]) {
    assert(s && shader && groups[0]==2 && groups[1]==1 && groups[2]==1);
    if(with_images) {
        assert(count==with_images);
        struct ps5_resource *t=(struct ps5_resource *)table;
        for(unsigned i=0;i<8;++i) {
            uint32_t expected[8]={0};
            if(with_images==39 || i==7)
                assert(!ps5_resource_storage_image_descriptor(buffers[count-1],expected));
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
''' + functions + r'''
int main(void) {
    struct pipe_screen screen={.resource_destroy=destroy}, other_screen={0};
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
    assert(!ps5_resource_storage_image_descriptor(&image.base,descriptor));
    assert(descriptor[0]==(uint32_t)((uintptr_t)pixels>>8));
    assert(descriptor[2]==(4u|(2u<<14)|0x80000000u));
    assert(descriptor[3]==0x90000fac && descriptor[4]==63 && descriptor[5]==0x400000);
    for(unsigned fault=0;fault<15;++fault) {
        struct ps5_resource bad=image;
        if(fault==0) bad.base.target=PIPE_BUFFER;
        if(fault==1) bad.base.format=PIPE_FORMAT_R32_FLOAT;
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
        assert(ps5_resource_storage_image_descriptor(&bad.base,descriptor)<0);
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
        "-x", "c", "-o", executable, "-"], input=code, text=True, check=True)
    subprocess.run([executable], check=True, timeout=10)
print("PASS: Gallium 39-resource bindings, image descriptors/lifetime, upload failure, direct/indirect guards and unbind")
