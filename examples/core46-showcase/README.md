# OpenGL 4.6 showcase

A GPU-driven scene that exercises the modern OpenGL 4.6 feature set at up to
3840x2160, using only public OpenGL/EGL interfaces:

- **Compute particles:** 262,144 particles in a shader storage buffer, advected by
  a flow field in a compute shader and respawned along a glowing torus knot, drawn
  as vertex-pulled camera-facing quads with additive HDR blending.
- **GPU-driven instancing:** a compute pass animates 8,192 gems and chrome cubes,
  culls them against the view frustum and appends survivors to two indirect
  commands with atomics; one `glMultiDrawElementsIndirectCount` draws both, and
  the vertex shader picks each material with `gl_DrawID` and `gl_BaseInstance`.
- **Lighting:** GGX physically based shading with three orbiting coloured lights,
  an iridescent knot with light pulses, faceted gems from screen-space derivatives
  and a procedural aurora sky with stars.
- **Texturing:** a mipmapped floor grid sampled with 16x anisotropic filtering
  (`GL_TEXTURE_MAX_ANISOTROPY`, core in 4.6).
- **Post-processing:** an RGBA16F HDR target, a compute-shader bloom chain
  (image load/store: prefilter, 6 downsample and tent upsample passes), ACES tone
  mapping, vignette and film grain.
- **HUD:** resolution, live frame rate and feature list drawn in the composite
  shader from a 5x7 bitmap-font texture.
- **Display modes:** asks for 4K with `eglSetDisplayModePS5` when the SDK
  provides it (0.5.0 and later), otherwise uses the SDK's display size.

Everything is created with direct state access (`glCreate*`, `glNamed*`).

## Build

With a downloaded SDK 0.5.0 (or `make sdk-gl46` first):

```sh
PS5_OPENGL_PREFIX=/path/to/ps5-opengl-sdk-0.5.0/sdk make showcase
```

Deploy `build/native-app/PPSA99005/dist/PPSA99005` and launch it as a registered
title. It runs until closed and logs `[ps5-gl46-showcase] ... fps=...` every five
seconds. Compile-time options: `SHOWCASE_WIDTH`/`SHOWCASE_HEIGHT` (display mode),
`SHOWCASE_PARTICLES` and `SHOWCASE_SECONDS` (0 runs until closed).

## Host preview

The same source renders off-screen with desktop Mesa, which is how the scene was
tuned:

```sh
cc -std=c11 -O2 -DSHOWCASE_HOST_PREVIEW=120 -DSHOWCASE_WIDTH=1920 -DSHOWCASE_HEIGHT=1080 \
   examples/core46-showcase/main.c -o showcase -lEGL -lGL -lm
EGL_PLATFORM=surfaceless MESA_GL_VERSION_OVERRIDE=4.6 ./showcase   # writes showcase.ppm
```
