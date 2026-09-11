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
   GLint major = 0, minor = 0, profile = 0, compiled = 0, linked = 0;
   GLint addend = -1, block_size = 0, ubo_alignment = 0, ssbo_alignment = 0;
   unsigned baseline = 0;
   int initialized = 0, current = 0, passed = 0;
   char log[4096] = {0};

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

   shader = glCreateShader(GL_COMPUTE_SHADER);
   if (!check_gl("create shader") || !shader)
      goto cleanup;
   glShaderSource(shader, 1, &source, NULL);
   puts(TAG "compile begin");
   glCompileShader(shader);
   glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
   glGetShaderInfoLog(shader, sizeof(log), NULL, log);
   printf(TAG "compile=%d log=%s\n", compiled, log);
   if (!check_gl("compile") || !compiled)
      goto cleanup;
   program = glCreateProgram();
   if (!check_gl("create program") || !program)
      goto cleanup;
   glAttachShader(program, shader);
   puts(TAG "link begin");
   glLinkProgram(program);
   glGetProgramiv(program, GL_LINK_STATUS, &linked);
   glGetProgramInfoLog(program, sizeof(log), NULL, log);
   printf(TAG "link=%d log=%s\n", linked, log);
   if (!check_gl("link") || !linked)
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
      glDeleteBuffers(2, buffers);
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
