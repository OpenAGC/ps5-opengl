// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

/* Private compute-API candidate only, not release or conformance coverage.
 * All shader execution and resource binding goes through normal GL entrypoints.
 * The internal context is used only to observe native submission results. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include "main/context.h"
#include "ps5_screen.h"

#define TAG "[ps5-egl-compute-api] "
#define GUARD UINT32_C(0xcdcdcdcd)

static int
check_gl(const char *step)
{
   GLenum error;
   int ok = 1;
   while ((error = glGetError()) != GL_NO_ERROR) {
      printf(TAG "%s GL error=%x\n", step, error);
      ok = 0;
   }
   printf(TAG "%s %s\n", step, ok ? "ok" : "FAILED");
   return ok;
}

static int
has_extension(const char *name)
{
   GLint count = 0;
   glGetIntegerv(GL_NUM_EXTENSIONS, &count);
   for (GLint i = 0; i < count; ++i) {
      const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, i);
      if (extension && !strcmp(extension, name))
         return 1;
   }
   printf(TAG "missing extension %s\n", name);
   return 0;
}

static int
check_egl(const char *step, EGLBoolean result)
{
   EGLint error = eglGetError();
   printf(TAG "%s result=%u EGL error=%x\n", step, result, error);
   return result == EGL_TRUE && error == EGL_SUCCESS;
}

static int
compile_program(const char *name, const char *source, GLuint *shader, GLuint *program)
{
   GLint compiled = 0, linked = 0;
   char log[4096] = {0};
   *shader = glCreateShader(GL_COMPUTE_SHADER);
   if (!check_gl("create shader") || !*shader)
      return 0;
   glShaderSource(*shader, 1, &source, NULL);
   printf(TAG "%s compile begin\n", name);
   glCompileShader(*shader);
   glGetShaderiv(*shader, GL_COMPILE_STATUS, &compiled);
   glGetShaderInfoLog(*shader, sizeof(log), NULL, log);
   printf(TAG "%s compile=%d log=%s\n", name, compiled, log);
   if (!check_gl("compile") || !compiled)
      return 0;
   *program = glCreateProgram();
   if (!check_gl("create program") || !*program)
      return 0;
   glAttachShader(*program, *shader);
   printf(TAG "%s link begin\n", name);
   glLinkProgram(*program);
   glGetProgramiv(*program, GL_LINK_STATUS, &linked);
   glGetProgramInfoLog(*program, sizeof(log), NULL, log);
   printf(TAG "%s link=%d log=%s\n", name, linked, log);
   return check_gl("link") && linked;
}

static int
reset_output(GLuint buffer)
{
   uint32_t words[80];
   for (unsigned i = 0; i < 80; ++i)
      words[i] = GUARD;
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
   glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(words), words, GL_DYNAMIC_DRAW);
   glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, buffer, 32, 16);
   return check_gl("reset guarded output");
}

static int
check_buffer(const char *name, GLenum target, GLuint buffer, uint32_t expected)
{
   uint32_t words[80] = {0};
   printf(TAG "%s readback begin\n", name);
   glBindBuffer(target, buffer);
   glGetBufferSubData(target, 0, sizeof(words), words);
   if (!check_gl("guarded readback"))
      return 0;
   for (unsigned i = 0; i < 80; ++i) {
      uint32_t wanted = i == 8 ? expected : GUARD;
      if (words[i] != wanted) {
         printf(TAG "%s word=%u actual=%08x expected=%08x\n",
                name, i, words[i], wanted);
         return 0;
      }
   }
   printf(TAG "%s value=%08x guards=79/79 PASS\n", name, expected);
   return 1;
}

static int
dispatch_checked(const char *name, struct pipe_context *pipe,
                 unsigned expected_dispatches, GLbitfield barriers)
{
   unsigned dispatches = 0;
   printf(TAG "%s dispatch begin\n", name);
   glDispatchCompute(1, 1, 1);
   if (!check_gl("dispatch"))
      return 0;
   glMemoryBarrier(barriers);
   if (!check_gl("barrier"))
      return 0;
   glFinish();
   int status = ps5_context_last_compute_status(pipe, &dispatches);
   printf(TAG "%s native status=%d dispatches=%u expected=%u\n",
          name, status, dispatches, expected_dispatches);
   return check_gl("dispatch finish") && !status && dispatches == expected_dispatches;
}

int
main(void)
{
   static const char *source =
      "#version 330\n"
      "#extension GL_ARB_compute_shader : require\n"
      "#extension GL_ARB_shader_storage_buffer_object : require\n"
      "layout(local_size_x=1, local_size_y=1, local_size_z=1) in;\n"
      "uniform uint addend;\n"
      "layout(std140) uniform Input { uint input_value; };\n"
      "layout(std430) buffer Output { uint result; };\n"
      "void main() { result = input_value + addend; }\n";
   static const char *image_source =
      "#version 330\n"
      "#extension GL_ARB_compute_shader : require\n"
      "#extension GL_ARB_shader_image_load_store : require\n"
      "layout(local_size_x=1) in;\n"
      "layout(r32f) uniform writeonly image2D destination;\n"
      "uniform float value;\n"
      "void main() { imageStore(destination, ivec2(0), vec4(value, 0, 0, 1)); }\n";
   static const char *sample_source =
      "#version 330\n"
      "#extension GL_ARB_compute_shader : require\n"
      "#extension GL_ARB_shader_storage_buffer_object : require\n"
      "layout(local_size_x=1) in;\n"
      "uniform sampler2D source_texture;\n"
      "layout(std430) buffer Output { uint result; };\n"
      "void main() { result = floatBitsToUint(texture(source_texture, vec2(0.5)).x); }\n";
   static const char *atomic_source =
      "#version 330\n"
      "#extension GL_ARB_compute_shader : require\n"
      "#extension GL_ARB_shader_storage_buffer_object : require\n"
      "#extension GL_ARB_shader_atomic_counters : require\n"
      "layout(local_size_x=1) in;\n"
      "layout(binding=7, offset=12) uniform atomic_uint counter;\n"
      "layout(std430) buffer Output { uint result; };\n"
      "void main() { result = atomicCounterIncrement(counter); }\n";
   const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   const EGLint surface_attributes[] = {
      EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE,
   };
   const EGLint context_attributes[] = {
      EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
      EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
      EGL_NONE,
   };
   EGLDisplay display = EGL_NO_DISPLAY;
   EGLSurface surface = EGL_NO_SURFACE;
   EGLContext context = EGL_NO_CONTEXT;
   EGLConfig config = NULL;
   EGLint count = 0;
   GLuint shader = 0, program = 0, buffers[2] = {0, 0};
   GLuint extra_shaders[3] = {0}, extra_programs[3] = {0};
   GLuint texture = 0, atomic_buffer = 0;
   GLint major = 0, minor = 0, profile = 0;
   GLint addend = -1, block_size = 0, ubo_alignment = 0, ssbo_alignment = 0;
   unsigned baseline = 0;
   int initialized = 0, current = 0, passed = 0;

   setvbuf(stdout, NULL, _IONBF, 0);
   puts(TAG "TEST-ONLY candidate; not release/conformance; no swaps");
   display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (display == EGL_NO_DISPLAY ||
       !check_egl("initialize", eglInitialize(display, NULL, NULL)))
      goto cleanup;
   initialized = 1;
   if (!check_egl("bind API", eglBindAPI(EGL_OPENGL_API)) ||
       !check_egl("choose config", eglChooseConfig(display, config_attributes,
                                                  &config, 1, &count)) || count != 1)
      goto cleanup;
   surface = eglCreatePbufferSurface(display, config, surface_attributes);
   if (!check_egl("create pbuffer", surface != EGL_NO_SURFACE))
      goto cleanup;
   context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
   if (!check_egl("create context", context != EGL_NO_CONTEXT) ||
       !check_egl("make current", eglMakeCurrent(display, surface, surface, context)))
      goto cleanup;
   current = 1;
   glGetIntegerv(GL_MAJOR_VERSION, &major);
   glGetIntegerv(GL_MINOR_VERSION, &minor);
   glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
   printf(TAG "GL version=%d.%d profile=%x\n", major, minor, profile);
   if (major != 3 || minor != 3 || !(profile & GL_CONTEXT_CORE_PROFILE_BIT) ||
       !has_extension("GL_ARB_compute_shader") ||
       !has_extension("GL_ARB_shader_storage_buffer_object") ||
       !check_gl("context contract"))
      goto cleanup;

   if (!compile_program("defaults+UBO", source, &shader, &program))
      goto cleanup;
   addend = glGetUniformLocation(program, "addend");
   GLuint block = glGetUniformBlockIndex(program, "Input");
   if (!check_gl("uniform lookup") || addend < 0 || block == GL_INVALID_INDEX)
      goto cleanup;
   glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &block_size);
   glUniformBlockBinding(program, block, 0);
   /* Output uses the normal initial SSBO binding zero; no binding layout. */
   glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ubo_alignment);
   glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &ssbo_alignment);
   printf(TAG "UBO size=%d alignment=%d SSBO alignment=%d\n",
          block_size, ubo_alignment, ssbo_alignment);
   if (!check_gl("resource contract") || block_size != 16 ||
       ubo_alignment <= 0 || 16 % ubo_alignment ||
       ssbo_alignment <= 0 || 32 % ssbo_alignment)
      goto cleanup;
   glUseProgram(program);
   glGenBuffers(2, buffers);
   glFinish();
   if (!check_gl("setup") || !buffers[0] || !buffers[1])
      goto cleanup;
   struct gl_context *mesa = _mesa_get_current_context();
   if (!mesa || !mesa->pipe)
      goto cleanup;
   (void)ps5_context_last_compute_status(mesa->pipe, &baseline);

   for (unsigned pass = 0; pass < 2; ++pass) {
      uint32_t words[80], readback[80], input[8];
      unsigned dispatches = 0;
      const uint32_t expected = pass ? 40 : 20;
      for (unsigned i = 0; i < 80; ++i)
         words[i] = GUARD;
      for (unsigned i = 0; i < 8; ++i)
         input[i] = GUARD;
      input[4] = pass ? 31 : 13;
      printf(TAG "pass=%u upload begin\n", pass);
      glUniform1ui(addend, pass ? 9 : 7);
      glBindBuffer(GL_UNIFORM_BUFFER, buffers[0]);
      glBufferData(GL_UNIFORM_BUFFER, sizeof(input), input, GL_DYNAMIC_DRAW);
      glBindBufferRange(GL_UNIFORM_BUFFER, 0, buffers[0], 16, 16);
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[1]);
      glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(words), words, GL_DYNAMIC_DRAW);
      glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, buffers[1], 32, 16);
      if (!check_gl("upload/bind"))
         goto cleanup;
      printf(TAG "pass=%u dispatch begin\n", pass);
      glDispatchCompute(1, 1, 1);
      if (!check_gl("dispatch"))
         goto cleanup;
      glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
      if (!check_gl("barrier"))
         goto cleanup;
      puts(TAG "readback begin");
      glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(readback), readback);
      glFinish();
      int status = ps5_context_last_compute_status(mesa->pipe, &dispatches);
      printf(TAG "pass=%u native status=%d dispatches=%u baseline=%u\n",
             pass, status, dispatches, baseline);
      if (!check_gl("readback/finish") || status || dispatches != baseline + pass + 1)
         goto cleanup;
      for (unsigned i = 0; i < 80; ++i) {
         uint32_t wanted = i == 8 ? expected : GUARD;
         if (readback[i] != wanted) {
            printf(TAG "pass=%u word=%u actual=%08x expected=%08x\n",
                   pass, i, readback[i], wanted);
            goto cleanup;
         }
      }
      printf(TAG "pass=%u value=%u guards=79/79 PASS\n", pass, expected);
   }

   if (!has_extension("GL_ARB_shader_image_load_store") ||
       !has_extension("GL_ARB_shader_atomic_counters") ||
       !check_gl("image/atomic extensions"))
      goto cleanup;
   if (!compile_program("image writer", image_source, &extra_shaders[0], &extra_programs[0]) ||
       !compile_program("texture sampler", sample_source, &extra_shaders[1], &extra_programs[1]) ||
       !compile_program("atomic counter", atomic_source, &extra_shaders[2], &extra_programs[2]))
      goto cleanup;
   GLint destination = glGetUniformLocation(extra_programs[0], "destination");
   GLint value = glGetUniformLocation(extra_programs[0], "value");
   GLint sampler = glGetUniformLocation(extra_programs[1], "source_texture");
   if (!check_gl("image/sampler uniforms") || destination < 0 || value < 0 || sampler < 0)
      goto cleanup;
   glGenTextures(1, &texture);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
   const float initial_pixel = 0.0f;
   glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 1, 1, 0, GL_RED, GL_FLOAT, &initial_pixel);
   glBindImageTexture(0, texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
   glGenBuffers(1, &atomic_buffer);
   if (!check_gl("image/atomic resources") || !texture || !atomic_buffer)
      goto cleanup;

   for (unsigned pass = 0; pass < 2; ++pass) {
      printf(TAG "image/atomic pass=%u begin\n", pass);
      /* Order the next image write after earlier shader accesses, including
       * sampling in the preceding pass. No texture readback/reset is used. */
      glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
      glUseProgram(extra_programs[0]);
      glUniform1i(destination, 0);
      glUniform1f(value, pass ? 0.75f : 0.25f);
      if (!check_gl("image uniforms") ||
          !dispatch_checked("image writer", mesa->pipe, baseline + 3 + pass * 3,
                            GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT))
         goto cleanup;
      glUseProgram(extra_programs[1]);
      glUniform1i(sampler, 0);
      if (!reset_output(buffers[1]) ||
          !dispatch_checked("texture sampler", mesa->pipe, baseline + 4 + pass * 3,
                            GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT) ||
          !check_buffer("sampled image", GL_SHADER_STORAGE_BUFFER, buffers[1],
                        pass ? UINT32_C(0x3f400000) : UINT32_C(0x3e800000)))
         goto cleanup;

      uint32_t counters[80];
      const uint32_t initial_counter = pass ? 17 : 5;
      for (unsigned i = 0; i < 80; ++i)
         counters[i] = GUARD;
      counters[8] = initial_counter;
      glUseProgram(extra_programs[2]);
      glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, atomic_buffer);
      glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(counters), counters, GL_DYNAMIC_DRAW);
      /* GL byte 20 + declaration byte 12 = byte 32. Mesa must supply the
       * alignment correction when lowering binding 7 to a shader buffer. */
      glBindBufferRange(GL_ATOMIC_COUNTER_BUFFER, 7, atomic_buffer, 20, 16);
      if (!reset_output(buffers[1]) ||
          !dispatch_checked("atomic counter", mesa->pipe, baseline + 5 + pass * 3,
                            GL_ATOMIC_COUNTER_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT |
                            GL_BUFFER_UPDATE_BARRIER_BIT) ||
          !check_buffer("atomic returned", GL_SHADER_STORAGE_BUFFER, buffers[1], initial_counter) ||
          !check_buffer("atomic incremented", GL_ATOMIC_COUNTER_BUFFER, atomic_buffer,
                        initial_counter + 1))
         goto cleanup;
   }

   /* Keep atomic binding 7 attached: changing programs must clear stale
    * lowered slots through the normal Mesa state atoms, not a manual unbind. */
   puts(TAG "return to defaults+UBO program begin");
   glUseProgram(program);
   glUniform1ui(addend, 7);
   const uint32_t restored_input[4] = {13, GUARD, GUARD, GUARD};
   glBindBuffer(GL_UNIFORM_BUFFER, buffers[0]);
   glBufferSubData(GL_UNIFORM_BUFFER, 16, sizeof(restored_input), restored_input);
   glBindBufferRange(GL_UNIFORM_BUFFER, 0, buffers[0], 16, 16);
   if (!reset_output(buffers[1]) ||
       !dispatch_checked("restored defaults+UBO", mesa->pipe, baseline + 9,
                         GL_SHADER_STORAGE_BARRIER_BIT | GL_ATOMIC_COUNTER_BARRIER_BIT |
                         GL_BUFFER_UPDATE_BARRIER_BIT) ||
       !check_buffer("restored output", GL_SHADER_STORAGE_BUFFER, buffers[1], 20) ||
       !check_buffer("retired atomic unchanged", GL_ATOMIC_COUNTER_BUFFER, atomic_buffer, 18))
      goto cleanup;
   puts(TAG "dispatch delta=9 (original 2 + image/sampler/atomic 6 + restored 1)");
   passed = 1;

