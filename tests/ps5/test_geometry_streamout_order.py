#!/usr/bin/env python3
# PS5 OpenGL - OpenGL implementation for PlayStation 5.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the actual ordered collector with shuffled GPU ranges and overflow."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/gallium/ps5/ps5_screen.c').read_text()

def function(name):
    start = source.rfind('static ', 0, source.index(name + '('))
    return source[start:source.index('\n}\n', start) + 3]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define PIPE_MAX_VERTEX_STREAMS 4
#define PS5_STREAMOUT_CONTROL_OFFSET 0
#define PS5_STREAMOUT_CONTROL_BYTES 64
#define BITFIELD_BIT(i) (1u<<(i))
#define MIN2(a,b) ((a)<(b)?(a):(b))
struct pipe_resource { unsigned unused; };
struct ps5_resource { struct pipe_resource base; unsigned char *data; size_t size; };
struct pipe_stream_output_target { struct pipe_resource *buffer; unsigned buffer_offset,buffer_size; };
struct ps5_stream_output_target { struct pipe_stream_output_target base; unsigned offset; };
struct pipe_stream_output { unsigned output_buffer,dst_offset,num_components; };
struct pipe_stream_output_info { unsigned num_outputs; struct pipe_stream_output output[4]; };
struct ps5_shader { struct pipe_stream_output_info stream_output; };
struct ps5_context { struct ps5_shader *gs; void *tes; struct pipe_resource *streamout_records,*streamout_staging[4];
    struct pipe_stream_output_target *stream_output_targets[4]; unsigned stream_output_primitive; };
typedef struct { unsigned streamout_enabled_stream_buffers_mask,streamout_strides_dwords[4]; } PsbcShaderMetadata;
struct ps5_streamout_control { uint32_t buffer_offsets[4],generated_primitives[4],emitted_primitives[4],reserved[4]; };
struct ps5_streamout_record { uint32_t primitive,invocation,reserved[2],offsets[4],generated[4],emitted[4]; };
static void ps5_flush_gpu_data(const void *p,size_t n) { assert(p && n); }
static unsigned ps5_streamout_vertices_per_primitive(unsigned p) { return p; }
static unsigned ps5_streamout_buffer_mask(unsigned m) { return (m | (m>>4) | (m>>8) | (m>>12))&15; }
static unsigned ps5_streamout_buffer_stream(unsigned m,unsigned b) {
    for(unsigned s=0;s<4;++s) if(m & (1u<<(4*s+b))) return s;
    return 4;
}
'''
code += function('ps5_streamout_record_compare')
code += function('ps5_collect_geometry_streamout')
code += r'''
int main(void) {
  for(unsigned cap=2;cap<=6;cap+=4) {
    struct { struct ps5_streamout_control control; struct ps5_streamout_record record[3]; } table={0};
    uint32_t input[3][32]={{0}},output[3][32];
    for(unsigned b=0;b<3;++b) for(unsigned i=0;i<32;++i) output[b][i]=0xdeadbeef;
    const unsigned keys[3]={9000,0,5000},counts[3]={2,1,3},stream1[3]={1,0,2};
    unsigned offsets[3]={0};
    for(unsigned i=0;i<3;++i) {
      struct ps5_streamout_record *r=&table.record[i]; r->primitive=keys[i];
      r->generated[0]=r->emitted[0]=counts[i]; r->generated[1]=r->emitted[1]=stream1[i];
      for(unsigned b=0;b<3;++b) {
        unsigned count=b==2?stream1[i]:counts[i]; r->offsets[b]=offsets[b]*4;
        for(unsigned j=0;j<count;++j) for(unsigned v=0;v<2;++v)
          input[b][offsets[b]++]=keys[i]+j;
      }
    }
    table.control.reserved[0]=table.control.reserved[1]=3;
    table.control.generated_primitives[0]=6; table.control.generated_primitives[1]=3;
    struct ps5_resource records={.data=(void *)&table,.size=sizeof(table)},src[3],dst[3];
    struct ps5_stream_output_target targets[3];
    struct ps5_shader shader={.stream_output={.num_outputs=3,.output={{0,0,1},{1,0,1},{2,0,1}}}};
    struct ps5_context c={.gs=&shader,.streamout_records=&records.base,.stream_output_primitive=2};
    PsbcShaderMetadata m={.streamout_enabled_stream_buffers_mask=3|(4<<4),.streamout_strides_dwords={1,1,1,0}};
    for(unsigned b=0;b<3;++b) {
      src[b]=(struct ps5_resource){.data=(void *)input[b],.size=sizeof(input[b])};
      dst[b]=(struct ps5_resource){.data=(void *)output[b],.size=sizeof(output[b])};
      targets[b]=(struct ps5_stream_output_target){.base={&dst[b].base,4,(b==0?cap:b==1?6:1)*8+4},.offset=4};
      c.streamout_staging[b]=&src[b].base; c.stream_output_targets[b]=&targets[b].base;
    }
    uint64_t written[4]={0},generated[4]={0};
    assert(ps5_collect_geometry_streamout(&c,&records,&m,written,generated));
    assert(written[0]==2*cap && written[1]==2 && generated[0]==6 && generated[1]==3);
    const uint32_t expected[12]={0,0,5000,5000,5001,5001,5002,5002,9000,9000,9001,9001};
    for(unsigned b=0;b<2;++b) {
      assert(output[b][0]==0xdeadbeef && output[b][1]==0xdeadbeef);
      assert(memcmp(output[b]+2,expected,cap*8)==0);
      assert(output[b][2+cap*2]==0xdeadbeef);
    }
    assert(output[2][2]==5000 && output[2][3]==5000 && output[2][4]==0xdeadbeef);
    shader.stream_output.output[1].num_components=0;
    for(unsigned i=0;i<32;++i) output[1][i]=0xdeadbeef;
    assert(ps5_collect_geometry_streamout(&c,&records,&m,written,generated));
    for(unsigned i=0;i<32;++i) assert(output[1][i]==0xdeadbeef);
    uint32_t saved[3][32]; memcpy(saved,output,sizeof(saved));
    unsigned size=targets[0].base.buffer_size;
    targets[0].base.buffer_size=UINT32_MAX;
    assert(!ps5_collect_geometry_streamout(&c,&records,&m,written,generated));
    assert(memcmp(saved,output,sizeof(saved))==0);
    targets[0].base.buffer_size=size;
    struct ps5_streamout_record original=table.record[1];
    table.record[1].primitive=table.record[0].primitive;
    assert(!ps5_collect_geometry_streamout(&c,&records,&m,written,generated));
    assert(memcmp(saved,output,sizeof(saved))==0);
    table.record[1]=original;
    table.record[0].offsets[0]=UINT32_MAX;
    assert(!ps5_collect_geometry_streamout(&c,&records,&m,written,generated));
    assert(memcmp(saved,output,sizeof(saved))==0);
    table.control.reserved[2]=1;
    assert(!ps5_collect_geometry_streamout(&c,&records,&m,written,generated));
  }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = str(Path(tmp) / 'ordered-streamout')
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all','-no-pie',
                    '-x','c','-','-o',exe],input=code,text=True,check=True)
    subprocess.run([exe],check=True)
print('PASS: actual ordered collector, keys beyond4096, variable counts, multiple buffers/streams, overflow, guards')
