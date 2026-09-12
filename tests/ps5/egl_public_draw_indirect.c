// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>

#define SIZE 64

#ifdef PS5_GLSL_400_TEST
#define TEST_NAME "ps5-egl-glsl400"
#define GLSL_VERSION "#version 400 core\n"
#define VERTEX_PROBE "uint bias=bitfieldExtract(0x38u,3,3)-4u;"
#define VERTEX_ID "gl_VertexID-int(bias)"
#define GREEN_BODY "c=bitCount(15u)==4?vec4(0,1,0,1):vec4(1,0,0,1);"
#define CYAN_BODY "c=findLSB(8u)==3?vec4(0,1,1,1):vec4(1,0,0,1);"
#else
#define TEST_NAME "ps5-egl-draw-indirect"
#define GLSL_VERSION "#version 330 core\n"
#define VERTEX_PROBE ""
#define VERTEX_ID "gl_VertexID-3"
#define GREEN_BODY "c=vec4(0,1,0,1);"
#define CYAN_BODY "c=vec4(0,1,1,1);"
#endif

int ps5_egl_current_draw_status(unsigned *draw_calls);

static GLuint
shader(GLenum type, const char *source)
{
   GLint ok = GL_FALSE;
   GLuint result = glCreateShader(type);

   glShaderSource(result, 1, &source, NULL);
   glCompileShader(result);
   glGetShaderiv(result, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[512] = {0};
      GLsizei length = 0;
      glGetShaderInfoLog(result, sizeof(log), &length, log);
      printf("[%s] shader=%x log=%.*s\n", TEST_NAME, type, length, log);
      glDeleteShader(result);
      return 0;
   }
   return result;
}

static GLuint
program(const char *fragment)
{
   static const char *vertex =
      GLSL_VERSION
      "void main(){" VERTEX_PROBE "int id=" VERTEX_ID ";"
      "float x=id==1?0.8:-0.8;float y=id==2?0.8:-0.8;"
      "gl_Position=vec4(x,y,0.5,1.0);}\n";
   GLuint shaders[2] = {shader(GL_VERTEX_SHADER, vertex),
                        shader(GL_FRAGMENT_SHADER, fragment)};
   GLuint result = glCreateProgram();
   GLint ok = GL_FALSE;

   if (!shaders[0] || !shaders[1])
      goto done;
   glAttachShader(result, shaders[0]);
   glAttachShader(result, shaders[1]);
   glLinkProgram(result);
   glGetProgramiv(result, GL_LINK_STATUS, &ok);
done:
   glDeleteShader(shaders[0]);
   glDeleteShader(shaders[1]);
   if (!ok) {
      glDeleteProgram(result);
      result = 0;
   }
   return result;
}

static unsigned
matching(uint32_t color)
{
   static uint32_t pixels[SIZE * SIZE];
   unsigned result = 0;

   glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
   for (unsigned i = 0; i < SIZE * SIZE; ++i)
      result += pixels[i] == color;
   return result;
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
   return 0;
}

int
main(void)
{
   static const char *green =
      GLSL_VERSION "out vec4 c;void main(){" GREEN_BODY "}\n";
   static const char *cyan =
      GLSL_VERSION "out vec4 c;void main(){" CYAN_BODY "}\n";
   static const uint32_t commands[] = {
      3, 1, 3, 0,
      3, 1, 0, 3, 0,
   };
   static const uint16_t indices[] = {0, 1, 2};
   const EGLint config_attrs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   const EGLint context_attrs[] = {
      EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
      EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
      EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE,
   };
   EGLDisplay display = EGL_NO_DISPLAY;
   EGLConfig config = NULL;
   EGLSurface surface = EGL_NO_SURFACE;
   EGLContext context = EGL_NO_CONTEXT;
   EGLint count = 0;
   GLuint programs[2] = {0}, vao = 0, buffers[2] = {0};
   unsigned green_count = 0, cyan_count = 0, draws = 0;
   const char *glsl = NULL;
   int draw_indirect = 0, gpu_shader5 = 0;
   int status = -1, passed = 0;

   display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL) ||
       !eglBindAPI(EGL_OPENGL_API) ||
       !eglChooseConfig(display, config_attrs, &config, 1, &count) || count != 1)
      goto done;
   surface = eglCreateWindowSurface(display, config, 0, NULL);
   context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, surface, surface, context))
      goto done;
   glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
   draw_indirect = has_extension("GL_ARB_draw_indirect");
   gpu_shader5 = has_extension("GL_ARB_gpu_shader5");
   printf("[%s] glsl=%s indirect=%d gpu-shader5=%d\n", TEST_NAME,
          glsl ? glsl : "(null)", draw_indirect, gpu_shader5);
   if (!draw_indirect
#ifdef PS5_GLSL_400_TEST
       || !gpu_shader5 || !glsl || strncmp(glsl, "4.00", 4)
#endif
       )
      goto done;
   programs[0] = program(green);
   programs[1] = program(cyan);
   if (!programs[0] || !programs[1])
      goto done;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glGenBuffers(2, buffers);
   glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffers[0]);
   glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(commands), commands,
                GL_STATIC_DRAW);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
                GL_STATIC_DRAW);
   glViewport(0, 0, SIZE, SIZE);
   glClearColor(0, 0, 0, 1);
   glUseProgram(programs[0]);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArraysIndirect(GL_TRIANGLES, 0);
   green_count = matching(UINT32_C(0xff00ff00));
   glUseProgram(programs[1]);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
                          (const void *)(uintptr_t)16);
   cyan_count = matching(UINT32_C(0xffffff00));
   status = ps5_egl_current_draw_status(&draws);
   passed = glGetError() == GL_NO_ERROR && status == 0 && draws == 4 &&
            green_count > 500 && cyan_count > 500;

done:
   printf("[%s] arrays=%u indexed=%u draws=%u status=%d result=%s\n",
          TEST_NAME, green_count, cyan_count, draws, status,
          passed ? "pass" : "fail");
   glDeleteBuffers(2, buffers);
   glDeleteVertexArrays(1, &vao);
   glDeleteProgram(programs[0]);
   glDeleteProgram(programs[1]);
   if (display != EGL_NO_DISPLAY)
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   if (context != EGL_NO_CONTEXT)
      eglDestroyContext(display, context);
   if (surface != EGL_NO_SURFACE)
      eglDestroySurface(display, surface);
   if (display != EGL_NO_DISPLAY)
      eglTerminate(display);
   return passed ? 0 : 1;
}
