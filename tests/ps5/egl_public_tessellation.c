// PS5 OpenGL - OpenGL implementation for PlayStation 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>
#include <stdio.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>

#define SIZE 64

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
      char log[1024] = {0};
      GLsizei length = 0;
      glGetShaderInfoLog(result, sizeof(log), &length, log);
      printf("[ps5-egl-tessellation] shader=%x log=%.*s\n",
             type, length, log);
      glDeleteShader(result);
      return 0;
   }
   return result;
}

static GLuint
program(const char *const *sources, const GLenum *types, unsigned count)
{
   GLuint shaders[4] = {0};
   GLuint result = glCreateProgram();
   GLint ok = GL_FALSE;

   for (unsigned i = 0; i < count; ++i) {
      shaders[i] = shader(types[i], sources[i]);
      if (!shaders[i])
         goto done;
      glAttachShader(result, shaders[i]);
   }
   glLinkProgram(result);
   glGetProgramiv(result, GL_LINK_STATUS, &ok);
   if (!ok) {
      char log[1024] = {0};
      GLsizei length = 0;
      glGetProgramInfoLog(result, sizeof(log), &length, log);
      printf("[ps5-egl-tessellation] link=%.*s\n", length, log);
   }
done:
   for (unsigned i = 0; i < count; ++i)
      glDeleteShader(shaders[i]);
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

int
main(void)
{
   static const char *vs =
      "#version 330 core\n"
      "void main(){"
      "float x=gl_VertexID==1?0.8:-0.8;"
      "float y=gl_VertexID==2?0.8:-0.8;"
      "gl_Position=vec4(x,y,0.5,1.0);}\n";
   static const char *tcs =
      "#version 330 core\n"
      "#extension GL_ARB_tessellation_shader : require\n"
      "layout(vertices=3) out;"
      "void main(){"
      "gl_out[gl_InvocationID].gl_Position=gl_in[gl_InvocationID].gl_Position;"
      "if(gl_InvocationID==0){"
      "gl_TessLevelOuter[0]=2.0;gl_TessLevelOuter[1]=2.0;"
      "gl_TessLevelOuter[2]=2.0;gl_TessLevelOuter[3]=2.0;"
      "gl_TessLevelInner[0]=2.0;gl_TessLevelInner[1]=2.0;}}\n";
   static const char *tes =
      "#version 330 core\n"
      "#extension GL_ARB_tessellation_shader : require\n"
      "layout(triangles,equal_spacing,ccw) in;"
      "void main(){gl_Position="
      "gl_in[0].gl_Position*gl_TessCoord.x+"
      "gl_in[1].gl_Position*gl_TessCoord.y+"
      "gl_in[2].gl_Position*gl_TessCoord.z;}\n";
   static const char *green =
      "#version 330 core\n"
      "out vec4 c;void main(){c=vec4(0,1,0,1);}\n";
   static const char *blue =
      "#version 330 core\n"
      "out vec4 c;void main(){c=vec4(0,0,1,1);}\n";
   const char *tess_sources[] = {vs, tcs, tes, green};
   const GLenum tess_types[] = {GL_VERTEX_SHADER, GL_TESS_CONTROL_SHADER,
                                GL_TESS_EVALUATION_SHADER,
                                GL_FRAGMENT_SHADER};
   const char *plain_sources[] = {vs, blue};
   const GLenum plain_types[] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
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
   GLuint programs[2] = {0};
   GLuint vao = 0;
   unsigned green_count = 0, blue_count = 0, draws = 0;
   int status = -1, passed = 0;

   display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL) ||
       !eglBindAPI(EGL_OPENGL_API) ||
       !eglChooseConfig(display, config_attrs, &config, 1, &count) ||
       count != 1)
      goto done;
   surface = eglCreateWindowSurface(display, config, 0, NULL);
   context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
   if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
       !eglMakeCurrent(display, surface, surface, context))
      goto done;
   programs[0] = program(tess_sources, tess_types, 4);
   programs[1] = program(plain_sources, plain_types, 2);
   if (!programs[0] || !programs[1])
      goto done;
   glGenVertexArrays(1, &vao);
   glBindVertexArray(vao);
   glViewport(0, 0, SIZE, SIZE);
   glPatchParameteri(GL_PATCH_VERTICES, 3);
   glUseProgram(programs[0]);
   glClearColor(0, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_PATCHES, 0, 3);
   green_count = matching(UINT32_C(0xff00ff00));
   glUseProgram(programs[1]);
   glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   blue_count = matching(UINT32_C(0xffff0000));
   status = ps5_egl_current_draw_status(&draws);
   passed = glGetError() == GL_NO_ERROR && status == 0 && draws == 4 &&
            green_count > 500 && blue_count > 500;

done:
   printf("[ps5-egl-tessellation] green=%u blue=%u draws=%u status=%d result=%s\n",
          green_count, blue_count, draws, status, passed ? "pass" : "fail");
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
