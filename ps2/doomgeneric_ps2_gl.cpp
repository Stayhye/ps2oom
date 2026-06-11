// doomgeneric PS2 backend -- ps2gl (hardware VU1+GS) video, MILESTONE 1.
//
// This is the foundation step for a GL world renderer. For now it does NOT
// render the world in GL yet: it brings ps2gl up inside the real game build
// (alongside libSDL2main's entry, audsrv, the game loop and the boot console)
// expands Doom's 8-bit (CMAP256) framebuffer through the palette into an RGBA
// texture and draws it on a fullscreen quad. That proves the integration --
// C++ linking, ps2gl init
// without the glut main loop, and coexistence with our audio -- before the
// real renderer (walls/flats/sprites as GL geometry) is built on top.
//
// We reuse glutInit() only for the GS / ps2gl bring-up (GIF reset, SetGsCrt,
// pglInit, GS-memory layout); we keep our own loop and don't call
// glutMainLoop(). Input is intentionally not wired here yet (demo runs without
// it) so glut's pad manager doesn't fight ps2_pad.c.

#include <stdio.h>
#include <string.h>

#include <SDL.h>            // libSDL2main entry + SDL timer
#include <kernel.h>         // FlushCache
#include "GL/gl.h"
#include "GL/glut.h"
#include "GL/ps2gl.h"

extern "C" {
#include "doomkeys.h"
#include "doomgeneric.h"
}

// Doom's palette (CMAP256), from i_video.c.
struct color { uint32_t b:8, g:8, r:8, a:8; };
extern "C" struct color colors[256];

extern "C" void BootScr_Begin(void);
extern "C" void BootScr_End(void);
extern "C" void PS2Audio_Init(void);
extern "C" void I_ResetBaseTime(void);
extern "C" volatile int g_audio_gate;   // i_audsrvsound.c
extern "C" void RGL_Init(void);         // r_gl.c -- one-time GL state + projection
extern "C" void RGL_DrawWorld(void);    // r_gl.c -- M2 world geometry

#ifndef BOOT_LOG_HOLD_MS
#define BOOT_LOG_HOLD_MS 3000
#endif
#define DISP_W 640
#define DISP_H 448

// The GS wants power-of-two textures, so the 320x200 framebuffer is expanded
// into the top-left of a 512x256 RGBA32 texture and the quad samples that region.
#define TEX_W 512
#define TEX_H 256

static int           gl_ready = 0;
static int           g_argc = 0;
static char        **g_argv = 0;
static unsigned int *gl_tex;             // 512x256 RGBA32 framebuffer (DMA mem)
static GLuint        gl_texid;

static void EnsureGl(void)
{
    if (gl_ready)
        return;

    SDL_InitSubSystem(SDL_INIT_TIMER);

    printf("\n>>> build %s %s  (ps2gl video M1, %dx%d) <<<\n",
           __DATE__, __TIME__, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    SDL_Delay(BOOT_LOG_HOLD_MS);
    I_ResetBaseTime();
    BootScr_End();

    // Bring up ps2gl + the GS (glutInit does GIF reset, SetGsCrt, pglInit and
    // the GS-memory layout). We do NOT call glutMainLoop -- our loop drives.
    glutInit(&g_argc, g_argv);

    gl_tex = (unsigned int *) pglutAllocDmaMem(TEX_W * TEX_H * 4);
    memset(gl_tex, 0, TEX_W * TEX_H * 4);
    glGenTextures(1, &gl_texid);
    glBindTexture(GL_TEXTURE_2D, gl_texid);
    // RGBA32, not paletted: the CPU expands Doom's 8-bit framebuffer through the
    // palette each frame (see DG_DrawFrame). This sidesteps every paletted-texture
    // pitfall on ps2gl -- per-frame CLUT upload, the CSM1 CLUT swizzle, and glut's
    // tiny 2-page PSMT8 slot (which silently promotes an 8-bit image into a 32-bit
    // slot) -- and a 512x256 RGBA area is exactly 64 GS pages = one of glut's
    // 64-page kPsm32 texture slots. RGBA also lets us bilinear-filter safely (no
    // index interpolation), for a smooth upscale.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // One-time GL render state + perspective projection for the world renderer.
    // MUST be done here, outside the per-frame pglBeginGeometry/EndGeometry
    // block -- enabling depth test or setting the projection inside that block
    // hangs the GS (it reselects the VU1 renderer mid-packet). See r_gl.c.
    RGL_Init();

    gl_ready = 1;
    g_audio_gate = 1;   // GS now shows the game -> start audio in sync
}

extern "C" void DG_Init(void) { /* ps2gl is brought up lazily in EnsureGl */ }

extern "C" void DG_DrawFrame(void)
{
    static int first = 1;

    EnsureGl();

    // ps2gl frame protocol (the same sequence glutMainLoop runs internally):
    // wait for the previous frame's render to finish before reusing its packet,
    // record this frame's geometry, then flip and kick the render.
    if (!first) pglFinishRenderingGeometry(PGL_DONT_FORCE_IMMEDIATE_STOP);
    else        first = 0;

    pglBeginGeometry();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // M2.1: render Doom's world as GL geometry (walls only, untextured) from the
    // viewpoint the software R_SetupFrame already computed this frame. The 8-bit
    // software framebuffer (DG_ScreenBuffer) is ignored for now; the 2D UI gets
    // composited back on top at M2.7. (The M1 framebuffer blit -- expand to RGBA
    // + textured quad -- lives in git history and will return for that overlay.)
    RGL_DrawWorld();

    pglEndGeometry();

    pglWaitForVSync();
    pglSwapBuffers();
    pglRenderGeometry();
}

extern "C" void DG_SleepMs(uint32_t ms)   { SDL_Delay(ms); }
extern "C" uint32_t DG_GetTicksMs(void)   { return SDL_GetTicks(); }
extern "C" void DG_SetWindowTitle(const char *t) { (void) t; }
extern "C" int  DG_GetKey(int *pressed, unsigned char *key) { (void)pressed; (void)key; return 0; }  // M1: no input yet

extern "C" int main(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;

    BootScr_Begin();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("\n=== doomgeneric for PlayStation 2 (ps2gl video, M1) ===\n");

    PS2Audio_Init();
    doomgeneric_Create(argc, argv);

    for (;;)
        doomgeneric_Tick();

    return 0;
}
