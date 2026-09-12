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

#if defined(PS5_FP64_VERTEX_TEST)
#define TEST_NAME "ps5-egl-fp64-vertex"
#define GLSL_VERSION "#version 410 core\n"
#define VERTEX_PROBE ""
#define VERTEX_ID "gl_VertexID-3"
#define GREEN_BODY "c=vec4(0,1,0,1);"
#define CYAN_BODY "c=vec4(0,1,1,1);"
#elif defined(PS5_FP64_TEST)
#define TEST_NAME "ps5-egl-fp64"
#define GLSL_VERSION "#version 330 core\n#extension GL_ARB_gpu_shader_fp64 : require\n"
#define VERTEX_PROBE "double bias64=double(gl_VertexID)-3.0lf;"
#define VERTEX_ID "int(bias64)"
#define GREEN_BODY "double x=double(gl_FragCoord.x)+1.0lf;c=abs((x/2.0lf)*2.0lf-x)<0.0001lf?vec4(0,1,0,1):vec4(1,0,0,1);"
#define CYAN_BODY "double y=double(gl_FragCoord.y)+1.0lf;c=abs(sqrt(y*y)-y)<0.0001lf?vec4(0,1,1,1):vec4(1,0,0,1);"
#elif defined(PS5_VIEWPORT_ARRAY_TEST)
#define TEST_NAME "ps5-egl-viewport-array"
#define GLSL_VERSION "#version 400 core\n"
#define VERTEX_PROBE ""
#define VERTEX_ID "gl_VertexID-3"
#define GREEN_BODY "c=vec4(0,1,0,1);"
#define CYAN_BODY "c=vec4(0,1,1,1);"
#elif defined(PS5_GLSL_400_TEST)
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
#ifdef PS5_FP64_VERTEX_TEST
      GLSL_VERSION
      "layout(location=0) in dvec2 p;void main(){"
      "dvec2 q=p+dvec2(0.125lf)-dvec2(0.125lf);"
      "gl_Position=vec4(vec2(q),0.5,1.0);}\n";
#else
      GLSL_VERSION
      "void main(){" VERTEX_PROBE "int id=" VERTEX_ID ";"
      "float x=id==1?0.8:-0.8;float y=id==2?0.8:-0.8;"
      "gl_Position=vec4(x,y,0.5,1.0);}\n";
#endif
   GLuint shaders[3] = {shader(GL_VERTEX_SHADER, vertex),
                        shader(GL_FRAGMENT_SHADER, fragment), 0};
#ifdef PS5_VIEWPORT_ARRAY_TEST
   static const char *geometry =
      GLSL_VERSION
      "#extension GL_ARB_viewport_array : require\n"
      "layout(triangles) in;layout(triangle_strip,max_vertices=3) out;"
      "void main(){gl_ViewportIndex=1;for(int i=0;i<3;++i){"
      "gl_Position=gl_in[i].gl_Position;EmitVertex();}EndPrimitive();}\n";
   shaders[2] = shader(GL_GEOMETRY_SHADER, geometry);
#endif
   GLuint result = glCreateProgram();
   GLint ok = GL_FALSE;

   if (!shaders[0] || !shaders[1]
#ifdef PS5_VIEWPORT_ARRAY_TEST
       || !shaders[2]
#endif
       )
      goto done;
   glAttachShader(result, shaders[0]);
   glAttachShader(result, shaders[1]);
#ifdef PS5_VIEWPORT_ARRAY_TEST
   glAttachShader(result, shaders[2]);
#endif
   glLinkProgram(result);
   glGetProgramiv(result, GL_LINK_STATUS, &ok);
done:
   glDeleteShader(shaders[0]);
   glDeleteShader(shaders[1]);
   glDeleteShader(shaders[2]);
   if (!ok) {
      glDeleteProgram(result);
      result = 0;
   }
   return result;
}

static unsigned
matching(unsigned x, uint32_t color)
{
   static uint32_t pixels[SIZE * SIZE];
   unsigned result = 0;

   glReadPixels(x, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
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
#ifdef PS5_FP64_VERTEX_TEST
   static const GLdouble vertices[12] = {
      -0.8, -0.8, 0.8, -0.8, -0.8, 0.8,
      -0.8, -0.8, 0.8, -0.8, -0.8, 0.8,
   };
#endif
   const EGLint config_attrs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   const EGLint context_attrs[] = {
#if defined(PS5_GLSL_400_TEST) || defined(PS5_VIEWPORT_ARRAY_TEST) || \
    defined(PS5_FP64_VERTEX_TEST)
#ifdef PS5_FP64_VERTEX_TEST
      EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 1,
#else
      EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 0,
#endif
#else
      EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
#endif
      EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
      EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE,
   };
   EGLDisplay display = EGL_NO_DISPLAY;
   EGLConfig config = NULL;
   EGLSurface surface = EGL_NO_SURFACE;
   EGLContext context = EGL_NO_CONTEXT;
   EGLint count = 0;
   GLuint programs[2] = {0}, vao = 0, buffers[3] = {0};
   unsigned green_count = 0, cyan_count = 0, draws = 0;
#ifdef PS5_VIEWPORT_ARRAY_TEST
   unsigned green_left = 0, cyan_left = 0;
#endif
   const char *glsl = NULL;
   int draw_indirect = 0, gpu_shader5 = 0;
#ifdef PS5_FP64_VERTEX_TEST
   GLint max_texture = 0, max_renderbuffer = 0, max_cube = 0;
   GLint max_3d = 0, max_layers = 0;
#endif
#ifdef PS5_VIEWPORT_ARRAY_TEST
   int viewport_array = 0;
#endif
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
#ifdef PS5_FP64_VERTEX_TEST
   glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
   glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_renderbuffer);
   glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &max_cube);
   glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max_3d);
   glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
   printf("[%s] limits texture=%d renderbuffer=%d cube=%d 3d=%d layers=%d\n",
          TEST_NAME, max_texture, max_renderbuffer, max_cube, max_3d,
          max_layers);
