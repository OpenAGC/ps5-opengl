# OpenGL 4.6 showcase

The project's demo app. Every SDK release attaches it, built against that SDK, as
`ps5-opengl-showcase-<version>-PPSA99005.zip`: extract it and upload the
`PPSA99005` folder to `/data/homebrew/`. It appears on the home screen as
**PS5 OpenGL Showcase** with its own icon, backgrounds and selection music
(`sce_sys/` in this directory).

A GPU-driven scene that exercises the modern OpenGL 4.6 feature set at
**3840x2160 and 120 FPS** on a 120 Hz display (119.9 FPS measured on PS5 with
SDK 0.5.0), using only public OpenGL/EGL interfaces:

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
- **Post-processing:** an RGBA16F HDR target, a bloom chain of fullscreen passes
  (prefilter, 6 downsample and tent upsample levels), ACES tone mapping, vignette
  and film grain.
- **HUD:** resolution, live frame rate and feature list drawn in the composite
  shader from a 5x7 bitmap-font texture.
- **Display modes:** asks for 4K at 120 Hz with `eglSetDisplayModePS5` and
  `eglSetDisplayRefreshPS5` when the SDK provides them (0.5.0 and later), and
  shows the accepted refresh rate in the HUD. A display without 120 Hz runs at 60.

Everything is created with direct state access (`glCreate*`, `glNamed*`).

## Reaching 4K120

- **Declare high frame rate.** The title's `param.json` carries the `attribute3`
  bits `0x80040`; without them the console refuses 120 Hz output and the driver
  stays at 60 Hz. The example packaging sets them for runtime display-mode SDKs.
- **Keep per-frame work on the graphics path.** Each compute dispatch currently
  runs synchronously (about 1 ms of CPU each), so the showcase uses two per frame
  (particles and culling) and draws bloom with fullscreen passes. Moving the
  11-pass bloom from compute to fragment shaders took it from 48 to 60+ FPS.

The log prints the CPU time per stage every two seconds, for example
`cpu-ms compute=0.51 scene=4.55 bloom=0.35 composite=2.41 present=0.51`.

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