cleanup:
   puts(TAG "cleanup begin");
   if (current) {
      glFinish();
      passed &= check_gl("cleanup finish");
      glUseProgram(0);
      glBindBufferBase(GL_UNIFORM_BUFFER, 0, 0);
      /* Do not issue unsupported SSBO calls if context validation failed. */
      if (buffers[1])
         glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
      if (atomic_buffer) {
         glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 7, 0);
         glDeleteBuffers(1, &atomic_buffer);
      }
      if (texture) {
         glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
         glBindTexture(GL_TEXTURE_2D, 0);
         glDeleteTextures(1, &texture);
      }
      glDeleteBuffers(2, buffers);
      for (unsigned i = 0; i < 3; ++i) {
         if (extra_programs[i])
            glDeleteProgram(extra_programs[i]);
         if (extra_shaders[i])
            glDeleteShader(extra_shaders[i]);
      }
      if (program)
         glDeleteProgram(program);
      if (shader)
         glDeleteShader(shader);
      passed &= check_gl("delete GL objects");
      passed &= check_egl("unbind", eglMakeCurrent(display, EGL_NO_SURFACE,
                                                   EGL_NO_SURFACE, EGL_NO_CONTEXT));
   }
   if (context != EGL_NO_CONTEXT)
      passed &= check_egl("destroy context", eglDestroyContext(display, context));
   if (surface != EGL_NO_SURFACE)
      passed &= check_egl("destroy pbuffer", eglDestroySurface(display, surface));
   if (initialized)
      passed &= check_egl("terminate", eglTerminate(display));
   printf(TAG "result=%d %s\n", passed ? 0 : 1, passed ? "PASS" : "FAIL");
   return passed ? 0 : 1;
}