#endif
#ifdef PS5_VIEWPORT_ARRAY_TEST
   viewport_array = has_extension("GL_ARB_viewport_array");
#endif
   printf("[%s] glsl=%s indirect=%d gpu-shader5=%d\n", TEST_NAME,
          glsl ? glsl : "(null)", draw_indirect, gpu_shader5);
   if (!draw_indirect
#ifdef PS5_GLSL_400_TEST
       || !gpu_shader5 || !glsl || strncmp(glsl, "4.00", 4)
#endif
#ifdef PS5_FP64_TEST
       || !has_extension("GL_ARB_gpu_shader_fp64")
#endif
#ifdef PS5_FP64_VERTEX_TEST
       || !glsl || strncmp(glsl, "4.10", 4) ||
          max_texture < 16384 || max_renderbuffer < 16384 ||
          max_cube < 16384 || max_3d < 2048 || max_layers < 2048 ||
          !has_extension("GL_ARB_gpu_shader_fp64") ||
          !has_extension("GL_ARB_vertex_attrib_64bit")
#endif
#ifdef PS5_VIEWPORT_ARRAY_TEST
       || !viewport_array
#endif
       )
      goto done;
   programs[0] = program(green);
   programs[1] = program(cyan);
   if (!programs[0] || !programs[1])
      goto done;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glGenBuffers(3, buffers);
   glBindBuffer(GL_DRAW_INDIRECT_BUFFER, buffers[0]);
   glBufferData(GL_DRAW_INDIRECT_BUFFER, sizeof(commands), commands,
                GL_STATIC_DRAW);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[1]);
   glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices,
                GL_STATIC_DRAW);
#ifdef PS5_FP64_VERTEX_TEST
   glBindBuffer(GL_ARRAY_BUFFER, buffers[2]);
   glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
   glVertexAttribLPointer(0, 2, GL_DOUBLE, 0, NULL);
   glEnableVertexAttribArray(0);
#endif
#ifdef PS5_VIEWPORT_ARRAY_TEST
   {
      const GLfloat viewports[8] = {
         0, 0, SIZE, SIZE, SIZE, 0, SIZE, SIZE,
      };
      const GLint scissors[8] = {
         0, 0, SIZE, SIZE, SIZE, 0, SIZE, SIZE,
      };
      glViewportArrayv(0, 2, viewports);
      glScissorArrayv(0, 2, scissors);
      glEnable(GL_SCISSOR_TEST);
   }
#else
   glViewport(0, 0, SIZE, SIZE);
#endif
   glClearColor(0, 0, 0, 1);
   glUseProgram(programs[0]);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArraysIndirect(GL_TRIANGLES, 0);
#ifdef PS5_VIEWPORT_ARRAY_TEST
   green_left = matching(0, UINT32_C(0xff00ff00));
   green_count = matching(SIZE, UINT32_C(0xff00ff00));
#else
   green_count = matching(0, UINT32_C(0xff00ff00));
#endif
   glUseProgram(programs[1]);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_SHORT,
                          (const void *)(uintptr_t)16);
#ifdef PS5_VIEWPORT_ARRAY_TEST
   cyan_left = matching(0, UINT32_C(0xffffff00));
   cyan_count = matching(SIZE, UINT32_C(0xffffff00));
#else
   cyan_count = matching(0, UINT32_C(0xffffff00));
#endif
   status = ps5_egl_current_draw_status(&draws);
   passed = glGetError() == GL_NO_ERROR && status == 0 && draws ==
#ifdef PS5_VIEWPORT_ARRAY_TEST
            2 && !green_left && !cyan_left &&
#else
            4 &&
#endif
            green_count >
#ifdef PS5_VIEWPORT_ARRAY_TEST
               500 && cyan_count > 500;
#else
               500 && cyan_count > 500;
#endif

done:
#ifdef PS5_VIEWPORT_ARRAY_TEST
   printf("[%s] left=%u/%u right=%u/%u\n", TEST_NAME, green_left,
          cyan_left, green_count, cyan_count);
#endif
   printf("[%s] arrays=%u indexed=%u draws=%u status=%d result=%s\n",
          TEST_NAME, green_count, cyan_count, draws, status,
          passed ? "pass" : "fail");
   glDeleteBuffers(3, buffers);
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
